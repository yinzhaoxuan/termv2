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
struct PumpBudget {
    size_t maxBytes = 64u * 1024u;
    uint64_t maxMs = 8;
    size_t sliceBytes = 16u * 1024u;
    uint64_t (*nowMs)(void *) = nullptr;
    void *clockCtx = nullptr;
    static PumpBudget unlimited() { PumpBudget b; b.maxBytes = SIZE_MAX; b.nowMs = nullptr; return b; }  // 收尾用
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
// 核心事件队列满（NEED_DRAIN 且 accepted==0）时先 afterFeed 排空再重试一次；仍收不下才跳过这一片（防死循环）。
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
            fr = itcore_feed(core, p, n);
            afterFeed();
            if (fr.status == ITCORE_FATAL) return PumpResult::Fatal;
            // 排空后仍收不下：不丢这一片，留在 carry 里交给下一次 pump（无 carry 时只能跳过，记诊断）
            if (fr.accepted == 0) {
                if (carry) return PumpResult::FedMore;
                std::fprintf(stderr, "[pump] core refused %zu bytes after drain, skipped\n", n);
            }
        }
        const size_t adv = fr.accepted ? fr.accepted : n;
        cur.off += adv;
        fed += adv;
    }
}

// 续跑方式：预算用尽时不能 PostMessage（投递消息先于输入/WM_PAINT，仍会饿死），改用一次性 1ms 定时器
// （WM_TIMER 优先级低于 WM_PAINT 与输入）。
enum class PumpNext { None, Timer };
inline PumpNext nextAfterPump(PumpResult r) { return r == PumpResult::FedMore ? PumpNext::Timer : PumpNext::None; }

// present() 的出帧闸门：?2026 期间按 SyncGate（同步结束计数变化放行、150ms 兜底）；force（改尺寸）总是放行
inline bool frame_due(SyncGate &gate, const itcore_core *core, uint64_t nowMs, bool force) {
    const bool g = gate.allow(itcore_mode_query(core, 2026) == ITCORE_MODE_SET, nowMs, itcore_sync_end_count(core));
    return g || force;
}

}  // namespace it2app
