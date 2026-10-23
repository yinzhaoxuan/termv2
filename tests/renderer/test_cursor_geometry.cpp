#include "cursor_geometry.h"
#include "itcore_mock.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace itren;
static GridMetrics M(float d) { GridMetrics m; m.dpi_scale = d; m.cell_w_dip = 8; m.cell_h_dip = 16; return m; }
int main() {
  // DECSCUSR 映射
  int sh[7] = {0, 0, 0, 1, 1, 2, 2}; bool bl[7] = {1, 1, 0, 1, 0, 1, 0};
  for (int p = 0; p <= 6; ++p) { auto s = decscusr_to_style(p); assert(int(s.shape) == sh[p] && s.blink == bl[p]); }
  assert(decscusr_to_style(99).shape == CursorShape::Block && decscusr_to_style(-1).blink);
  // 几何 @100%：格 8x16，pad 4
  auto m = M(1.0f);
  auto r = cursor_rects(m, 1, 2, decscusr_to_style(2), true, false, 1.0f);
  assert(r.size() == 1 && r[0].x == 20 && r[0].y == 20 && r[0].w == 8 && r[0].h == 16);
  r = cursor_rects(m, 1, 2, decscusr_to_style(2), true, true, 1.0f); assert(r[0].w == 16);  // 宽字符 2 格
  r = cursor_rects(m, 1, 2, decscusr_to_style(4), true, false, 1.0f);
  assert(r.size() == 1 && r[0].h == 2 && r[0].y == 20 + 16 - 2 && r[0].w == 8);
  r = cursor_rects(m, 1, 2, decscusr_to_style(6), true, false, 1.0f); assert(r[0].w == 2 && r[0].h == 16 && r[0].x == 20);
  // 失焦：块变空心 4 条，下划线/竖线不变
  r = cursor_rects(m, 1, 2, decscusr_to_style(2), false, false, 1.0f);
  assert(r.size() == 4 && r[0].h == 1 && r[2].w == 1 && r[3].x == 27 && r[2].h == 14);
  assert(cursor_rects(m, 1, 2, decscusr_to_style(6), false, false, 1.0f).size() == 1);
  // 150%：格 12x24，线 3 px，框 2 px；125%：线 round(2.5)=3
  auto m15 = M(1.5f);
  r = cursor_rects(m15, 0, 0, decscusr_to_style(4), true, false, 1.5f); assert(r[0].h == 3 && r[0].w == 12);
  r = cursor_rects(m15, 0, 0, decscusr_to_style(1), false, false, 1.5f); assert(r[0].h == 2);
  assert(line_thickness_px(0.25f) == 1 && outline_thickness_px(0.4f) == 1);
  assert(cursor_inverts_glyph(decscusr_to_style(2), true) && !cursor_inverts_glyph(decscusr_to_style(2), false));
  assert(!cursor_inverts_glyph(decscusr_to_style(5), true));
  // 闪烁
  auto b = decscusr_to_style(1), st = decscusr_to_style(2);
  assert(cursor_visible(b, true, 100) && !cursor_visible(b, true, 600) && cursor_visible(b, true, 1100));
  assert(cursor_visible(st, true, 600) && cursor_visible(b, false, 600));
  // 链接
  const char *t = "ab中文cd\nxyz";  // 行 0: a b 中 中' 文 文' c d
  auto *s = itmock_snapshot_from_utf8(t, strlen(t), 8, 3);
  itmock_set_hyperlink(s, 0, 2, 6, 7, "https://x");
  const char *u; size_t ul;
  assert(itcore_snapshot_hyperlink(s, 7, &u, &ul, nullptr, nullptr) == 1 && ul == 9);
  assert(itcore_snapshot_hyperlink(s, 0, &u, &ul, nullptr, nullptr) == 0);
  auto sp = hover_link_span(s, 0, 3); assert(sp.id == 7 && sp.c0 == 2 && sp.c1 == 6);  // 宽字符尾格
  sp = hover_link_span(s, 0, 0); assert(sp.id == 0 && sp.c0 == sp.c1);
  auto ur = underline_rect(m, hover_link_span(s, 0, 5), 1.0f);
  assert(ur.x == 4 + 16 && ur.w == 32 && ur.h == 2 && ur.y == 4 + 14);
  // 跨行：行 0 链接到行尾，行 1 从行首开始
  itmock_set_hyperlink(s, 0, 6, 8, 9, "https://y"); itmock_set_hyperlink(s, 1, 0, 2, 9, nullptr);
  auto rows = hover_link_rows(s, 1, 1);
  assert(rows.size() == 2 && rows[0].row == 0 && rows[0].c0 == 6 && rows[0].c1 == 8 && rows[1].row == 1 && rows[1].c1 == 2);
  assert(hover_link_rows(s, 0, 2).size() == 1);  // id 7 不跨行
  // 宽字符尾格未单独带 id 也算在段内
  itmock_set_hyperlink(s, 0, 0, 8, 0, nullptr); itmock_set_hyperlink(s, 0, 4, 5, 5, "https://z");
  sp = hover_link_span(s, 0, 5); assert(sp.c0 == 4 && sp.c1 == 6);
  itcore_snapshot_release(s);
  std::puts("cursor_geometry ok");
}
