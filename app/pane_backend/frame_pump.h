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

// Fed：本次喂过数据且已取空；FedMore：预算用尽、可能还有数据，须安排续跑。

// 同一未读片上 NEED_DRAIN 且 accepted==0：先 afterFeed 排空事件，再重喂这一片，偏移不前进。
// 这样的重试最多 kNeedDrainZeroProgressRetries 次（不含第一次 feed）。全部仍是 accepted==0
// 才把该片计入 droppedBytes 并跳过，避免核心一直拒收时靠续跑定时器忙等。
// Running 与 Draining 共用此上限。accepted>0 的 NEED_DRAIN 只前进已接收字节，不占重试次数。
inline constexpr int kNeedDrainZeroProgressRetries = 8;

// 每次 pump 的预算：喂满 maxBytes 或用时达到 maxMs 就停，让出 UI 线程给 WM_PAINT / 输入 / 模态对话框。
// 每次 itcore_feed 至多 sliceBytes，切片之间检查预算；给了 PumpCarry 时一块可以在中间停下，剩余部分下次接着喂
// （不给 carry 则一块总是喂完再检查，保持旧语义）。nowMs 可注入（测试用假时钟）；为空则只按字节限。
// 零进展重试见 kNeedDrainZeroProgressRetries / finalRetries。仍拒收则计数跳过；下一轮继续排空其它字节。
// log 接 OutputDebugString，stats 累计所有故障丢弃字节。终态把该计数写成「输出可能不完整，丢弃字节数=N」。
struct PumpBudget {
    size_t maxBytes = 64u * 1024u;
    uint64_t maxMs = 8;
    size_t sliceBytes = 16u * 1024u;
    uint64_t (*nowMs)(void *) = nullptr;
    void *clockCtx = nullptr;
    bool final = false;
    // 连续 accepted==0 的重试次数（不含第一次 feed）。Running 与 Draining 都用它。
    // 默认 kNeedDrainZeroProgressRetries。小于 1 时按 1 处理，保证至少排空后再试一次。
    int finalRetries = kNeedDrainZeroProgressRetries;
    void (*log)(void *ctx, const char *msg) = nullptr;
    void *logCtx = nullptr;
    // 片间让出 UI（可注入）。返回 true → 提前结束本轮（当作预算用尽，返回 FedMore），以便消息循环处理输入/绘制。
    // 只能查询，不得派发消息、弹模态框或调用外部回调。返回后靠一次性低优先级定时器续跑。
    bool (*yieldUi)(void *ctx) = nullptr;
    void *yieldCtx = nullptr;
    PumpStats *stats = nullptr;
    // 单次不限预算仅供底层测试；正式收尾每次只运行 finalSlice。
    static PumpBudget unlimited() { PumpBudget b; b.maxBytes = SIZE_MAX; b.nowMs = nullptr; b.final = true; return b; }
    // 收尾用的单片预算：final=true，但仍按字节/时间切片，便于片间 yieldUi。
    // NEED_DRAIN 零进展重试次数与 Running 相同（finalRetries）。
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
// NEED_DRAIN 且 accepted==0：不把未读字节当成已消费。afterFeed 之后重喂同一片，最多 finalRetries 次。
// 仍是 accepted==0：该片计入 stats->droppedBytes（会话终态「输出可能不完整，丢弃字节数=N」）并跳过。
// accepted>0 只前进已接收的部分，剩余留在 carry 里下次再喂。
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
        // 至少一次：第一次 accepted==0 之后必须先排空再重试，不能把这一片直接记成已消费。
        const int retries = std::max(1, budget.finalRetries);
        auto fr = itcore_feed(core, p, n);
        afterFeed();
        if (fr.status == ITCORE_FATAL) { cur.off += fr.accepted; return PumpResult::Fatal; }
        if (fr.accepted == 0) {
            for (int i = 0; i < retries && fr.accepted == 0; ++i) {  // 每次重试前都已 afterFeed
                fr = itcore_feed(core, p, n);
                afterFeed();
                if (fr.status == ITCORE_FATAL) { cur.off += fr.accepted; return PumpResult::Fatal; }
            }
        }
        size_t adv = fr.accepted;
        if (fr.accepted == 0) {
            adv = n;
            if (budget.stats) { budget.stats->droppedBytes += n; ++budget.stats->skippedSlices; }
            if (budget.log) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "[pump] core refused %zu bytes after %d drain retries, skipped%s", n, retries,
                              budget.final ? " (final drain)" : "");
                budget.log(budget.logCtx, msg);
            }
        }
        cur.off += adv;
        fed += adv;
        // 切片之间：若 UI 有输入/绘制待处理，提前让出（收尾同样返回 FedMore，下一轮定时器继续）
        if (budget.yieldUi && budget.yieldUi(budget.yieldCtx)) { afterFeed(); return PumpResult::FedMore; }
    }
}

