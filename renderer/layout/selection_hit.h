/* 拖动选区的像素命中与选区绘制矩形（平台无关）。边界语义：boundary ∈ [0, cols]，选区为 [起点边界, 终点边界)。 */
#pragma once
#include "grid_metrics.h"
#include "itcore.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace itren {

struct SelHit {
  uint16_t row = 0, col = 0;   // 命中的格（宽字符取首格）
  bool after = false;          // 落在格（宽字符整体）右半：选到并包含该格
  uint16_t boundary = 0;       // 列边界：after ? col+span : col
  int autoscroll_dir = 0;      // -1 向上（历史）、+1 向下、0 不滚
  int autoscroll_lines = 0;    // 每个滚动节拍的行数，与越界距离（行）成正比，封顶
};

// s 可为空（不做宽字符吸附）；max_speed 为每节拍最多滚动行数
inline SelHit selection_hit(const GridMetrics &m, int px, int py, uint16_t cols, uint16_t rows,
                            const itcore_snapshot *s = nullptr, int max_speed = 10) {
  SelHit h;
  int cw = std::max(1, m.cell_w_px()), ch = std::max(1, m.cell_h_px());
  int top = m.pad_y_px(), bottom = top + rows * ch, left = m.pad_x_px(), right = left + cols * cw;
  auto speed = [&](int dist) { return std::min(max_speed, std::max(1, (dist + ch - 1) / ch)); };
  if (py < top) { h.autoscroll_dir = -1; h.autoscroll_lines = speed(top - py); return h; }  // row0 col0
  if (py >= bottom) {
    h.row = uint16_t(rows - 1); h.col = uint16_t(cols - 1); h.after = true; h.boundary = cols;
    h.autoscroll_dir = 1; h.autoscroll_lines = speed(py - bottom + 1);
    if (s) { itcore_wrap w; uint64_t rr; const itcore_cell *r = itcore_snapshot_row(s, h.row, &w, &rr);
             while (r && h.col > 0 && (r[h.col].flags & ITCORE_CELL_CONTINUATION)) --h.col; }
    return h;
  }
  h.row = uint16_t((py - top) / ch);
  if (px < left) return h;                         // col 0, 左边界
  if (px >= right) {
    h.col = uint16_t(cols - 1); h.after = true; h.boundary = cols;
  } else {
    int c = (px - left) / cw;
    int lead = c, span = 1;
    if (s) {
      itcore_wrap w; uint64_t rr; const itcore_cell *r = itcore_snapshot_row(s, h.row, &w, &rr);
      if (r) {
        while (lead > 0 && (r[lead].flags & ITCORE_CELL_CONTINUATION)) --lead;
        if (r[lead].width == 2 && lead + 1 < cols) span = 2;
      }
    }
    int x0 = left + lead * cw, mid2 = 2 * x0 + span * cw;   // 2*中线，避免半像素
    h.col = uint16_t(lead); h.after = 2 * px >= mid2;
    h.boundary = uint16_t(h.after ? lead + span : lead);
    if (s && h.col + span - 1 >= cols) h.boundary = cols;
  }
  if (h.col >= cols) h.col = uint16_t(cols - 1);
  return h;
}

struct SelSpan { uint16_t row, c0, c1; };  // [c0,c1)

// 先把每行重叠/相邻的段合并，再把上下相邻且列范围相同的行合并为一个矩形
inline std::vector<GridMetrics::Rect> selection_rects(const GridMetrics &m, std::vector<SelSpan> spans) {
  std::vector<GridMetrics::Rect> out;
  spans.erase(std::remove_if(spans.begin(), spans.end(), [](const SelSpan &s) { return s.c1 <= s.c0; }), spans.end());
  std::sort(spans.begin(), spans.end(), [](const SelSpan &a, const SelSpan &b) { return a.row != b.row ? a.row < b.row : a.c0 < b.c0; });
  std::vector<SelSpan> merged;
  for (auto &s : spans) {
    if (!merged.empty() && merged.back().row == s.row && s.c0 <= merged.back().c1) merged.back().c1 = std::max(merged.back().c1, s.c1);
    else merged.push_back(s);
  }
  struct Blk { SelSpan s; uint16_t nrows; };
  std::vector<Blk> blocks;
  for (auto &s : merged) {
    bool done = false;
    for (auto &b : blocks)
      if (b.s.c0 == s.c0 && b.s.c1 == s.c1 && b.s.row + b.nrows == s.row) { b.nrows++; done = true; break; }
    if (!done) blocks.push_back({s, 1});
  }
  for (auto &b : blocks) {
    auto r = m.cell_rect(b.s.c0, b.s.row);
    out.push_back({r.x, r.y, r.w * (b.s.c1 - b.s.c0), r.h * b.nrows});
  }
  return out;
}

}  // namespace itren
