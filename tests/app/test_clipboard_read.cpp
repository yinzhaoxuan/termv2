#undef NDEBUG
#include "pane_logic.h"
#include <cassert>
#include <string>
#include <string_view>

using it2app::ClipboardReadGate;
using Decision = uishell::ClipboardReadDecision;
using Action = ClipboardReadGate::Action;

static void requests() {
    ClipboardReadGate gate;
    uint64_t id = 99;
    assert(gate.onRequest(false, &id) == Action::Drop && id == 0 && !gate.pending());
    assert(!gate.onAnswer(0, Decision::AllowSession));
    assert(gate.onRequest(true, &id) == Action::Ask && id != 0 && gate.pending());
    uint64_t other = 99;
    assert(gate.onRequest(true, &other) == Action::Busy && other == 0 && gate.pending());
    assert(gate.onRequest(false, &other) == Action::Drop && gate.pending());
    assert(!gate.onAnswer(id + 1, Decision::DenySession) && gate.pending());
    assert(gate.onAnswer(id, Decision::AllowOnce) && !gate.pending());
    assert(!gate.onAnswer(id, Decision::AllowSession));
    const auto first = id;
    assert(gate.onRequest(true, &id) == Action::Ask && id > first);
    assert(!gate.onAnswer(id, Decision::DenyOnce) && !gate.pending());
    assert(!gate.onAnswer(id, Decision::AllowOnce));
    assert(gate.onRequest(true, &id) == Action::Ask);  // 一次拒绝不留记忆
}

static void sessions() {
    for (auto decision : {Decision::AllowSession, Decision::DenySession}) {
        ClipboardReadGate gate;
        uint64_t id = 0;
        const bool allowed = decision == Decision::AllowSession;
        assert(gate.onRequest(true, &id) == Action::Ask);
        assert(gate.onAnswer(id, decision) == allowed && !gate.pending());
        assert(!gate.onAnswer(id, Decision::AllowOnce));
        for (int i = 0; i < 3; ++i) {
            assert(gate.onRequest(true, &id) == (allowed ? Action::Deliver : Action::Drop));
            assert(id == 0 && !gate.pending());
            assert(gate.onRequest(false, &id) == Action::Drop);  // 无回调永远拒绝
        }
    }
}

static void resets() {
    ClipboardReadGate gate;
    uint64_t old = 0, current = 0;
    assert(gate.onRequest(true, &old) == Action::Ask);
    gate.reset();
    assert(!gate.pending() && !gate.onAnswer(old, Decision::AllowSession));
    assert(gate.onRequest(true, &current) == Action::Ask && current > old);
    assert(!gate.onAnswer(old, Decision::DenySession) && gate.pending());
    assert(gate.onAnswer(current, Decision::AllowSession));
    gate.reset();
    assert(gate.onRequest(true, &current) == Action::Ask);  // 清允许记忆
    assert(!gate.onAnswer(current, Decision::DenySession));
    gate.reset();
    assert(gate.onRequest(true, &current) == Action::Ask);  // 清拒绝记忆
    assert(!gate.onAnswer(current, Decision::DenyOnce));
    gate.reset();
    assert(!gate.pending());
    assert(gate.onRequest(false, &current) == Action::Drop);
}

// 真实核心：请求事件保留跨多次 feed；答复继续使用原选择参数和 BEL / ST。
static void delayedReply(std::string_view ending) {
    itcore_options options{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 100, 1u << 20};
    itcore_core *core = nullptr;
    assert(itcore_create(&options, &core) == 0 && core);
    assert(it2app::applyOsc52(core, {false, true, 0}) == 0);
    auto feed = [&](std::string_view text) {
        auto result = itcore_feed(core, reinterpret_cast<const uint8_t *>(text.data()), text.size());
        assert(result.status == ITCORE_OK && result.accepted == text.size());
    };
    feed(std::string("\x1b]52;c;?") + std::string(ending));
    itcore_event *request = nullptr;
    while (auto *event = itcore_next_event(core)) {
        if (itcore_event_type(event) == ITCORE_EV_CLIPBOARD_READ) {
            assert(!request);
            request = event;
        } else itcore_event_release(event);
    }
    assert(request);
    for (auto text : {"普通输出\r\n", "another line\r\n", "\x1b[31mred\x1b[0m"}) {
        feed(text);
        while (auto *event = itcore_next_event(core)) itcore_event_release(event);
    }
    std::string reply;
    auto send = [](void *ctx, const char *data, size_t n) -> int {
        static_cast<std::string *>(ctx)->append(data, n);
        return 0;
    };
    assert(it2app::deliverOsc52ReadReply(core, request, "hello", 5, send, &reply, nullptr, nullptr)
           == it2app::Osc52ReadDeliver::Ok);
    assert(reply == std::string("\x1b]52;c;aGVsbG8=") + std::string(ending));
    itcore_event_release(request);
    itcore_destroy(core);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "requests") requests();
    else if (mode == "sessions") sessions();
    else if (mode == "reset") resets();
    else if (mode == "bel") delayedReply("\a");
    else if (mode == "st") delayedReply("\x1b\\");
    else return 1;
}
