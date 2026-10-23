#undef NDEBUG  // assert 在任何构建类型下都必须执行
#include "itcore_mock.h"
#include "grid_metrics.h"
#include <cassert>
#include <cstdio>
#include <cmath>
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
  // BUG-01：常见等宽字体（设计单位取近似值，口径同 DirectWrite GetMetrics / GetDesignGlyphMetrics("M")）在
  // 96/144/192 DPI 下：格高 >= ceil((ascent+descent)×scale)，格宽 = round(advance×scale)，基线落在格内且留出 descent
  {
    struct F { const char *name; float upem, asc, desc, gap, adv_m; } fonts[] = {
        {"Consolas", 2048, 1884, 514, 0, 1126}, {"Cascadia Mono", 2048, 1900, 480, 0, 1200},
        {"Courier New", 2048, 1705, 615, 0, 1229}, {"Lucida Console", 2048, 1616, 432, 0, 1234},
        {"Sarasa Mono SC", 1000, 880, 120, 0, 500},
        {"Gap Mono (lineGap≠0)", 2048, 1638, 410, 300, 1229}};
    for (float pt : {10.f, 12.f, 14.f})
      for (auto &f : fonts) {
        const float k = pt * (96.f / 72.f) / f.upem;
        itren::FontMetricsDip fm{f.adv_m * k, f.asc * k, f.desc * k, f.gap * k, true};
        for (float sc : {1.f, 1.5f, 2.f}) {
          auto m = itren::GridMetrics::from_font(fm, sc);
          const int need = int(std::ceil((fm.ascent + fm.descent) * sc - 1e-3f));
          assert(m.cell_h_px() >= need);
          assert(m.cell_h_px() > 10);  // 旧的写死 10 DIP 行高在 96 DPI 下必然裁字
          assert(m.cell_w_px() == int(std::lround(fm.advance * sc)));
          const float bl = itren::GridMetrics::baseline_px(fm, sc);
          assert(bl > 0 && bl + fm.descent * sc <= m.cell_h_px() + 1.f);
          assert(bl == std::round(fm.ascent * sc));  // lineGap 不挪基线
          if (f.gap > 0) {  // 行距留白在字形下方：格高包含 gap，基线+descent 之后还剩 gap
            assert(m.cell_h_px() >= int(std::ceil((fm.ascent + fm.descent + fm.line_gap) * sc - 1e-3f)));
            assert(bl + fm.descent * sc <= m.cell_h_px() - fm.line_gap * sc + 1.f);
          }
        }
      }
    // 估算值标记为未实测（后端据此打日志）
    assert(!itren::estimate_font_metrics(12.f).measured);
  }
  puts("renderer mock/layout tests passed");
}
