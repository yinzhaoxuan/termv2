#pragma once
// generation 不一致时的布局保持（渲染评审第 5 点）。平台无关，可在 Linux 单测。
// 规则：快照 generation 与期望一致（或未设期望）时采用快照布局；不一致时保留上一次一致帧的布局，
// 只绘制两者交集内的格子，交集以外（含客户区多出部分）一律填背景色。
#include <cstdint>

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

}  // namespace itren
