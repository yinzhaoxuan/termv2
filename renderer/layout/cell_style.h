// SPDX-License-Identifier: GPL-3.0-or-later
/* 平台无关的颜色与脏行解析（DESIGN-T06 §3.2、§3.6）。D2D 后端只消费这里的结果。 */
#pragma once
#include "itcore.h"
#include "grid_metrics.h"
#include "search_highlight.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace itren {

struct Theme {
  // 无快照时的兜底副本；来源 uicfg::builtinAppearance（ui/src/appearance.cpp）。
  // ui/tests/appearance_tests.cpp 对照两者，防止默认外观漂移。
  uint32_t fg = 0xD4D4D4, bg = 0x1E1E1E, cursor = 0x3399FF;
  std::array<uint32_t, 256> palette = [] {
    std::array<uint32_t, 256> p{0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
                                0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF};
    constexpr uint32_t levels[]{0, 95, 135, 175, 215, 255};
    for (size_t i = 16; i < 232; ++i) {
      const size_t n = i - 16;
      p[i] = (levels[n / 36] << 16) | (levels[(n / 6) % 6] << 8) | levels[n % 6];
    }
    for (size_t i = 232; i < p.size(); ++i) {
      const auto v = uint32_t(8 + (i - 232) * 10);
      p[i] = (v << 16) | (v << 8) | v;
    }
    return p;
  }();
  SearchColors search{};
  bool operator==(const Theme&) const = default;
};

inline uint32_t palette256(const Theme& t, uint8_t i) { return t.palette[i]; }

// 快照的生效色（配置 + OSC 覆盖）直接映射到渲染主题。
inline Theme themeFromColors(const itcore_colors& colors, const SearchColors& search) {
  Theme theme;
  theme.fg = colors.foreground; theme.bg = colors.background; theme.cursor = colors.cursor;
  std::copy_n(colors.palette, theme.palette.size(), theme.palette.begin());
  theme.search = search;
  return theme;
}

// 0xTTRRGGBB → 0xRRGGBB；TT=0 默认、1 调色板（RR 为下标）、2 真彩
inline uint32_t resolve(const Theme &t, uint32_t c, bool is_fg) {
  switch (c >> 24) {
    case 1: return palette256(t, uint8_t((c >> 16) & 0xFF));
    case 2: return c & 0xFFFFFF;
    default: return is_fg ? t.fg : t.bg;
  }
}

// SGR 2/5/8/9 与下划线样式位在 itcore.h。没有该宏时用同一组占位位，便于旧头编译。
#ifndef ITCORE_HAS_SGR_EXT_FLAGS
constexpr uint16_t ITREN_CELL_DIM = 1u << 6, ITREN_CELL_BLINK = 1u << 7, ITREN_CELL_HIDDEN = 1u << 8, ITREN_CELL_STRIKE = 1u << 9;
#else
constexpr uint16_t ITREN_CELL_DIM = ITCORE_CELL_DIM, ITREN_CELL_BLINK = ITCORE_CELL_BLINK,
                   ITREN_CELL_HIDDEN = ITCORE_CELL_HIDDEN, ITREN_CELL_STRIKE = ITCORE_CELL_STRIKE;
#endif

inline uint32_t blend50(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (int sh = 0; sh <= 16; sh += 8) r |= ((((a >> sh) & 0xFF) + ((b >> sh) & 0xFF)) / 2) << sh;
  return r;
}

