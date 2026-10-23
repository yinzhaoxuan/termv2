#undef NDEBUG  // assert 在任何构建类型下都必须执行
#include "itcore_mock.h"
#include "grid_metrics.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  const char *txt = "ab中文\nhello";
  auto *s = itmock_snapshot_from_utf8(txt, strlen(txt), 20, 5);
  itcore_wrap w; uint64_t rv;
  auto *row = itcore_snapshot_row(s, 0, &w, &rv);
  assert(row[2].width == 2 && !(row[2].flags & ITCORE_CELL_CONTINUATION));
  assert(row[3].flags & ITCORE_CELL_CONTINUATION && row[3].grapheme_id == row[2].grapheme_id);
  size_t n; auto *g = itcore_snapshot_grapheme(s, row[2].grapheme_id, &n);
  assert(n == 3 && memcmp(g, "中", 3) == 0);
  uint16_t cc, cr; int vis; itcore_snapshot_cursor(s, &cc, &cr, &vis);
  assert(cr == 1 && cc == 5);
  itcore_snapshot_release(s);

  // 宽字符贴右边界：放不下则换行并标记 wide-wrap（CORE-03 渲染侧）
  const char *edge = "abc中";
  s = itmock_snapshot_from_utf8(edge, strlen(edge), 4, 2);
  itcore_snapshot_row(s, 0, &w, &rv); assert(w == ITCORE_WRAP_WIDE);
  row = itcore_snapshot_row(s, 1, &w, &rv); assert(row[0].width == 2);
  itcore_snapshot_release(s);

  // DPI 100/150/200%：命中测试往返不漂移（DPI-01 渲染侧）
  for (float sc : {1.0f, 1.5f, 2.0f}) {
    auto m = itren::GridMetrics::from_font(7.8f, 12.0f, 3.5f, 0.0f, sc);
    for (uint16_t r = 0; r < 40; r++) for (uint16_t c = 0; c < 120; c++) {
      auto rc = m.cell_rect(c, r);
      uint16_t hc, hr; m.hit_test(rc.x + rc.w / 2, rc.y + rc.h / 2, 120, 40, hc, hr);
      assert(hc == c && hr == r);
    }
    uint16_t cols, rows; m.grid_size(100, 50, cols, rows); assert(cols == 20 && rows == 5);
  }
  puts("renderer mock/layout tests passed");
}
