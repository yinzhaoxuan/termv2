/* 平台无关的颜色与脏行解析（DESIGN-T06 §3.2、§3.6）。D2D 后端只消费这里的结果。 */
#pragma once
#include "itcore.h"
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

struct ResolvedColors { uint32_t fg, bg, underline; };
// INVERSE 或 SELECTED 交换前景与背景，二者叠加也只交换一次（背景段合并也必须用这里的 bg）
inline ResolvedColors resolve_cell(const Theme &t, const itcore_cell &c) {
  uint32_t fg = resolve(t, c.fg, true), bg = resolve(t, c.bg, false);
  if (c.flags & (ITCORE_CELL_INVERSE | ITCORE_CELL_SELECTED)) { uint32_t x = fg; fg = bg; bg = x; }  // 选中+INVERSE 只反一次，避免“负负得正”看不出选中
  uint32_t ul = (c.underline_color >> 24) ? resolve(t, c.underline_color, true) : fg;
  return {fg, bg, ul};
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