// Win32 和可移植 FakeHost 共用同一驱动；Host 原语不得派发消息或发出 PaneEvents。
// 所有回调必须在 step 返回后，用 PendingEvents 的局部批次发送。
class PumpDriver {
public:
    DrainPhase phase = DrainPhase::Running;
    PumpStats stats;
    PumpCarry carry;
    bool inputFinished = false;
    CloseGraceState closeGrace;

    template<class Host> void requestFinish(Host &host, PumpScheduler &sched) {
        if (phase != DrainPhase::Running) return;
        phase = DrainPhase::Draining;
        arm(host, sched); // 只登记续跑，绝不同步 feed/finish_input。
    }
    template<class Host> void reset(Host &host, PumpScheduler &sched) {
        host.cancelPumpTimer(); sched.reset();
        phase = DrainPhase::Running; stats = {}; carry.clear(); inputFinished = false;
        closeGrace = {};
    }
    template<class Host> void stop(Host &host, PumpScheduler &sched) {
        host.cancelPumpTimer(); sched.reset(); phase = DrainPhase::Finished; carry.clear();
    }
    template<class Host> void step(Host &host, itcore_core *core, PumpScheduler &sched, PumpBudget budget = {}) {
        if (phase == DrainPhase::Finished || (phase == DrainPhase::Running && (!core || inputFinished))) return;
        sched.begin();
        budget.final = phase == DrainPhase::Draining;
        budget.stats = &stats;
        PumpResult r = inputFinished || !core ? PumpResult::Idle :
            pump_core(core, [&] { return host.popOutput(); }, [&] { host.afterFeed(); }, budget, &carry);
        auto decision = decidePumpStep(phase, r, carry.empty());
        if (r == PumpResult::Fatal || inputFinished) {
            inputFinished = true; // 真正 Fatal 后不再 feed，也不再 finish_input。
            stats.droppedBytes += carry.bytes.size() - carry.off;
            carry.clear();
            if (phase == DrainPhase::Draining) {
                // Fatal 的丢弃也按轮推进；超时关闭时读线程可能仍在写，不能无界 pop。
                size_t discarded = 0;
                bool empty = false;
                while (discarded < budget.maxBytes) {
                    auto chunk = host.popOutput();
                    if (!chunk) { empty = true; break; }
                    discarded += chunk->bytes.size();
                    stats.droppedBytes += chunk->bytes.size();
                }
                decision.complete = empty;
                decision.armTimer = !empty;
            }
        }
        if (decision.fault) {
            host.cancelPumpTimer(); sched.reset(); host.closePumpFault(); return;
        }
        // 关闭结果可能早于读线程退出。队列见空也不收尾，直到传输 Closed/Failed（pumpInputEnded）。
        // 例外：closer 已报 timedOut。宽限 kCloseTimeoutGraceMs 内继续排空；期满按本轮预算丢弃
        // 未喂字节并完成收尾，终态带上超时原因。未超时的关闭不走宽限。无时钟则保持等待。
        const bool inputEnded = host.pumpInputEnded();
        const uint64_t now = budget.nowMs ? budget.nowMs(budget.clockCtx) : 0;
        const auto grace = decideCloseTimeoutGrace(phase, inputEnded, host.pumpCloseTimedOut(),
                                                   closeGrace, budget.nowMs != nullptr, now);
        if (grace == CloseGraceAction::ArmAndWait) {
            closeGrace.armed = true;
            closeGrace.startMs = now;
        }
        if (grace == CloseGraceAction::Abandon) {
            if (!carry.empty()) stats.droppedBytes += carry.bytes.size() - carry.off;
            carry.clear();
            size_t discarded = 0;
            while (discarded < budget.maxBytes) {
                auto chunk = host.popOutput();
                if (!chunk) break;
                discarded += chunk->bytes.size();
                stats.droppedBytes += chunk->bytes.size();
            }
            decision.complete = true;
            decision.armTimer = false;
        } else if (decision.complete && !inputEnded) {
            decision.complete = false;
            decision.armTimer = true;
        }
        if (decision.complete) {
            host.cancelPumpTimer(); sched.reset();
            if (core && !inputFinished) { host.finishPumpInput(core); inputFinished = true; }
            host.afterFeed();
            phase = DrainPhase::Finished;
            host.presentPump(chooseFrameMode(phase, false));
            host.completePump(stats.droppedBytes);
            return;
        }
        bool pending = sched.end(decision.armTimer) == PumpScheduler::Next::Timer;
        if (pending) arm(host, sched);
        else host.cancelPumpTimer();
        if (decision.frame) host.presentPump(chooseFrameMode(phase, pending));
    }
private:
    template<class Host> static void arm(Host &host, PumpScheduler &sched) {
        if (!host.armPumpTimer()) sched.timerFailed(); // 周期 tick 兜底，禁止 Post 续跑。
    }
};

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
