// ABI-01 / PARSE-01 部分：feed_result 语义、事件释放、EOF、超大 OSC。
#include "itcore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make() {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 1000, 0};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
struct Ev { itcore_event_kind k; std::string d; };
static std::vector<Ev> drain(itcore_core *c) {
    std::vector<Ev> v;
    while (itcore_event *e = itcore_next_event(c)) {
        size_t n; const uint8_t *p = itcore_event_data(e, &n);
        v.push_back({itcore_event_type(e), std::string(reinterpret_cast<const char *>(p), n)});
        itcore_event_release(e);
    }
    return v;
}
static itcore_feed_result feed(itcore_core *c, const std::string &s) {
    return itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

static void test_basic_events() {
    itcore_core *c = make();
    auto r = feed(c, "hi\x07\x1b]2;ti\x01tle\x07\x1b[5n\x1b]52;c;aGk=\x07");
    CHECK(r.status == ITCORE_OK && r.accepted == 30);
    auto v = drain(c);
    bool bell = false, title = false, reply = false, clip = false; int inval = 0;
    for (auto &e : v) {
        bell |= e.k == ITCORE_EV_BELL;
        title |= e.k == ITCORE_EV_TITLE && e.d == "title";
        reply |= e.k == ITCORE_EV_REPLY_BYTES && e.d == "\x1b[0n";
        clip |= e.k == ITCORE_EV_CLIPBOARD_REQUEST && e.d == "c";
        inval += e.k == ITCORE_EV_RENDER_INVALIDATED;
    }
    CHECK(bell && title && reply && clip);
    CHECK(inval == 1);  // 合并
    CHECK(itcore_feed(c, nullptr, 0).status == ITCORE_OK);
    itcore_destroy(c);
}

static void test_title_cap() {
    itcore_core *c = make();
    std::string t; for (int i = 0; i < 300; ++i) t += "中";
    feed(c, "\x1b]0;" + t + "\x07");
    auto v = drain(c);
    CHECK(v.size() == 1 && v[0].k == ITCORE_EV_TITLE && v[0].d.size() == 256 * 3);
    itcore_destroy(c);
}

static void test_need_drain() {
    itcore_core *c = make();
    std::string bells(5000, '\x07');
    size_t off = 0; int drains = 0; size_t total = 0;
    while (off < bells.size()) {
        auto r = itcore_feed(c, reinterpret_cast<const uint8_t *>(bells.data()) + off, bells.size() - off);
        CHECK(r.status != ITCORE_FATAL);
        if (r.accepted == 0) CHECK(r.status == ITCORE_NEED_DRAIN);
        off += r.accepted;
        if (r.status == ITCORE_NEED_DRAIN) { ++drains; CHECK(r.accepted > 0 || drains > 0); }
        total += drain(c).size();
        if (drains > 100) break;
    }
    CHECK(drains >= 4);
    CHECK(total == 5000);  // 无丢失、无重复
    // 不排空时 accepted=0 必须伴随 NEED_DRAIN
    for (int i = 0; i < 2; ++i) feed(c, std::string(2000, '\x07'));
    auto r = feed(c, "\x07");
    CHECK(r.accepted == 0 && r.status == ITCORE_NEED_DRAIN);
    itcore_destroy(c);  // 未取出的事件由 destroy 释放（ASan/valgrind 检查）
}

static void test_finish() {
    itcore_core *c = make();
    feed(c, "a\xe4\xb8");
    drain(c);
    CHECK(itcore_finish_input(c) == 0);
    auto v = drain(c);
    CHECK(v.size() == 1 && v[0].k == ITCORE_EV_RENDER_INVALIDATED);  // U+FFFD 进入屏幕
    CHECK(itcore_incomplete_count(c) == 1);
    CHECK(itcore_finish_input(c) == 0);
    CHECK(drain(c).empty() && itcore_incomplete_count(c) == 1);  // 重复调用无副作用
    auto r = feed(c, "x");
    CHECK(r.status == ITCORE_FATAL && r.accepted == 0);
    itcore_destroy(c);

    c = make();
    feed(c, "\x1b[12;");
    itcore_finish_input(c);
    CHECK(itcore_incomplete_count(c) == 1);
    itcore_destroy(c);
}

static void test_bad_args() {
    itcore_core *c = nullptr;
    itcore_options o{ITCORE_ABI_VERSION + 1, sizeof(itcore_options), 80, 24, 0, 0};
    CHECK(itcore_create(&o, &c) != 0 && !c);
    CHECK(itcore_feed(nullptr, nullptr, 0).status == ITCORE_FATAL);
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "big_osc") == 0) {
        // 1.5 MiB OSC：截断但可恢复，后续 BEL 正常
        itcore_core *c = make();
        std::string s = "\x1b]2;" + std::string(1536 * 1024, 'x') + "\x07\x07";
        auto r = feed(c, s);
        CHECK(r.status == ITCORE_OK && r.accepted == s.size());
        auto v = drain(c);
        CHECK(v.size() == 2 && v[0].k == ITCORE_EV_TITLE && v[0].d.size() == 256 && v[1].k == ITCORE_EV_BELL);
        itcore_destroy(c);
        // 超过 1 MiB + 1 MiB 丢弃预算：FATAL，之后 feed 失败
        c = make();
        std::string big = "\x1b]2;" + std::string((2u << 20) + 16, 'y');
        size_t off = 0; itcore_status st = ITCORE_OK;
        while (off < big.size() && st != ITCORE_FATAL) {  // 64 KiB 分块，模拟管道读
            size_t n = std::min<size_t>(65536, big.size() - off);
            auto rr = itcore_feed(c, reinterpret_cast<const uint8_t *>(big.data()) + off, n);
            off += rr.accepted; st = rr.status;
        }
        CHECK(st == ITCORE_FATAL);
        CHECK(off == 2 + (2u << 20) + 1);  // ESC ] + 1 MiB 负载 + 1 MiB 丢弃预算 + 1
        CHECK(feed(c, "a").status == ITCORE_FATAL);
        CHECK(itcore_finish_input(c) == 0);
        itcore_destroy(c);
    } else {
        test_basic_events(); test_title_cap(); test_need_drain(); test_finish(); test_bad_args();
    }
    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
