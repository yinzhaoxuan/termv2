#pragma once
// generation 不一致时的布局保持（渲染评审第 5 点）。平台无关，可在 Linux 单测。
// 规则：快照 generation 与期望一致（或未设期望）时采用快照布局；不一致时保留上一次一致帧的布局，
// 只绘制两者交集内的格子，交集以外（含客户区多出部分）一律填背景色。
#include <cstdint>

// ?2026 同步输出：后端暂停取快照、D2D 暂停 Present 的共同上限（渲染设计第 4 节）
#ifndef ITREN_SYNC_TIMEOUT_MS
#define ITREN_SYNC_TIMEOUT_MS 150
#endif

namespace itren {

struct FrameLayout {
    uint16_t cols = 0, rows = 0;        // 本帧使用的布局
    uint16_t draw_cols = 0, draw_rows = 0;  // 实际从快照取内容的范围（交集）
    bool gen_mismatch = false;
    bool layout_changed = false;        // 布局变化：调用方须整帧重画
};

class LayoutKeeper {
public:
    FrameLayout update(uint16_t snap_cols, uint16_t snap_rows, uint64_t snap_gen, uint64_t expected_gen) {
        FrameLayout f;
        f.gen_mismatch = expected_gen != 0 && snap_gen != expected_gen;
        if (!f.gen_mismatch || !have_) { cols_ = snap_cols; rows_ = snap_rows; have_ = true; }
        f.cols = cols_; f.rows = rows_;
        f.draw_cols = snap_cols < cols_ ? snap_cols : cols_;
        f.draw_rows = snap_rows < rows_ ? snap_rows : rows_;
        f.layout_changed = f.cols != last_.cols || f.rows != last_.rows || f.gen_mismatch != last_.gen_mismatch ||
                           f.draw_cols != last_.draw_cols || f.draw_rows != last_.draw_rows;
        last_ = f;
        return f;
    }
    void reset() { have_ = false; last_ = {}; }
private:
    bool have_ = false;
    uint16_t cols_ = 0, rows_ = 0;
    FrameLayout last_{};
};

// 文本样式选择（下划线/粗体/斜体）；D2D 用它选 TextFormat 变体与是否画下划线
struct TextStyle { bool bold, italic, underline; int format_index() const { return (bold ? 1 : 0) | (italic ? 2 : 0); } };
inline TextStyle text_style(uint16_t flags, uint16_t bold_bit, uint16_t italic_bit, uint16_t underline_bit) {
    return {(flags & bold_bit) != 0, (flags & italic_bit) != 0, (flags & underline_bit) != 0};
}


// Present 节流（PERF-01）：洪峰期避免每次 Present(1,0) 卡 vsync (~16ms) 饿死 UI。
// 平台无关；D2DRenderer::present_frame 与单测共用。
enum class PresentMode {
    VSync,      // 空闲/稳态：允许 Present(sync_interval=1)
    Immediate,  // 续跑中（FedMore / 即将再 pump）：Present(0, DO_NOT_WAIT)，不堵 UI
    Force,      // 改尺寸等：始终 Present(1,0)，绕过间隔
};

struct PresentPace {
    static constexpr uint64_t kMinIntervalMs = 16;  // ≈60fps 上限

    enum class Action { Skip, PresentWait, PresentNoWait };

    // now_ms：单调毫秒（与 set_clock / steady_clock 一致）
    Action decide(uint64_t now_ms, PresentMode mode) const {
        if (mode == PresentMode::Force) return Action::PresentWait;
        if (have_ && now_ms >= last_ok_ms_ && (now_ms - last_ok_ms_) < kMinIntervalMs)
            return Action::Skip;  // 距上次成功 Present 过近：保持脏区，下一机会再出
        return mode == PresentMode::VSync ? Action::PresentWait : Action::PresentNoWait;
    }
    void note_presented(uint64_t now_ms) { last_ok_ms_ = now_ms; have_ = true; }
    void reset() { have_ = false; last_ok_ms_ = 0; }
    bool have() const { return have_; }
    uint64_t last_ok_ms() const { return last_ok_ms_; }

private:
    bool have_ = false;
    uint64_t last_ok_ms_ = 0;
};

}  // namespace itren
