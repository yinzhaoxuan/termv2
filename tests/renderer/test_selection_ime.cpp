#include "selection_hit.h"
#include "ime_layout.h"
#include "itcore_mock.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace itren;
int main() {
  GridMetrics m; m.cell_w_dip = 8; m.cell_h_dip = 16;  // pad 4
  // 普通格：左半 / 右半
  auto h = selection_hit(m, 4 + 8 * 2 + 1, 4 + 16 + 3, 10, 5); assert(h.row == 1 && h.col == 2 && !h.after && h.boundary == 2);
  h = selection_hit(m, 4 + 8 * 2 + 5, 20, 10, 5); assert(h.after && h.boundary == 3 && h.autoscroll_dir == 0);
  // 宽字符：行 0 = a 中 中' b
  const char *t = "a中b"; auto *s = itmock_snapshot_from_utf8(t, strlen(t), 10, 5);
  h = selection_hit(m, 4 + 8 * 2 + 1, 5, 10, 5, s); assert(h.col == 1 && h.after && h.boundary == 3);  // 尾格左侧 → 宽字符右半
  h = selection_hit(m, 4 + 8 + 2, 5, 10, 5, s); assert(h.col == 1 && !h.after && h.boundary == 1);
  h = selection_hit(m, 4 + 8 * 2 + 0, 5, 10, 5, s); assert(h.after && h.boundary == 3);  // 正中线算右半
  // 越界
  h = selection_hit(m, 50, 0, 10, 5); assert(h.row == 0 && h.col == 0 && h.boundary == 0 && h.autoscroll_dir == -1 && h.autoscroll_lines == 1);
  h = selection_hit(m, 50, 4 - 40, 10, 5); assert(h.autoscroll_lines == 3);
  h = selection_hit(m, 50, -100000, 10, 5); assert(h.autoscroll_lines == 10);
  int bottom = 4 + 5 * 16;
  h = selection_hit(m, 0, bottom, 10, 5); assert(h.row == 4 && h.boundary == 10 && h.after && h.autoscroll_dir == 1 && h.autoscroll_lines == 1);
  h = selection_hit(m, 0, bottom + 33, 10, 5, nullptr, 2); assert(h.autoscroll_lines == 2);
  h = selection_hit(m, -30, 40, 10, 5); assert(h.row == 2 && h.col == 0 && h.boundary == 0 && h.autoscroll_dir == 0);
  h = selection_hit(m, 9999, 40, 10, 5); assert(h.col == 9 && h.boundary == 10);
  itcore_snapshot_release(s);
  // selection_rects
  auto rs = selection_rects(m, {{0, 3, 10}, {1, 0, 4}, {1, 4, 10}, {2, 0, 10}, {3, 0, 5}, {3, 2, 6}, {4, 1, 1}});
  assert(rs.size() == 3);
  assert(rs[0].x == 4 + 24 && rs[0].w == 56 && rs[0].h == 16);
  assert(rs[1].x == 4 && rs[1].y == 20 && rs[1].w == 80 && rs[1].h == 32);  // 行 1-2 合并
  assert(rs[2].y == 52 && rs[2].w == 48);
  assert(selection_rects(m, {}).empty());

  // IME：caret (1,7)，cols 10，[中 文 a 字] 宽 2 2 1 2
  std::vector<uint8_t> w = {2, 2, 1, 2};
  std::vector<ClauseAttr> at = {ClauseAttr::Converted, ClauseAttr::Target, ClauseAttr::Target, ClauseAttr::Input};
  auto L = ime_layout(m, 1, 7, w, at, 2, 10, 5);
  assert(L.cells[0].row == 1 && L.cells[0].col == 7);
  assert(L.cells[1].row == 2 && L.cells[1].col == 0);   // 7+2=9, 9+2>10 换行，不拆宽字符
  assert(L.cells[2].row == 2 && L.cells[2].col == 2 && L.cells[3].col == 3 && !L.overflow);
  assert(L.underlines.size() == 3);
  assert(L.underlines[0].h == 1 && L.underlines[0].w == 16);
  assert(L.underlines[1].h == 2 && L.underlines[1].w == 24 && L.underlines[1].x == 4);   // 目标子句粗线
  assert(L.underlines[2].h == 1);
  assert(L.caret.x == 4 + 16 && L.caret.y == 4 + 32 && L.caret.w == 2);  // 光标在 cluster 2 前
  assert(L.anchor.x == L.caret.x && L.anchor.y == L.caret.y && L.anchor.w == 8);
  L = ime_layout(m, 1, 7, w, at, 4, 10, 5); assert(L.caret.x == 4 + 5 * 8);  // 末尾
  // 恰好到行尾
  L = ime_layout(m, 0, 8, {2}, {}, 1, 10, 5); assert(L.cells[0].col == 8 && L.caret.y == 20 && L.caret.x == 4);
  // 溢出底行：锚点留在网格内
  L = ime_layout(m, 4, 8, {2, 2, 2}, {}, 3, 10, 5);
  assert(L.overflow && L.cells[1].row == 5);
  assert(L.anchor.y == 4 + 4 * 16 && L.anchor.x <= 4 + 9 * 8);
  // 空预编辑：锚点=caret
  L = ime_layout(m, 2, 3, {}, {}, 0, 10, 5); assert(L.cells.empty() && L.anchor.x == 28 && L.anchor.y == 36);
  // 150% DPI 线宽
  GridMetrics m2 = m; m2.dpi_scale = 1.5f;
  L = ime_layout(m2, 0, 0, {1, 1}, {ClauseAttr::Input, ClauseAttr::Target}, 0, 10, 5);
  assert(L.underlines[0].h == 2 && L.underlines[1].h == 3);
  std::puts("selection_ime ok");
}