struct ResolvedColors { uint32_t fg, bg, underline; };
// 顺序：DECSCNM ?5 只交换默认色（TT=0）→ INVERSE/SELECTED 交换一次 → DIM 文字色向 bg 混 50% → HIDDEN 文字色=bg。
// 选中的 HIDDEN 仍隐藏，只显示反色背景。BLINK 不影响颜色，见 text_visible。
// 搜索高亮（hit，草案 v0.2 §2.6）优先级：选区 > 当前命中 > 其他命中 > 反显。
//   - SELECTED 时忽略 hit，照旧反色（当前命中经 itcore_search_select 设成选区时按选区显示）。
//   - 否则命中格的文字/背景直接换成高亮色，替代“反显交换”这一步：INVERSE 和 DECSCNM 对命中格不再起作用。
//   - DIM 照常作用（文字色向高亮背景混 50%），与选区路径一致。
//   - HIDDEN 仍隐藏（文字色=高亮背景）：搜索高亮不能把隐藏文字（如密码）显示出来。
//   - 下划线有自定义色（SGR 58）时保留，否则跟随高亮文字色。
// hit == None 时与原来完全一致。
inline ResolvedColors resolve_cell(const Theme &t, const itcore_cell &c, bool reverse_screen = false,
                                   SearchHit hit = SearchHit::None) {
  bool fdef = (c.fg >> 24) == 0, bdef = (c.bg >> 24) == 0;
  uint32_t fg = fdef ? (reverse_screen ? t.bg : t.fg) : resolve(t, c.fg, true);
  uint32_t bg = bdef ? (reverse_screen ? t.fg : t.bg) : resolve(t, c.bg, false);
  if (c.flags & ITCORE_CELL_SELECTED) hit = SearchHit::None;
  if (hit == SearchHit::Current) { fg = t.search.current_fg; bg = t.search.current_bg; }
  else if (hit == SearchHit::Other) { fg = t.search.other_fg; bg = t.search.other_bg; }
  else if (c.flags & (ITCORE_CELL_INVERSE | ITCORE_CELL_SELECTED)) { uint32_t x = fg; fg = bg; bg = x; }  // 选中+INVERSE 只反一次，避免“负负得正”看不出选中
  if (c.flags & ITREN_CELL_DIM) fg = blend50(fg, bg);
  if (c.flags & ITREN_CELL_HIDDEN) fg = bg;
  uint32_t ul = (c.underline_color >> 24) ? resolve(t, c.underline_color, true) : fg;
  if (c.flags & ITREN_CELL_HIDDEN) ul = bg;
  return {fg, bg, ul};
}

// SGR 5：文字按 1 s 亮 / 1 s 灭（blink_phase_ms 为全局闪烁时钟），背景始终绘制
inline bool text_visible(const itcore_cell &c, uint64_t blink_phase_ms, uint32_t half_period_ms = 1000) {
  if (!(c.flags & ITREN_CELL_BLINK) || half_period_ms == 0) return true;
  return ((blink_phase_ms / half_period_ms) % 2) == 0;
}

// SGR 9：删除线在格高一半处，粗细同下划线 max(1, round(2*dpi))；width 为占格数
inline GridMetrics::Rect strike_rect(const GridMetrics &m, uint16_t col, uint16_t row, uint16_t ncells = 1) {
  auto a = m.cell_rect(col, row);
  int t = std::min(std::max(1, int(std::lround(2.0f * m.dpi_scale))), a.h);
  return {a.x, a.y + a.h / 2 - t / 2, a.w * std::max<uint16_t>(ncells, 1), t};
}

// SGR 4：下划线贴格底，粗细与删除线同为 max(1, round(2*dpi))；只在 text_visible 时画（闪烁隐藏相位不画）
inline GridMetrics::Rect underline_rect(const GridMetrics &m, uint16_t col, uint16_t row, uint16_t ncells = 1) {
  auto a = m.cell_rect(col, row);
  int t = std::min(std::max(1, int(std::lround(2.0f * m.dpi_scale))), a.h);
  int gap = std::min(std::max(0, int(std::lround(m.dpi_scale))), a.h - t);  // 离底边约 1 逻辑像素
  return {a.x, a.y + a.h - t - gap, a.w * std::max<uint16_t>(ncells, 1), t};
}

// 下划线是否画：随文本一起受闪烁隐藏（BLINK-02）
inline bool underline_visible(const itcore_cell &c, uint64_t blink_phase_ms) {
  return (c.flags & ITCORE_CELL_UNDERLINE) && text_visible(c, blink_phase_ms);
}

