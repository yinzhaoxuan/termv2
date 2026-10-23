// pump() → present() 的平台无关核心：Win32PaneBackend 与 Linux 端到端测试走同一段代码。
#pragma once
#include "pane_logic.h"
#include "itcore.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace it2app {

// Fed：本次喂过数据且已取空；FedMore：预算用尽、可能还有数据，须安排续跑（见 nextAfterPump）
enum class PumpResult { Idle, Fed, FedMore, Fatal };

// 每次 pump 的预算：喂满 maxBytes 或用时达到 maxMs 就停（以块为单位，块不拆分），让出 UI 线程给
// WM_PAINT / 输入 / 模态对话框。nowMs 可注入（测试用假时钟）；为空则只按字节限。
struct PumpBudget {
    size_t maxBytes = 64u * 1024u;
    uint64_t maxMs = 8;
    uint64_t (*nowMs)(void *) = nullptr;
    void *clockCtx = nullptr;
};

// 把 pop() 取到的输出喂给核心，直到取空或预算用尽；每次 itcore_feed 后调用 afterFeed()（排空核心事件）。
// pop() 返回可判真、可 -> 取 bytes（std::string / vector<char>）的对象，如 std::optional<Chunk>。
template <class PopFn, class AfterFeed>
PumpResult pump_core(itcore_core *core, PopFn &&pop, AfterFeed &&afterFeed, const PumpBudget &budget = {}) {
    bool any = false;
    size_t fed = 0;
    const uint64_t t0 = budget.nowMs ? budget.nowMs(budget.clockCtx) : 0;
    auto exhausted = [&] {
        if (fed >= budget.maxBytes) return true;
        return budget.nowMs && budget.nowMs(budget.clockCtx) - t0 >= budget.maxMs;
    };
    while (!(any && exhausted())) {
        auto chunk = pop();
        if (!chunk) { afterFeed(); return any ? PumpResult::Fed : PumpResult::Idle; }
        any = true;
        const auto &b = chunk->bytes;
        size_t off = 0;
        while (off < b.size()) {
            auto fr = itcore_feed(core, reinterpret_cast<const uint8_t *>(b.data() + off), b.size() - off);
            off += fr.accepted ? fr.accepted : b.size() - off;
            afterFeed();
            if (fr.status == ITCORE_FATAL) return PumpResult::Fatal;
        }
        fed += b.size();
    }
    afterFeed();
    return PumpResult::FedMore;
}

// 续跑方式：预算用尽时不能 PostMessage（投递消息先于输入/WM_PAINT，仍会饿死），改用一次性 1ms 定时器
// （WM_TIMER 优先级低于 WM_PAINT 与输入）。
enum class PumpNext { None, Timer };
inline PumpNext nextAfterPump(PumpResult r) { return r == PumpResult::FedMore ? PumpNext::Timer : PumpNext::None; }

// 读线程有新数据时是否投递 pump：续跑定时器在等时不投递（否则绕过定时器重新饿死 UI）；
// 否则沿用 pumpPending 合并（同一时刻至多一条投递）。
inline bool readerShouldPost(std::atomic<bool> &pumpPending, const std::atomic<bool> &contTimerPending) {
    if (contTimerPending.load()) return false;
    return !pumpPending.exchange(true);
}

// present() 的出帧闸门：?2026 期间按 SyncGate（同步结束计数变化放行、150ms 兜底）；force（改尺寸）总是放行
inline bool frame_due(SyncGate &gate, const itcore_core *core, uint64_t nowMs, bool force) {
    const bool g = gate.allow(itcore_mode_query(core, 2026) == ITCORE_MODE_SET, nowMs, itcore_sync_end_count(core));
    return g || force;
}

}  // namespace it2app
