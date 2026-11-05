// SPDX-License-Identifier: GPL-3.0-or-later
// DS-18 §10：分段、塑形项规划、按格定位、塑形缓存、光标隔离、文本计时统计（纯函数）。
#undef NDEBUG  // assert 在任何构建类型下都必须执行
#include "text_runs.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
using namespace itren;

namespace {
TextCellIn cell(std::u16string t, uint8_t w = 1, uint32_t fg = 7, uint32_t bg = 0, uint8_t fmt = 0) {
  TextCellIn c; c.text = std::move(t); c.width = w; c.fg = fg; c.bg = bg; c.format = fmt; return c;
}
TextCellIn tail() { TextCellIn c; c.width = 0; return c; }
std::vector<TextCellIn> ascii(const std::u16string &s) {
  std::vector<TextCellIn> v;
  for (char16_t c : s) v.push_back(cell(std::u16string(1, c)));
  return v;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
// 构造一个塑形项：每个单元一个簇（clusterMap 由 clusters 给出），advance 统一或逐个指定
ShapedItem item(TextSpan span, std::vector<uint16_t> clusterMap, std::vector<float> adv) {
  ShapedItem it; it.span = span; it.clusterMap = std::move(clusterMap); it.advancesPx = std::move(adv);
  it.glyphIndices.assign(it.advancesPx.size(), 1);
  it.offsetXPx.assign(it.advancesPx.size(), 0); it.offsetYPx.assign(it.advancesPx.size(), 0);
  return it;
}
ShapedItem identity(const TextRun &run, float advPx) {  // 一单元一字形
  std::vector<uint16_t> cm(run.text.size());
  for (size_t i = 0; i < cm.size(); ++i) cm[i] = uint16_t(i);
  return item({0, uint32_t(run.text.size())}, cm, std::vector<float>(run.text.size(), advPx));
}
std::vector<uint8_t> widthsOf(const std::vector<TextCellIn> &cells) {
  std::vector<uint8_t> w; for (auto &c : cells) w.push_back(c.width); return w;
}
// 每个核心格恰好出现在一个片段里
void assertEachCellOnce(const std::vector<DrawPiece> &pieces, const TextRun &run, const std::vector<ShapedItem> &items) {
  std::vector<int> seen(run.cellSpans.size(), 0);
  std::vector<uint16_t> cellOfUnit(run.text.size());
  for (size_t k = 0; k < run.cellSpans.size(); ++k)
    for (uint32_t u = run.cellSpans[k].begin; u < run.cellSpans[k].end(); ++u) cellOfUnit[u] = uint16_t(k);
  for (const auto &p : pieces) {
    if (p.kind == DrawPiece::LegacyCell) { ++seen[p.cell]; continue; }
    const auto &it = items[p.item];
    std::set<uint16_t> cells;
    for (uint32_t u = 0; u < it.span.length; ++u)
      if (it.clusterMap[u] >= p.glyphBegin && it.clusterMap[u] < p.glyphEnd) cells.insert(cellOfUnit[it.span.begin + u]);
    for (uint16_t k : cells) ++seen[k];
  }
  for (int n : seen) assert(n == 1);
}
}  // namespace

static void segmentation() {
  // 样式、前景、背景、不可见格断段；全空格段丢弃
  auto row = ascii(u"ab  cd");
  row[1].fg = 9;                       // b 换色
  row[4].bg = 3;                       // c 换背景
  row[5].visible = false;              // d 隐藏
  auto runs = segmentRow(row, std::nullopt);
  assert(runs.size() == 3);                     // b 后的两个空格颜色不同，单独成段后被丢弃
  assert(runs[0].text == u"a" && runs[0].cols == (ColRange{0, 1}));
  assert(runs[1].text == u"b" && runs[1].cols == (ColRange{1, 1}) && runs[1].fg == 9);
  assert(runs[2].text == u"c" && runs[2].bg == 3 && runs[2].cols == (ColRange{4, 1}));
  // 同色的空格留在段内（决定后续字符的位置）
  auto spaced = ascii(u"a  b");
  assert(segmentRow(spaced, std::nullopt)[0].text == u"a  b");
  assert(segmentRow(ascii(u"    "), std::nullopt).empty());
  // 尾格并入宽字符；colOfUnit / cellSpans / cellCols 一一对应（含代理对、组合字符）
  std::vector<TextCellIn> wide{cell(u"中", 2), tail(), cell(u"\U0001F600", 2), tail(), cell(u"é"), cell(u"x")};
  runs = segmentRow(wide, std::nullopt);
  assert(runs.size() == 1);
  const auto &r = runs[0];
  assert(r.cols == (ColRange{0, 6}));
  assert(r.cellCols == (std::vector<uint16_t>{0, 2, 4, 5}));
  assert(r.cellSpans[1] == (TextSpan{1, 2}) && r.cellSpans[2] == (TextSpan{3, 2}));
  assert(r.colOfUnit == (std::vector<uint16_t>{0, 2, 2, 4, 4, 5}));
  // 空格子按空格处理
  std::vector<TextCellIn> holes{cell(u"a"), cell(u""), cell(u"b")};
  assert(segmentRow(holes, std::nullopt)[0].text == u"a b");
  // isolate：光标格单独成段，不影响前后；落在宽字符上时覆盖两列
  runs = segmentRow(ascii(u"a->b"), ColRange{2, 1});
  assert(runs.size() == 3 && runs[0].text == u"a-" && runs[1].text == u">" && runs[2].text == u"b");
  runs = segmentRow(wide, ColRange{0, 2});
  assert(runs.size() == 2 && runs[0].cols == (ColRange{0, 2}) && runs[0].text == u"中");
  // 隐藏宽字符的尾格不并入别的段
  wide[0].visible = false;
  runs = segmentRow(wide, std::nullopt);
  assert(runs.size() == 1 && runs[0].cols.col0 == 2);
}

static void directionAndItems() {
  // RTL 格：奇数层级；阿拉伯文中的数字（层级 2）不是
  std::vector<TextSpan> cells{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
  assert((rtlCells({0, 1, 2, 0}, cells) == std::vector<bool>{false, true, false, false}));
  // RLE 之后的拉丁格按层级判定
  assert((rtlCells({0, 1, 1, 0}, cells) == std::vector<bool>{false, true, true, false}));

  // 零宽格式字符：ZWJ、变体选择符、方向控制符；普通字母与组合符不是
  for (uint32_t cp : {0x200Du, 0xFE0Fu, 0x200Fu, 0x202Bu, 0x2067u, 0xE0100u, 0x061Cu}) assert(isZeroWidthFormat(cp));
  for (uint32_t cp : {0x41u, 0x301u, 0x4E2Du, 0x1F600u, 0x2069u + 1}) assert(!isZeroWidthFormat(cp));
  // intersectRanges：交错、包含、边界重合、空输入
  std::vector<ScriptRange> sc{{{0, 3}, 1, 0}, {{3, 3}, 2, 0}};
  std::vector<FontRange> fo{{{0, 2}, 0}, {{2, 4}, 1}};
  auto x = intersectRanges(sc, fo);
  assert(x.size() == 3);
  assert(x[0] == (ItemRange{{0, 2}, 0, 0}) && x[1] == (ItemRange{{2, 1}, 0, 1}) && x[2] == (ItemRange{{3, 3}, 1, 1}));
  assert(intersectRanges({}, fo).empty());

  auto hasAllNone = [](uint16_t, uint16_t) { return false; };
  auto boundariesOnCells = [](const ItemPlan &p, const std::vector<TextSpan> &cs) {
    std::set<uint32_t> edges{0};
    for (auto &c : cs) edges.insert(c.end());
    for (auto &it : p.items) assert(edges.count(it.span.begin) && edges.count(it.span.end()));
  };
  // 格内跨两个字体：改用第一个含全部码点的字体
  std::vector<TextSpan> c2{{0, 1}, {1, 2}, {2 + 1, 1}};  // a | e+◌́ | b
  std::vector<ScriptRange> latin{{{0, 4}, 1, 0}};
  std::vector<FontRange> split{{{0, 2}, 0}, {{2, 1}, 1}, {{3, 1}, 0}};  // 组合符落在字体 1
  auto p = planItems(c2, latin, split, {}, [](uint16_t f, uint16_t k) { return k == 1 && f == 1; });
  assert(p.legacyCells.empty());
  assert(p.items.size() == 3 && p.items[1].span == (TextSpan{1, 2}) && p.items[1].font == 1);
  boundariesOnCells(p, c2);
  // 都不含：降级，降级格从项中挖掉，项一分为二
  p = planItems(c2, latin, split, {}, hasAllNone);
  assert(p.legacyCells == (std::vector<uint16_t>{1}));
  assert(p.items.size() == 2 && p.items[0].span == (TextSpan{0, 1}) && p.items[1].span == (TextSpan{3, 1}));
  // 同一字体、格内跨两个 script：降级，不拆成两个项
  std::vector<ScriptRange> mixed{{{0, 2}, 1, 0}, {{2, 2}, 5, 0}};  // 组合符被分到另一个 script
  std::vector<FontRange> one{{{0, 4}, 0}};
  p = planItems(c2, mixed, one, {}, hasAllNone);
  assert(p.legacyCells == (std::vector<uint16_t>{1}));
  for (auto &it : p.items) assert(!(it.span.begin < 3 && it.span.end() > 1));  // 没有项碰到格 1
  boundariesOnCells(p, c2);
  // RTL 格自成一项
  std::vector<TextSpan> c3{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
  std::vector<ScriptRange> s3{{{0, 4}, 1, 0}};
  p = planItems(c3, s3, one, {false, true, true, false}, hasAllNone);
  assert(p.items.size() == 4);
  for (size_t k = 0; k < 4; ++k) assert(p.items[k].span == c3[k] || k == 0 || k == 3);
  assert(p.items[1].span == (TextSpan{1, 1}) && p.items[2].span == (TextSpan{2, 1}));
  // 不变量：随机组合下所有项边界都在格边界上
  for (uint32_t seed = 1; seed < 200; ++seed) {
    std::vector<TextSpan> cs; uint32_t pos = 0, r = seed;
    auto rnd = [&](uint32_t m) { r = r * 1103515245u + 12345u; return (r >> 16) % m; };
    for (int k = 0; k < 6; ++k) { uint32_t len = 1 + rnd(3); cs.push_back({pos, len}); pos += len; }
    std::vector<ScriptRange> ss; std::vector<FontRange> fs;
    for (uint32_t b = 0; b < pos;) { uint32_t len = 1 + rnd(4); len = std::min(len, pos - b); ss.push_back({{b, len}, uint16_t(rnd(3)), 0}); b += len; }
    for (uint32_t b = 0; b < pos;) { uint32_t len = 1 + rnd(4); len = std::min(len, pos - b); fs.push_back({{b, len}, uint16_t(rnd(2))}); b += len; }
    std::vector<bool> rtl; for (int k = 0; k < 6; ++k) rtl.push_back(rnd(4) == 0);
    auto plan = planItems(cs, ss, fs, rtl, [&](uint16_t f, uint16_t) { return f == 0; });
    boundariesOnCells(plan, cs);
    for (uint16_t k : plan.legacyCells)
      for (auto &it : plan.items) assert(it.span.end() <= cs[k].begin || it.span.begin >= cs[k].end());
  }
}

static void placement() {
  const float w = 10.f;
  // ASCII 一字一形：各落在自己的列（advance 与格宽相同时不偏移）
  auto row = ascii(u"abc");
  auto run = segmentRow(row, std::nullopt)[0];
  std::vector<ShapedItem> items{identity(run, w)};
  std::vector<uint16_t> noLegacy;
  auto pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(pieces.size() == 1 && pieces[0].kind == DrawPiece::Glyphs);
  assert(near(pieces[0].originXPx, 0) && pieces[0].advancesPx == (std::vector<float>{10, 10, 10}));
  assert(pieces[0].clip == run.cols);
  // 比例回退字形（advance 8 < 格宽 10）：居中，不挪后续字形
  items = {identity(run, 8.f)};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(near(pieces[0].originXPx, 1.f) && near(pieces[0].advancesPx[0], 10.f) && near(pieces[0].advancesPx[1], 10.f));
  // 宽字符占两格，下一个字落在 col+2
  std::vector<TextCellIn> wide{cell(u"中", 2), tail(), cell(u"a")};
  run = segmentRow(wide, std::nullopt)[0];
  items = {item({0, 2}, {0, 1}, {20.f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(wide), w);
  assert(near(pieces[0].originXPx, 0) && near(pieces[0].advancesPx[0], 20.f));
  // 组合字符同簇：e + U+0301 两个字形（第二个零 advance），下一格不受影响
  std::vector<TextCellIn> comb{cell(u"é"), cell(u"x")};
  run = segmentRow(comb, std::nullopt)[0];
  items = {item({0, 3}, {0, 0, 2}, {10.f, 0.f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(comb), w);
  assert(pieces[0].advancesPx == (std::vector<float>{10, 0, 10}));
  // 同一核心格在同一项内的两个簇：组合符落在基字符之后的自然位置
  items = {item({0, 3}, {0, 1, 2}, {10.f, 0.f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(comb), w);
  assert(pieces.size() == 1 && pieces[0].advancesPx == (std::vector<float>{10, 0, 10}));
  // 连字：3 字符 → 1 字形横跨 3 列，后续字形落在 col 3
  row = ascii(u"===a");
  run = segmentRow(row, std::nullopt)[0];
  items = {item({0, 4}, {0, 0, 0, 1}, {30.f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(pieces.size() == 1 && pieces[0].advancesPx == (std::vector<float>{30, 10}));
  // calt：一字一形的替代字形，与 ASCII 相同
  items = {identity(run, w)};
  assert(placeOnGrid({items, noLegacy}, run, widthsOf(row), w)[0].advancesPx.size() == 4);
  // 超宽单元：独立片段并缩放；前后普通片段原点正确；被拆出的字形不在普通片段里
  row = ascii(u"aWb");
  run = segmentRow(row, std::nullopt)[0];
  items = {item({0, 3}, {0, 1, 2}, {10.f, 15.f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(pieces.size() == 3);
  assert(pieces[0].glyphBegin == 0 && pieces[0].glyphEnd == 1 && near(pieces[0].originXPx, 0));
  assert(pieces[1].glyphBegin == 1 && pieces[1].glyphEnd == 2 && near(pieces[1].scale, 10.f / 15.f) && near(pieces[1].originXPx, 10.f));
  assert(pieces[1].clip == (ColRange{1, 1}));
  assert(pieces[2].glyphBegin == 2 && near(pieces[2].originXPx, 20.f));
  assertEachCellOnce(pieces, run, items);
  // 超出不足 1 像素不缩（居中，偏移为负的半个溢出量）
  items = {item({0, 3}, {0, 1, 2}, {10.f, 10.9f, 10.f})};
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(pieces.size() == 1 && near(pieces[0].advancesPx[0], 10.f - 0.f + (-0.45f)) );
  // 降级格：列、宽度、文字来自本次输入；跨列复用；前面有宽字尾格；每格只画一次
  std::vector<TextCellIn> lr{cell(u"中", 2), tail(), cell(u"á"), cell(u"b")};
  run = segmentRow(lr, std::nullopt)[0];
  ShapedRun sr;
  sr.items = {item({0, 1}, {0}, {20.f}), item({3, 1}, {0}, {10.f})};
  sr.legacyCells = {1};
  pieces = placeOnGrid({sr.items, sr.legacyCells}, run, widthsOf(lr), w);
  assert(pieces.size() == 3 && pieces[1].kind == DrawPiece::LegacyCell);
  assert(pieces[1].cell == 1 && pieces[1].clip == (ColRange{2, 1}));
  assert(run.text.substr(run.cellSpans[1].begin, run.cellSpans[1].length) == u"á");
  assert(near(pieces[2].originXPx, 30.f));
  assertEachCellOnce(pieces, run, sr.items);
  // 同一缓存条目挪到别的列：前面多三格空白，降级格与字形都随之移动
  std::vector<TextCellIn> shifted{cell(u"x", 1, 1), cell(u"y", 1, 1), cell(u"z", 1, 1)};
  shifted.insert(shifted.end(), lr.begin(), lr.end());
  run = segmentRow(shifted, std::nullopt)[1];
  pieces = placeOnGrid({sr.items, sr.legacyCells}, run, widthsOf(shifted), w);
  assert(near(pieces[0].originXPx, 30.f) && pieces[1].clip == (ColRange{5, 1}) && near(pieces[2].originXPx, 60.f));
  // 同文本与分组、但格宽布局变化（核心把降级格改成宽字符）
  std::vector<TextCellIn> lr2{cell(u"中", 2), tail(), cell(u"á", 2), tail(), cell(u"b")};
  run = segmentRow(lr2, std::nullopt)[0];
  assert(shapeKeyFor(run, false) == shapeKeyFor(segmentRow(lr, std::nullopt)[0], false));
  pieces = placeOnGrid({sr.items, sr.legacyCells}, run, widthsOf(lr2), w);
  assert(pieces[1].clip == (ColRange{2, 2}) && near(pieces[2].originXPx, 40.f));
  assertEachCellOnce(pieces, run, sr.items);
  // 塑形结果不可用（clusterMap 越界）：该项的格按旧方式，不越界访问
  items = {item({0, 3}, {0, 5, 1}, {10.f, 10.f})};
  row = ascii(u"abc"); run = segmentRow(row, std::nullopt)[0];
  pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), w);
  assert(pieces.size() == 3 && pieces[0].kind == DrawPiece::LegacyCell);
  // 四档 DPI：格宽 10 / 12.5→13 / 15 / 20，非整数 advance，1 像素容差始终是 1 像素
  for (float cw : {10.f, 13.f, 15.f, 20.f}) {
    items = {item({0, 3}, {0, 1, 2}, {cw + 0.6f, cw + 1.2f, cw})};
    pieces = placeOnGrid({items, noLegacy}, run, widthsOf(row), cw);
    assert(pieces.size() == 3 && pieces[1].scale < 1.f && near(pieces[2].originXPx, 2 * cw));
    assert(pieces[0].scale == 1.f && near(pieces[0].originXPx, -0.3f));
  }
}

static void isolation() {
  IsolateState none, a{true, 3, {5, 1}}, b{true, 3, {6, 1}}, c{true, 7, {0, 2}};
  assert(isolateDirtyRows(a, a).empty());
  assert(isolateDirtyRows(a, b) == (std::vector<uint16_t>{3}));          // 同一行内移动
  assert(isolateDirtyRows(a, c) == (std::vector<uint16_t>{3, 7}));       // 跨行
  assert(isolateDirtyRows(none, a) == (std::vector<uint16_t>{3}));       // 出现
  assert(isolateDirtyRows(a, none) == (std::vector<uint16_t>{3}));       // 消失（隐藏或换成竖线光标）
  // 闪烁、焦点不改变隔离状态 → 不标脏（状态里本就没有这两项）
}

static void cache() {
  int released = 0;
  auto make = [&](float adv) {
    auto r = std::make_shared<ShapedRun>();
    ShapedItem it = item({0, 1}, {0}, {adv});
    it.face = std::shared_ptr<void>(new int(1), [&](void *p) { delete static_cast<int *>(p); ++released; });
    r->items.push_back(std::move(it));
    return std::shared_ptr<const ShapedRun>(std::move(r));
  };
  auto key = [](std::u16string t, std::vector<uint16_t> lens = {1}) { ShapeKey k; k.text = std::move(t); k.cellLengths = std::move(lens); return k; };
  const size_t one = shapedRunBytes(key(u"a"), *make(1));
  released = 0;
  ShapeCache c(one * 4 + 8);                               // 恰好放下 4 项（单项不超过预算的 1/4）
  assert(!c.find(key(u"a")) && c.stats().misses == 1);
  c.put(key(u"a"), make(1)); c.put(key(u"b"), make(2)); c.put(key(u"c"), make(3)); c.put(key(u"d"), make(4));
  assert(c.size() == 4 && c.stats().bypassed == 0);
  assert(c.find(key(u"a")) && c.stats().hits == 1);
  auto held = c.find(key(u"b"));                           // 持有 b
  c.find(key(u"a"));                                       // LRU：a, b, d, c
  c.put(key(u"e"), make(5));                               // 淘汰最久未用的 c
  assert(!c.find(key(u"c")) && c.stats().evictions == 1 && released == 1);
  c.put(key(u"f"), make(6));                               // LRU 为 e, a, b, d → 淘汰 d
  assert(released == 2);
  c.put(key(u"g"), make(7));                               // 淘汰 b，但 held 仍持有
  assert(!c.find(key(u"b")) && c.stats().evictions == 3 && released == 2);
  assert(near(held->items[0].advancesPx[0], 2.f));
  held.reset();
  assert(released == 3);                                   // 最后一个引用释放时删除器才调用
  // 相同文本、不同格分组不命中；格式、连字开关不同也不命中
  assert(!c.find(key(u"a", {2})));
  ShapeKey k2 = key(u"a"); k2.ligatures = true; assert(!c.find(k2));
  k2 = key(u"a"); k2.format = 1; assert(!c.find(k2));
  // 单项过大绕过
  ShapeCache small(64);
  small.put(key(u"big"), make(1));
  assert(small.size() == 0 && small.stats().bypassed == 1 && released == 4);  // 未进缓存，临时引用随即释放
  c.clear();                                               // 剩下 a、e、f、g
  assert(c.size() == 0 && c.bytes() == 0 && released == 8);
}

static void plans() {
  auto row = ascii(u"ab");
  auto run = segmentRow(row, std::nullopt)[0];
  ShapeCache cache;
  PlanCounters counters;
  int calls = 0;
  auto failing = [&](const TextRun &) -> std::shared_ptr<const ShapedRun> { ++calls; return nullptr; };
  auto ok = [&](const TextRun &r) -> std::shared_ptr<const ShapedRun> {
    ++calls;
    auto s = std::make_shared<ShapedRun>();
    s->items.push_back(identity(r, 10.f));
    return s;
  };
  // 注入一次塑形失败：底层标为整段旧方式，错误只计一次；覆盖层用同一计划，不重试
  RunPlan p = planRun(run, widthsOf(row), 10.f, false, cache, failing, counters);
  assert(p.legacyWhole() && counters.shapeFailures == 1 && calls == 1);
  IsolatedPlan iso{{true, 0, {0, 2}}, 42, p, run};
  assert(iso.usableFor({true, 0, {0, 2}}, 42) && iso.plan->legacyWhole());
  assert(counters.shapeFailures == 1 && calls == 1);
  // 状态或行内容变了不能用；作废后不能用
  assert(!iso.usableFor({true, 0, {1, 1}}, 42) && !iso.usableFor({true, 0, {0, 2}}, 43));
  iso.invalidate();
  assert(!iso.usableFor({true, 0, {0, 2}}, 42));
  // 成功塑形进缓存；第二次命中，不再调用塑形
  p = planRun(run, widthsOf(row), 10.f, false, cache, ok, counters);
  assert(!p.legacyWhole() && calls == 2 && p.pieces.size() == 1);
  p = planRun(run, widthsOf(row), 10.f, false, cache, ok, counters);
  assert(calls == 2 && cache.stats().hits == 1);
}

static void timing() {
  uint64_t clock = 0;
  TextTimer t([&] { return clock; });
  // 背景 5、文本 3、装饰 2、文本 4、背景 7：只有文本区间进样本
  clock += 5; t.begin(); clock += 3; t.end(); clock += 2; t.begin(); clock += 4; t.end(); clock += 7;
  assert(t.takeFrameNs() == 7 && t.takeFrameNs() == 0);
  // 分类按重画行数：空白行的全屏重画仍是 Full
  assert(classifyFrame(0, 50) == FrameClass::NoText && classifyFrame(1, 50) == FrameClass::Partial);
  assert(classifyFrame(50, 50) == FrameClass::Full);
  // 分位数（最近秩）
  std::vector<uint64_t> v; for (uint64_t i = 1; i <= 100; ++i) v.push_back(i);
  auto p = percentiles(v);
  assert(p.count == 100 && p.p50 == 50 && p.p95 == 95 && p.max == 100);
  assert(percentiles({}).count == 0 && percentiles({7}).p95 == 7);
  // 窗口：1000 个样本先到；冷标记只在第一个窗口
  RenderStatsWindow w;
  w.markCold();
  ShapeCacheStats cs; PlanCounters pc;
  std::optional<RenderStatsSummary> s;
  for (int i = 0; i < 1000 && !s; ++i) {
    cs.hits += 3; cs.misses += 1;
    s = w.add(uint64_t(i) * 1000, i % 2 ? 50 : 0, 50, 2000, cs, pc);
  }
  assert(s && s->cold && s->noTextFrames == 500 && s->full.count == 500 && s->full.p95 == 2000);
  assert(s->cache.hits == 2997 && s->cache.misses == 999);   // 第一个样本作基线
  s = std::nullopt;
  s = w.add(0, 10, 50, 100, cs, pc);
  assert(!s);
  s = w.add(RenderStatsWindow::kWindowNs, 10, 50, 300, cs, pc);  // 10 秒到
  assert(s && !s->cold && s->partial.count == 2 && s->partial.p95 == 300);
  RenderStatsContext ctx{"glyphrun", true, 1.5f, 120, 40};
  const std::string line = formatRenderStats(*s, ctx);
  assert(line.find("path=glyphrun lig=1 dpi=1.50 grid=120x40 cold=0") == 0);
  assert(line.find("partial_us=2/0/0/0") != std::string::npos);
}

static void fitGlyph() {  // 原 test_glyph_cache 的 fit_glyph 用例
  auto f = fit_glyph(8, 1, 10); assert(f.scale == 1.0f && f.x_offset == 1.0f);
  f = fit_glyph(25, 2, 10); assert(std::fabs(f.scale - 0.8f) < 1e-6 && std::fabs(f.x_offset) < 1e-5);
  f = fit_glyph(20, 2, 10); assert(f.scale == 1.0f && f.x_offset == 0.0f);
  f = fit_glyph(0, 1, 10); assert(f.scale == 1.0f);
  // 溢出 < 1px 不缩（144 DPI：格 13 vs advance 13.195），居中
  f = fit_glyph(13.195f, 1, 13); assert(f.scale == 1.0f && std::fabs(f.x_offset + 0.0975f) < 1e-4);
  f = fit_glyph(13.99f, 1, 13); assert(f.scale == 1.0f);
  f = fit_glyph(14.0f, 1, 13); assert(f.scale < 1.0f);
}

int main() {
  segmentation();
  directionAndItems();
  placement();
  isolation();
  cache();
  plans();
  timing();
  fitGlyph();
  std::puts("text_runs ok");
}
