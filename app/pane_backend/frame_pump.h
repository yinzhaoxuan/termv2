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

// 每次 pump 的预算：喂满 maxBytes 或用时达到 maxMs 就停，让出 UI 线程给 WM_PAINT / 输入 / 模态对话框。
// 每次 itcore_feed 至多 sliceBytes，切片之间检查预算；给了 PumpCarry 时一块可以在中间停下，剩余部分下次接着喂
// （不给 carry 则一块总是喂完再检查，保持旧语义）。nowMs 可注入（测试用假时钟）；为空则只按字节限。
// final 收尾预算：NEED_DRAIN 零进展有界重试，仍拒收则计数跳过；下一轮仍可继续排空。
// log 接 OutputDebugString，stats 累计所有故障丢弃字节。
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
    // 只能查询，不得派发消息、弹模态框或调用外部回调。返回后靠一次性低优先级定时器续跑。
    bool (*yieldUi)(void *ctx) = nullptr;
    void *yieldCtx = nullptr;
    PumpStats *stats = nullptr;
    // 单次不限预算仅供底层测试；正式收尾每次只运行 finalSlice。
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
        if (fr.status == ITCORE_FATAL) { cur.off += fr.accepted; return PumpResult::Fatal; }
        if (fr.accepted == 0) {
            const int retries = budget.final ? std::max(1, budget.finalRetries) : 1;
            for (int i = 0; i < retries && fr.accepted == 0; ++i) {  // 每次重试前都已 afterFeed 排空
                fr = itcore_feed(core, p, n);
                afterFeed();
                if (fr.status == ITCORE_FATAL) { cur.off += fr.accepted; return PumpResult::Fatal; }
            }
            if (fr.accepted == 0) {
                // 非收尾：不丢这一片，留在 carry 里交给下一次 pump
                if (carry && !budget.final) return PumpResult::FedMore;
                if (budget.stats) { budget.stats->droppedBytes += n; ++budget.stats->skippedSlices; }
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

    template<class Host> void requestFinish(Host &host, PumpScheduler &sched) {
        if (phase != DrainPhase::Running) return;
        phase = DrainPhase::Draining;
        arm(host, sched); // 只登记续跑，绝不同步 feed/finish_input。
    }
    template<class Host> void reset(Host &host, PumpScheduler &sched) {
        host.cancelPumpTimer(); sched.reset();
        phase = DrainPhase::Running; stats = {}; carry.clear(); inputFinished = false;
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
        // 关闭线程超时结果可能早于读线程退出；等真正 Closed/Failed 才能证明队列不会再增长。
        if (decision.complete && !host.pumpInputEnded()) {
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