// 0 不画。样式位 0 或超出 1..5 时按单线（旧单元格 / 未知 3-bit 值）。
inline int cell_underline_style(const itcore_cell &c) {
  if (!(c.flags & ITCORE_CELL_UNDERLINE)) return 0;
#ifndef ITCORE_CELL_UL_STYLE_MASK
  return 1;
#else
  int st = (c.flags & ITCORE_CELL_UL_STYLE_MASK) >> ITCORE_CELL_UL_STYLE_SHIFT;
  if (st < ITCORE_UL_SINGLE || st > ITCORE_UL_DASHED) return ITCORE_UL_SINGLE;
  return st;
#endif
}

inline bool overline_visible(const itcore_cell &c, uint64_t blink_phase_ms) {
  return (c.flags & ITCORE_CELL_OVERLINE) && text_visible(c, blink_phase_ms);
}

// 线宽 max(1, round(2*dpi))，离边约 1 逻辑像素，与 underline_rect 相同。
inline void ul_metrics(const GridMetrics &m, const GridMetrics::Rect &a, int *t, int *gap) {
  *t = std::min(std::max(1, int(std::lround(2.0f * m.dpi_scale))), a.h);
  *gap = std::min(std::max(0, int(std::lround(m.dpi_scale))), std::max(0, a.h - *t));
}

// SGR 4:1..5 / 21。单线与 underline_rect 重合。双线在其上一笔宽的空白再画一条。
// 点线：边长 t、间隔 t。虚线：段长 max(3t,1)、间隔 t。波浪：周期 max(4t,4)、振幅 t、步长 t，y 钳在格内。
inline std::vector<GridMetrics::Rect> underline_style_rects(const GridMetrics &m, uint16_t col, uint16_t row,
                                                            uint16_t ncells, int style) {
  auto a = m.cell_rect(col, row);
  const int w = a.w * std::max<uint16_t>(ncells, 1);
  int t = 1, gap = 0;
  ul_metrics(m, a, &t, &gap);
  int y1 = a.y + a.h - t - gap;
  if (y1 < a.y) y1 = a.y;
  std::vector<GridMetrics::Rect> out;
  auto push_bar = [&](int x, int y, int bw, int bh) {
    if (bw <= 0 || bh <= 0) return;
    if (y < a.y) { bh -= a.y - y; y = a.y; }
    if (y + bh > a.y + a.h) bh = a.y + a.h - y;
    if (bh > 0) out.push_back({x, y, bw, bh});
  };
  if (style == ITCORE_UL_DOUBLE) {
    push_bar(a.x, y1, w, t);
    push_bar(a.x, y1 - t - t, w, t);
  } else if (style == ITCORE_UL_DOTTED) {
    const int step = t + t;
    for (int x = 0; x < w; x += step) push_bar(a.x + x, y1, std::min(t, w - x), t);
  } else if (style == ITCORE_UL_DASHED) {
    const int seg = std::max(t * 3, 1);
    const int g = std::max(t, 1);
    for (int x = 0; x < w; x += seg + g) push_bar(a.x + x, y1, std::min(seg, w - x), t);
  } else if (style == ITCORE_UL_WAVY) {
    const int period = std::max(4 * t, 4);
    const int step = std::max(t, 1);
    for (int x = 0; x < w; x += step) {
      const double phase = (2.0 * 3.14159265358979323846 * x) / period;
      const int yy = y1 + int(std::lround(std::sin(phase) * t));
      push_bar(a.x + x, yy, std::min(step, w - x), t);
    }
  } else {
    push_bar(a.x, y1, w, t);  // single，以及未知样式
  }
  return out;
}

// SGR 53：格顶，粗细与下划线相同。颜色由调用方用前景。
inline GridMetrics::Rect overline_rect(const GridMetrics &m, uint16_t col, uint16_t row, uint16_t ncells = 1) {
  auto a = m.cell_rect(col, row);
  int t = 1, gap = 0;
  ul_metrics(m, a, &t, &gap);
  return {a.x, a.y + gap, a.w * std::max<uint16_t>(ncells, 1), t};
}

