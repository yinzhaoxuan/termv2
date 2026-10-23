// pump() → present() 的平台无关核心：Win32PaneBackend 与 Linux 端到端测试走同一段代码。
#pragma once
#include "pane_logic.h"
#include "itcore.h"
#include <cstdint>

namespace it2app {

enum class PumpResult { Idle, Fed, Fatal };

// 把 pop() 取到的全部输出喂给核心；每次 itcore_feed 后调用 afterFeed()（排空核心事件）。
// pop() 返回可判真、可 -> 取 bytes（std::string / vector<char>）的对象，如 std::optional<Chunk>。
template <class PopFn, class AfterFeed>
PumpResult pump_core(itcore_core *core, PopFn &&pop, AfterFeed &&afterFeed) {
    bool any = false;
    while (auto chunk = pop()) {
        any = true;
        const auto &b = chunk->bytes;
        size_t off = 0;
        while (off < b.size()) {
            auto fr = itcore_feed(core, reinterpret_cast<const uint8_t *>(b.data() + off), b.size() - off);
            off += fr.accepted ? fr.accepted : b.size() - off;
            afterFeed();
            if (fr.status == ITCORE_FATAL) return PumpResult::Fatal;
        }
    }
    afterFeed();
    return any ? PumpResult::Fed : PumpResult::Idle;
}

// present() 的出帧闸门：?2026 期间按 SyncGate（同步结束计数变化放行、150ms 兜底）；force（改尺寸）总是放行
inline bool frame_due(SyncGate &gate, const itcore_core *core, uint64_t nowMs, bool force) {
    const bool g = gate.allow(itcore_mode_query(core, 2026) == ITCORE_MODE_SET, nowMs, itcore_sync_end_count(core));
    return g || force;
}

}  // namespace it2app
