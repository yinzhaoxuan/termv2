/* 平台无关的光标几何（DECSCUSR）、闪烁与 OSC 8 链接悬停下划线（DESIGN-T06）。D2D 只按这里的矩形画。
   光标样式来自 itcore_snapshot_cursor_style（DECSCUSR Ps），D2DRenderer::present_frame 经 decscusr_to_style 使用。 */
#pragma once
#include "grid_metrics.h"
#include "itcore.h"
#include <cmath>
#include <vector>

namespace itren {

enum class CursorShape { Block, Underline, Bar };
struct CursorStyle { CursorShape shape = CursorShape::Block; bool blink = true; };
using PxRect = GridMetrics::Rect;

// DECSCUSR：0/1 闪烁块，2 稳定块，3/4 下划线，5/6 竖线；未知值按闪烁块
inline CursorStyle decscusr_to_style(int ps) {
  switch (ps) {
    case 2: return {CursorShape::Block, false};
    case 3: return {CursorShape::Underline, true};
    case 4: return {CursorShape::Underline, false};
    case 5: return {CursorShape::Bar, true};
    case 6: return {CursorShape::Bar, false};
    default: return {CursorShape::Block, true};
  }
}

inline int line_thickness_px(float dpi_scale) { return std::max(1, int(std::lround(2.0f * dpi_scale))); }
inline int outline_thickness_px(float dpi_scale) { return std::max(1, int(std::lround(dpi_scale))); }

// wide：光标在宽字符首格时为 true，块/空心框覆盖 2 格；下划线同宽，竖线只在左边
inline std::vector<PxRect> cursor_rects(const GridMetrics &m, uint16_t row, uint16_t col, CursorStyle st,
                                        bool focused, bool wide, float dpi_scale) {
  PxRect c = m.cell_rect(col, row, wide ? 2 : 1);
  switch (st.shape) {
    case CursorShape::Underline: { int t = std::min(line_thickness_px(dpi_scale), c.h); return {{c.x, c.y + c.h - t, c.w, t}}; }
    case CursorShape::Bar: { int t = std::min(line_thickness_px(dpi_scale), c.w); return {{c.x, c.y, t, c.h}}; }
    case CursorShape::Block: break;
  }
  if (focused) return {c};
  int t = std::min({outline_thickness_px(dpi_scale), c.w / 2, c.h / 2}); t = std::max(t, 1);
  return {{c.x, c.y, c.w, t}, {c.x, c.y + c.h - t, c.w, t},
          {c.x, c.y + t, t, c.h - 2 * t}, {c.x + c.w - t, c.y + t, t, c.h - 2 * t}};
}

// 仅聚焦的实心块光标下字形需反色绘制
inline bool cursor_inverts_glyph(CursorStyle st, bool focused) { return focused && st.shape == CursorShape::Block; }

// 闪烁：不闪烁、失焦或输入后 period 内常亮；之后按 period 亮灭交替
inline bool cursor_visible(CursorStyle st, bool focused, uint64_t ms_since_last_input, uint32_t period_ms = 530) {
  if (!st.blink || !focused || period_ms == 0 || ms_since_last_input < period_ms) return true;
  return ((ms_since_last_input / period_ms) % 2) == 0;
}

struct LinkSpan { uint16_t row, c0, c1; uint32_t id; };  // [c0,c1)，c1==c0 表示无链接

// 悬停格所在的同 hyperlink_id 连续格段；落在宽字符尾格时按首格处理
inline LinkSpan hover_link_span(const itcore_snapshot *s, uint16_t row, uint16_t col) {
  LinkSpan none{row, col, col, 0};
  if (!s || row >= itcore_snapshot_rows(s)) return none;
  uint16_t cols = itcore_snapshot_columns(s);
  if (col >= cols) return none;
  itcore_wrap w; uint64_t rr;
  const itcore_cell *r = itcore_snapshot_row(s, row, &w, &rr);
  if (!r) return none;
  uint16_t c = col;
  while (c > 0 && (r[c].flags & ITCORE_CELL_CONTINUATION)) --c;
  uint32_t id = r[c].hyperlink_id;
  if (!id) return none;
  auto same = [&](uint16_t x) { return r[x].hyperlink_id == id || ((r[x].flags & ITCORE_CELL_CONTINUATION) && x > 0 && r[x - 1].hyperlink_id == id); };
  uint16_t c0 = c, c1 = uint16_t(c + 1);
  while (c0 > 0 && same(uint16_t(c0 - 1))) --c0;
  while (c1 < cols && same(c1)) ++c1;
  return {row, c0, c1, id};
}

// 跨行链接：从悬停段向上下扩展，仅当段贴着行边界（上行延伸到行尾 / 下行从行首开始）且同 id
inline std::vector<LinkSpan> hover_link_rows(const itcore_snapshot *s, uint16_t row, uint16_t col) {
  std::vector<LinkSpan> out;
  LinkSpan h = hover_link_span(s, row, col);
  if (!h.id) return out;
  uint16_t cols = itcore_snapshot_columns(s), rows = itcore_snapshot_rows(s);
  std::vector<LinkSpan> up;
  LinkSpan cur = h;
  while (cur.c0 == 0 && cur.row > 0) {
    LinkSpan p = hover_link_span(s, uint16_t(cur.row - 1), uint16_t(cols - 1));
    if (p.id != h.id || p.c1 != cols) break;
    up.push_back(p); cur = p;
  }
  out.assign(up.rbegin(), up.rend());
  out.push_back(h);
  cur = h;
  while (cur.c1 == cols && cur.row + 1 < rows) {
    LinkSpan n = hover_link_span(s, uint16_t(cur.row + 1), 0);
    if (n.id != h.id || n.c0 != 0) break;
    out.push_back(n); cur = n;
  }
  return out;
}

// 段的下划线：格底部，粗细同下划线光标
inline PxRect underline_rect(const GridMetrics &m, const LinkSpan &sp, float dpi_scale) {
  PxRect a = m.cell_rect(sp.c0, sp.row);
  int t = std::min(line_thickness_px(dpi_scale), a.h);
  return {a.x, a.y + a.h - t, a.w * std::max(0, sp.c1 - sp.c0), t};
}

}  // namespace itren
