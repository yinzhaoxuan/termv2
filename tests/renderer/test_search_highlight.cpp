#undef NDEBUG  // assert 在任何构建类型下都必须执行
// SRCH-HL：搜索高亮层（建表健壮性、O(1) 查询、清除、与 resolve_cell 的叠加顺序、脏行）
#include "cell_style.h"
#include "search_highlight.h"
#include <cassert>
#include <cstdio>
using namespace itren;
static SearchRange R(int64_t row, uint16_t a, uint16_t b, bool cur = false) { SearchRange r; r.row = row; r.start_col = a; r.end_col = b; r.is_current = cur; return r; }

int main() {
  // 1. 建表：视口首行绝对行号 100，5 行 10 列；无序、重叠、越界、空区间
  SearchHighlight h;
  std::vector<SearchRange> in = {
      R(103, 6, 9), R(101, 2, 5), R(101, 4, 7, true),  // 重叠：Current 优先
      R(99, 0, 10), R(105, 0, 10),                      // 视口上方/下方
      R(102, 8, 40),                                    // 右越界：裁到 cols
      R(102, 3, 3), R(102, 5, 2),                       // 空区间 / 反向
      R(104, 20, 30),                                   // 整段在列外
  };
  h.build(in, 100, 5, 10);
  assert(h.any() && h.rows() == 5 && h.cols() == 10);
  assert(h.kind(1, 1) == SearchHit::None && h.kind(1, 2) == SearchHit::Other && h.kind(1, 3) == SearchHit::Other);
  assert(h.kind(1, 4) == SearchHit::Current && h.kind(1, 6) == SearchHit::Current && h.kind(1, 7) == SearchHit::None);
  assert(h.kind(3, 6) == SearchHit::Other && h.kind(3, 8) == SearchHit::Other && h.kind(3, 9) == SearchHit::None);
  assert(h.kind(2, 8) == SearchHit::Other && h.kind(2, 9) == SearchHit::Other);
  for (uint16_t x = 0; x < 8; ++x) assert(h.kind(2, x) == SearchHit::None);
  for (uint16_t x = 0; x < 10; ++x) assert(h.kind(0, x) == SearchHit::None && h.kind(4, x) == SearchHit::None);
  assert(h.row_any(1) && h.row_any(2) && h.row_any(3) && !h.row_any(0) && !h.row_any(4));
  assert(h.kind(5, 0) == SearchHit::None && h.kind(0, 10) == SearchHit::None && h.kind(60000, 60000) == SearchHit::None);
  // 负绝对行号 / 空输入 / 0 列不崩
  SearchHighlight z; z.build(nullptr, 0, 0, 5, 10); assert(!z.any() && z.kind(0, 0) == SearchHit::None);
  z.build({R(-3, 0, 2)}, -5, 3, 4); assert(z.kind(2, 1) == SearchHit::Other);
  z.build({R(0, 0, 2)}, 0, 3, 0); assert(!z.any());

  // 2. 宽字符：首格 4 的宽字符 → end_col = 6，首尾两格都命中
  SearchHighlight w; w.build({R(0, 4, 6)}, 0, 1, 10);
  assert(w.kind(0, 4) == SearchHit::Other && w.kind(0, 5) == SearchHit::Other && w.kind(0, 6) == SearchHit::None);

  // 3. clear（RESTARTED）
  SearchHighlight before = h;
  h.clear();
  assert(!h.any() && h.kind(1, 4) == SearchHit::None && !h.row_any(1) && h.rows() == 5);

  // 5. 脏行：清除后旧命中行 1/2/3 重画；尺寸变化全部重画；相同则无
  auto d = SearchHighlight::dirty_rows(before, h);
  assert((d == std::vector<uint16_t>{1, 2, 3}));
  SearchHighlight moved; moved.build({R(101, 2, 5), R(101, 4, 7, true), R(102, 8, 40), R(103, 6, 9), R(104, 0, 1)}, 100, 5, 10);
  assert((SearchHighlight::dirty_rows(before, moved) == std::vector<uint16_t>{4}));
  SearchHighlight cur2; cur2.build({R(101, 2, 7), R(102, 8, 40), R(103, 6, 9, true)}, 100, 5, 10);  // 当前命中换到第 3 行
  assert((SearchHighlight::dirty_rows(before, cur2) == std::vector<uint16_t>{1, 3}));
  assert(SearchHighlight::dirty_rows(before, before).empty());
  SearchHighlight big; big.build(nullptr, 0, 0, 6, 10);
  assert(SearchHighlight::dirty_rows(before, big).size() == 6);

  // 4. resolve_cell 叠加：选区 > 当前 > 其他 > 反显
  Theme t;
  const auto &sc = t.search;
  assert(sc.other_bg == 0xFFFF00 && sc.other_fg == 0 && sc.current_bg == 0xFF9632 && sc.current_fg == 0);
  itcore_cell c{}; c.fg = 0x02FF0000; c.bg = 0x020000FF;
  auto base = resolve_cell(t, c);
  auto none = resolve_cell(t, c, false, SearchHit::None);
  assert(none.fg == base.fg && none.bg == base.bg && none.underline == base.underline);  // None 不变
  auto o = resolve_cell(t, c, false, SearchHit::Other);
  assert(o.fg == sc.other_fg && o.bg == sc.other_bg && o.underline == sc.other_fg);
  auto cu = resolve_cell(t, c, false, SearchHit::Current);
  assert(cu.fg == sc.current_fg && cu.bg == sc.current_bg);
  c.flags = ITCORE_CELL_INVERSE;                                             // 命中 > 反显
  o = resolve_cell(t, c, false, SearchHit::Other); assert(o.fg == sc.other_fg && o.bg == sc.other_bg);
  o = resolve_cell(t, c, true, SearchHit::Other); assert(o.fg == sc.other_fg && o.bg == sc.other_bg);  // DECSCNM 也不影响
  c.flags = ITCORE_CELL_SELECTED;                                            // 选区 > 命中
  auto s0 = resolve_cell(t, c), s1 = resolve_cell(t, c, false, SearchHit::Current);
  assert(s1.fg == s0.fg && s1.bg == s0.bg && s1.fg == 0x0000FF && s1.bg == 0xFF0000);
  c.flags = ITCORE_CELL_SELECTED | ITCORE_CELL_INVERSE;
  s1 = resolve_cell(t, c, false, SearchHit::Other); assert(s1.fg == 0x0000FF && s1.bg == 0xFF0000);
  c.flags = ITREN_CELL_DIM;                                                  // DIM 向高亮背景混
  o = resolve_cell(t, c, false, SearchHit::Other); assert(o.bg == 0xFFFF00 && o.fg == blend50(0x000000, 0xFFFF00));
  c.flags = ITREN_CELL_HIDDEN;                                               // HIDDEN 不被高亮暴露：橙/黄底空白格
  o = resolve_cell(t, c, false, SearchHit::Current); assert(o.fg == sc.current_bg && o.bg == sc.current_bg && o.underline == sc.current_bg);
  o = resolve_cell(t, c, false, SearchHit::Other); assert(o.fg == sc.other_bg && o.bg == sc.other_bg && o.underline == sc.other_bg);
  c.flags = 0; c.underline_color = 0x0200FF00;                               // 自定义下划线色保留
  o = resolve_cell(t, c, false, SearchHit::Other); assert(o.underline == 0x00FF00);
  Theme t2; t2.search.other_bg = 0x123456;                                   // 可配置
  c.underline_color = 0; assert(resolve_cell(t2, c, false, SearchHit::Other).bg == 0x123456);

  std::puts("search_highlight OK");
  return 0;
}