// SGR 5 闪烁的行级跟踪：重画某行时记录它是否含闪烁格；相位翻转只需重画这些行；全部消失后停止动画
class BlinkRows {
 public:
  void resize(uint16_t rows) { if (rows != has_.size()) { has_.assign(rows, 0); count_ = 0; } }
  void clear() { has_.assign(has_.size(), 0); count_ = 0; }
  void set(uint16_t row, bool blink) {
    if (row >= has_.size() || bool(has_[row]) == blink) return;
    has_[row] = blink; count_ += blink ? 1 : -1;
  }
  bool any() const { return count_ > 0; }
  // 相位翻转时要重画的行
  std::vector<uint16_t> rows() const {
    std::vector<uint16_t> out;
    for (uint16_t y = 0; y < has_.size(); ++y) if (has_[y]) out.push_back(y);
    return out;
  }
 private:
  std::vector<uint8_t> has_;
  int count_ = 0;
};

// 脏行追踪：返回需要重画的行；尺寸或 generation 变化时全部重画
class DirtyRows {
 public:
  std::vector<uint16_t> collect(const itcore_snapshot *s) {
    uint16_t rows = itcore_snapshot_rows(s);
    uint64_t gen = itcore_snapshot_generation(s), ep = itcore_snapshot_epoch(s);
    // generation（尺寸）或 epoch（字素/链接 id 重编号）任一变化都整屏重画
    bool all = rows != rev_.size() || gen != gen_ || ep != epoch_ || cols_ != itcore_snapshot_columns(s);
    if (all) { rev_.assign(rows, UINT64_MAX); gen_ = gen; epoch_ = ep; cols_ = itcore_snapshot_columns(s); }
    std::vector<uint16_t> out;
    for (uint16_t y = 0; y < rows; ++y) {
      itcore_wrap w; uint64_t rr = 0;
      itcore_snapshot_row(s, y, &w, &rr);
      if (rr != rev_[y]) { out.push_back(y); rev_[y] = rr; }
    }
    return out;
  }
  void invalidate() { rev_.clear(); }  // 设备丢失 / DPI / 字体变化
  void mark(uint16_t row) { if (row < rev_.size()) rev_[row] = UINT64_MAX; }  // 单行强制重画（闪烁相位）
 private:
  std::vector<uint64_t> rev_;
  uint64_t gen_ = UINT64_MAX, epoch_ = UINT64_MAX;
  uint16_t cols_ = 0;
};

// present_frame 的闪烁/脏行规划（BLINK-03）。full：整屏重画（清闪烁行表）；否则相位翻转把
// 闪烁行表里的行标脏，再按行 revision 收集。滚动后行 revision 变了的行会被重画，重画时
// blink_note_row 按该行的新内容重设闪烁标记——闪烁行滚走后原位置不再标脏。
inline std::vector<uint16_t> blink_frame_rows(DirtyRows &dirty, BlinkRows &blink, bool &last_blink_on,
                                              const itcore_snapshot *s, uint16_t rows, bool full, bool blink_on) {
  blink.resize(rows);
  if (full) { dirty.invalidate(); blink.clear(); }
  else if (blink_on != last_blink_on) for (uint16_t y : blink.rows()) dirty.mark(y);
  last_blink_on = blink_on;
  return dirty.collect(s);
}
// 重画第 y 行后更新闪烁行表；cells 为空（布局外）视为无闪烁
inline void blink_note_row(BlinkRows &blink, uint16_t y, const itcore_cell *cells, uint16_t cols) {
  bool any = false;
  for (uint16_t x = 0; cells && x < cols && !any; ++x)
    any = !(cells[x].flags & ITCORE_CELL_CONTINUATION) && (cells[x].flags & ITREN_CELL_BLINK);
  blink.set(y, any);
}

}  // namespace itren
