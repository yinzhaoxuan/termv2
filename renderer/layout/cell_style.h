/* 平台无关的颜色与脏行解析（DESIGN-T06 §3.2、§3.6）。D2D 后端只消费这里的结果。 */
#pragma once
#include "itcore.h"
#include "grid_metrics.h"
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace itren {

struct Theme {
  uint32_t fg = 0xD4D4D4, bg = 0x1E1E1E;   // 0xRRGGBB
  std::array<uint32_t, 16> ansi = {0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
                                   0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF};
};

// 256 色：0-15 主题，16-231 为 6x6x6 立方，232-255 为灰阶（xterm 标准）
inline uint32_t palette256(const Theme &t, uint8_t i) {
  if (i < 16) return t.ansi[i];
  if (i < 232) {
    static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
    uint8_t n = uint8_t(i - 16);
    return (uint32_t(lv[n / 36]) << 16) | (uint32_t(lv[(n / 6) % 6]) << 8) | lv[n % 6];
  }
  uint8_t g = uint8_t(8 + (i - 232) * 10);
  return (uint32_t(g) << 16) | (uint32_t(g) << 8) | g;
}

// 0xTTRRGGBB → 0xRRGGBB；TT=0 默认、1 调色板（RR 为下标）、2 真彩
inline uint32_t resolve(const Theme &t, uint32_t c, bool is_fg) {
  switch (c >> 24) {
    case 1: return palette256(t, uint8_t((c >> 16) & 0xFF));
    case 2: return c & 0xFFFFFF;
    default: return is_fg ? t.fg : t.bg;
  }
}

// SGR 2/5/8/9 标志：itcore.h 尚未定义（缺口，待架构师在 itcore_cell.flags 补位），先用本地占位位值
#ifndef ITCORE_HAS_SGR_EXT_FLAGS
enum : uint16_t { ITREN_CELL_DIM = 1u << 6, ITREN_CELL_BLINK = 1u << 7, ITREN_CELL_HIDDEN = 1u << 8, ITREN_CELL_STRIKE = 1u << 9 };
#else
enum : uint16_t { ITREN_CELL_DIM = ITCORE_CELL_DIM, ITREN_CELL_BLINK = ITCORE_CELL_BLINK,
                  ITREN_CELL_HIDDEN = ITCORE_CELL_HIDDEN, ITREN_CELL_STRIKE = ITCORE_CELL_STRIKE };
#endif

inline uint32_t blend50(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (int sh = 0; sh <= 16; sh += 8) r |= ((((a >> sh) & 0xFF) + ((b >> sh) & 0xFF)) / 2) << sh;
  return r;
}

struct ResolvedColors { uint32_t fg, bg, underline; };
// 顺序：DECSCNM ?5 只交换默认色（TT=0）→ INVERSE/SELECTED 交换一次 → DIM 文字色向 bg 混 50% → HIDDEN 文字色=bg。
// 选中的 HIDDEN 仍隐藏，只显示反色背景。BLINK 不影响颜色，见 text_visible。
inline ResolvedColors resolve_cell(const Theme &t, const itcore_cell &c, bool reverse_screen = false) {
  bool fdef = (c.fg >> 24) == 0, bdef = (c.bg >> 24) == 0;
  uint32_t fg = fdef ? (reverse_screen ? t.bg : t.fg) : resolve(t, c.fg, true);
  uint32_t bg = bdef ? (reverse_screen ? t.fg : t.bg) : resolve(t, c.bg, false);
  if (c.flags & (ITCORE_CELL_INVERSE | ITCORE_CELL_SELECTED)) { uint32_t x = fg; fg = bg; bg = x; }  // 选中+INVERSE 只反一次，避免“负负得正”看不出选中
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
 private:
  std::vector<uint64_t> rev_;
  uint64_t gen_ = UINT64_MAX, epoch_ = UINT64_MAX;
  uint16_t cols_ = 0;
};

}  // namespace itren
