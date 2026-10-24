// pump() → present() 的平台无关核心：Win32PaneBackend 与 Linux 端到端测试走同一段代码。
#pragma once
#include "pane_logic.h"
#include "itcore.h"
#include <atomic>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

namespace it2app {

// Fed：本次喂过数据且已取空；FedMore：预算用尽、可能还有数据，须安排续跑（见 nextAfterPump）
enum class PumpResult { Idle, Fed, FedMore, Fatal };

// 每次 pump 的预算：喂满 maxBytes 或用时达到 maxMs 就停，让出 UI 线程给 WM_PAINT / 输入 / 模态对话框。
// 每次 itcore_feed 至多 sliceBytes，切片之间检查预算；给了 PumpCarry 时一块可以在中间停下，剩余部分下次接着喂
// （不给 carry 则一块总是喂完再检查，保持旧语义）。nowMs 可注入（测试用假时钟）；为空则只按字节限。
// final（收尾，unlimited() 置位）：核心排空后仍收不下一片时不能返回 FedMore（之后就 finish_input 了，
// 留在 carry 里的数据会丢、调用方也不会续跑），改为先 afterFeed 排空再重试至多 finalRetries 次，仍不行就跳过
// 这一片、记日志，继续喂剩余积压。非收尾时只重试一次，收不下就整片留在 carry 里交给下一次 pump。
// log：诊断输出（可注入；为空则不输出），后端接 OutputDebugString。
struct PumpBudget {
    size_t maxBytes = 64u * 1024u;
    uint64_t maxMs = 8;
    size_t sliceBytes = 16u * 1024u;
    uint64_t (*nowMs)(void *) = nullptr;
    void *clockCtx = nullptr;
    bool final = false;
    int finalRetries = 8;
    void (*log)(void *ctx, const char *msg) = nullptr;
    void *logCtx = nullptr;
    // 片间让出 UI（可注入）。返回 true → 提前结束本轮（当作预算用尽，返回 FedMore），以便消息循环处理输入/绘制。
    // Win32：若 QS_INPUT|QS_PAINT 已挂起则 Peek+Dispatch 非 pump 消息并返回 true；不得 Dispatch 续跑定时器/WAKE（防重入）。
    bool (*yieldUi)(void *ctx) = nullptr;
    void *yieldCtx = nullptr;
    // 单次不限预算（测试 / 旧路径）。正式收尾请用 pump_core_final：按片推进并 yieldUi，避免卡住消息循环。
    static PumpBudget unlimited() { PumpBudget b; b.maxBytes = SIZE_MAX; b.nowMs = nullptr; b.final = true; return b; }
    // 收尾用的单片预算：final=true（NEED_DRAIN 重试），但仍按字节/时间切片，便于片间 yieldUi
    static PumpBudget finalSlice() {
        PumpBudget b;
        b.final = true;
        b.maxBytes = 64u * 1024u;
        b.maxMs = 8;
        return b;
    }
};
// 一块输出里还没喂给核心的部分（跨 pump 调用保留；换 attempt 时清空）
struct PumpCarry {
    std::string bytes;
    size_t off = 0;
    bool empty() const { return off >= bytes.size(); }
    void clear() { bytes.clear(); off = 0; }
};

// 把 pop() 取到的输出喂给核心，直到取空或预算用尽；每次 itcore_feed 后调用 afterFeed()（排空核心事件）。
// pop() 返回可判真、可 -> 取 bytes（std::string / vector<char>）的对象，如 std::optional<Chunk>。
// 核心事件队列满（NEED_DRAIN 且 accepted==0）时先 afterFeed 排空再重试（收尾 finalRetries 次，否则一次）；
// 仍收不下：非收尾且有 carry → 整片留在 carry、返回 FedMore；收尾或无 carry → 跳过这一片并记日志（防死循环）。
template <class PopFn, class AfterFeed>
PumpResult pump_core(itcore_core *core, PopFn &&pop, AfterFeed &&afterFeed, const PumpBudget &budget = {},
                     PumpCarry *carry = nullptr) {
    bool any = false;
    size_t fed = 0;
    PumpCarry local;
    PumpCarry &cur = carry ? *carry : local;
    const uint64_t t0 = budget.nowMs ? budget.nowMs(budget.clockCtx) : 0;
    auto exhausted = [&] {
        if (fed >= budget.maxBytes) return true;
        return budget.nowMs && budget.nowMs(budget.clockCtx) - t0 >= budget.maxMs;
    };
    const size_t slice = budget.sliceBytes ? budget.sliceBytes : SIZE_MAX;
    for (;;) {
        if (any && (carry || cur.empty()) && exhausted()) { afterFeed(); return PumpResult::FedMore; }
        if (cur.empty()) {
            cur.clear();
            auto chunk = pop();
            if (!chunk) { afterFeed(); return any ? PumpResult::Fed : PumpResult::Idle; }
            if constexpr (std::is_same_v<std::decay_t<decltype(chunk->bytes)>, std::string>) cur.bytes = std::move(chunk->bytes);
            else cur.bytes.assign(chunk->bytes.begin(), chunk->bytes.end());
            if (cur.bytes.empty()) continue;
        }
        any = true;
        const size_t n = std::min(cur.bytes.size() - cur.off, slice);
        const auto *p = reinterpret_cast<const uint8_t *>(cur.bytes.data() + cur.off);
        auto fr = itcore_feed(core, p, n);
        afterFeed();
        if (fr.status == ITCORE_FATAL) return PumpResult::Fatal;
        if (fr.accepted == 0) {
            const int retries = budget.final ? std::max(1, budget.finalRetries) : 1;
            for (int i = 0; i < retries && fr.accepted == 0; ++i) {  // 每次重试前都已 afterFeed 排空
                fr = itcore_feed(core, p, n);
                afterFeed();
                if (fr.status == ITCORE_FATAL) return PumpResult::Fatal;
            }
            if (fr.accepted == 0) {
                // 非收尾：不丢这一片，留在 carry 里交给下一次 pump
                if (carry && !budget.final) return PumpResult::FedMore;
                if (budget.log) {
                    char msg[128];
                    std::snprintf(msg, sizeof msg, "[pump] core refused %zu bytes after %d drain retries, skipped%s", n, retries,
                                  budget.final ? " (final drain)" : "");
                    budget.log(budget.logCtx, msg);
                }
            }
        }
        const size_t adv = fr.accepted ? fr.accepted : n;
        cur.off += adv;
        fed += adv;
        // 切片之间：若 UI 有输入/绘制待处理，提前让出（收尾同样返回 FedMore，由 pump_core_final 片间再 yield）
        if (budget.yieldUi && budget.yieldUi(budget.yieldCtx)) { afterFeed(); return PumpResult::FedMore; }
    }
}

// 收尾按片喂完积压：每片用 finalSlice 预算，片间调用 budget.yieldUi（可空）。
// 语义与一次 unlimited 相同（全部喂完才返回），但不会长时间占住 UI 线程。
// NEED_DRAIN 走 final 重试；非 FedMore 结束（Fed / Idle / Fatal）。
template <class PopFn, class AfterFeed>
PumpResult pump_core_final(itcore_core *core, PopFn &&pop, AfterFeed &&afterFeed, PumpCarry *carry,
                           PumpBudget slice = PumpBudget::finalSlice()) {
    slice.final = true;
    for (;;) {
        auto r = pump_core(core, pop, afterFeed, slice, carry);
        if (r != PumpResult::FedMore) return r;
        if (slice.yieldUi) (void)slice.yieldUi(slice.yieldCtx);  // 片间再抽一次，给对话框/会话树 WM_PAINT
    }
}

// finishSession 的输入收尾顺序（平台无关，可测）：
//   pumpFinal()（按片喂完积压，片间可 yieldUi）→ killContTimer()（撤掉挂着的续跑）→ sched.reset()（读线程恢复投递资格，
//   但 inputFinished 之后 pump 直接返回）→ itcore_finish_input → inputFinished = true。
// finish_input 必须在最后一次 feed 之后；inputFinished 必须在 finish_input 之后才置位（pumpFinal 内仍须能喂）。
template <class PumpFinal, class KillTimer>
void finish_session_input(itcore_core *core, bool &inputFinished, PumpScheduler &sched, PumpFinal &&pumpFinal,
                          KillTimer &&killContTimer) {
    pumpFinal();
    killContTimer();
    sched.reset();
    if (core && !inputFinished) { itcore_finish_input(core); inputFinished = true; }
}

// 续跑方式：FedMore 与 Fed+missed 都走定时器（见 PumpScheduler::end），禁止 Post 续跑。
// WM_TIMER 优先级低于 WM_PAINT 与输入，关闭点击才能进 MessageBoxW。
enum class PumpNext { None, Timer };
inline PumpNext nextAfterPump(PumpResult r) { return r == PumpResult::FedMore ? PumpNext::Timer : PumpNext::None; }

// present() 的出帧闸门：?2026 期间按 SyncGate（同步结束计数变化放行、150ms 兜底）；force（改尺寸）总是放行
inline bool frame_due(SyncGate &gate, const itcore_core *core, uint64_t nowMs, bool force) {
    const bool g = gate.allow(itcore_mode_query(core, 2026) == ITCORE_MODE_SET, nowMs, itcore_sync_end_count(core));
    return g || force;
}

}  // namespace it2app
