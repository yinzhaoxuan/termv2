/* IME 预编辑（preedit）内联布局与候选窗锚点（平台无关）。只产出覆盖层几何，绝不写入核心。
   cluster 宽度由调用方按核心宽度规则给出（1 或 2）。 */
#pragma once
#include "grid_metrics.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace itren {

enum class ClauseAttr : uint8_t { Input = 0, Target = 1, Converted = 2 };  // 未转换 / 当前目标子句 / 已转换
struct PreeditCell { uint16_t row, col; uint8_t width; };  // row 可能 ≥ rows（越过底行，只用于判断溢出）
struct ImeLayout {
  std::vector<PreeditCell> cells;               // 每个 cluster 的位置
  std::vector<GridMetrics::Rect> underlines;    // 每行按属性合并
  GridMetrics::Rect caret{};                    // 预编辑内光标（竖线）
  GridMetrics::Rect anchor{};                   // 预编辑内光标所在格（cursor_index 处；缺省为预编辑末尾），始终在网格内
  // 候选窗锚点（ImmSetCandidateWindow / TSF GetTextExt 按 Windows 惯例用此值）：第一个 Target 子句 cluster 的格矩形，
  // 无 Target 时为第一个 cluster（组合起点）；空预编辑为光标格。与 anchor 同样夹在网格内（越过底行 → 末行末列单格）。
  GridMetrics::Rect composition_anchor{};
  bool overflow = false;                        // 预编辑超出底行
};

// attrs 可短于 widths（缺省按 Input）；cursor_index ∈ [0, n]
inline ImeLayout ime_layout(const GridMetrics &m, uint16_t caret_row, uint16_t caret_col,
                            const std::vector<uint8_t> &widths, const std::vector<ClauseAttr> &attrs,
                            size_t cursor_index, uint16_t cols, uint16_t rows) {
  ImeLayout L;
  float d = m.dpi_scale;
  int thin = std::max(1, int(std::lround(d))), thick = std::max(1, int(std::lround(2 * d)));
  int r = caret_row, c = std::min<int>(caret_col, cols - 1);
  for (uint8_t w0 : widths) {
    int w = std::clamp<int>(w0, 1, 2);
    if (w > cols) w = 1;
    if (c + w > cols) { ++r; c = 0; }  // 宽字符不跨行拆分
    L.cells.push_back({uint16_t(r), uint16_t(c), uint8_t(w)});
    c += w;
  }
  auto pos_at = [&](size_t i, int &pr, int &pc) {
    if (i < L.cells.size()) { pr = L.cells[i].row; pc = L.cells[i].col; return; }
    if (L.cells.empty()) { pr = caret_row; pc = std::min<int>(caret_col, cols - 1); return; }
    auto &b = L.cells.back(); pr = b.row; pc = b.col + b.width;
    if (pc >= cols) { pr++; pc = 0; }
  };
  for (auto &pc : L.cells) if (pc.row >= rows) L.overflow = true;
  // 下划线：同一行、同属性的连续 cluster 合并
  for (size_t i = 0; i < L.cells.size();) {
    auto a = i < attrs.size() ? attrs[i] : ClauseAttr::Input;
    size_t j = i + 1;
    while (j < L.cells.size() && L.cells[j].row == L.cells[i].row && (j < attrs.size() ? attrs[j] : ClauseAttr::Input) == a) ++j;
    auto &f = L.cells[i], &l = L.cells[j - 1];
    auto rc = m.cell_rect(f.col, f.row);
    int t = a == ClauseAttr::Target ? thick : thin;
    L.underlines.push_back({rc.x, rc.y + rc.h - t, (l.col + l.width - f.col) * rc.w, t});
    i = j;
  }
  int pr, pc; pos_at(std::min(cursor_index, L.cells.size()), pr, pc);
  auto cr = m.cell_rect(uint16_t(pc), uint16_t(pr));
  L.caret = {cr.x, cr.y, std::max(1, int(std::lround(2 * d))), cr.h};
  int ar = std::min<int>(pr, rows - 1), ac = pr >= rows ? cols - 1 : std::min<int>(pc, cols - 1);
  L.anchor = m.cell_rect(uint16_t(ac), uint16_t(ar));
  size_t ti = 0;
  for (size_t i = 0; i < L.cells.size() && i < attrs.size(); ++i)
    if (attrs[i] == ClauseAttr::Target) { ti = i; break; }
  if (ti < L.cells.size() && L.cells[ti].row < rows) {
    auto &tc = L.cells[ti];
    L.composition_anchor = m.cell_rect(tc.col, tc.row, tc.width);
  } else {
    int qr, qc; pos_at(ti, qr, qc);  // 空预编辑 → 光标格
    int zr = std::min<int>(qr, rows - 1), zc = qr >= rows ? cols - 1 : std::min<int>(qc, cols - 1);
    L.composition_anchor = m.cell_rect(uint16_t(zc), uint16_t(zr));
  }
  return L;
}

}  // namespace itren
