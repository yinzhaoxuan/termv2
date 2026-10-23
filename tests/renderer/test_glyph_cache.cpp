#undef NDEBUG  // assert 在任何构建类型下都必须执行
#include "glyph_cache.h"
#include <cassert>
#include <cstdio>
using namespace itren;
static GlyphKey K(uint64_t h, uint16_t dpi = 100) { GlyphKey k; k.grapheme_hash = h; k.dpi_bucket = dpi; return k; }
static bool overlap(const AtlasSlot &a, const AtlasSlot &b) {
  return a.page == b.page && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
int main() {
  // 命中/未命中
  GlyphCache c;
  assert(!c.lookup(K(1)) && c.stats().misses == 1);
  auto s1 = c.insert(K(1), 10, 20); assert(s1.valid() && s1.page == 0);
  auto *p = c.lookup(K(1)); assert(p && p->x == s1.x && c.stats().hits == 1);
  GlyphKey kb = K(1); kb.bold = true; assert(!c.lookup(kb));
  GlyphKey kw = K(1); kw.cell_span = 2; assert(!c.lookup(kw));
  assert(hash_grapheme("中") != hash_grapheme("文") && dpi_bucket(1.25f) == 125);
  // 打包不重叠（小页多 shelf 多页）
  GlyphCache small(1u << 30, 64);
  std::vector<AtlasSlot> v;
  for (int i = 0; i < 200; ++i) v.push_back(small.insert(K(100 + i), 7 + i % 9, 10 + i % 7));
  for (auto &a : v) { assert(a.valid() && a.x >= 0 && a.y >= 0 && a.x + a.w <= 64 && a.y + a.h <= 64); }
  for (size_t i = 0; i < v.size(); ++i) for (size_t j = i + 1; j < v.size(); ++j) assert(!overlap(v[i], v[j]));
  assert(small.live_pages() > 1);
  assert(!small.insert(K(9999), 65, 10).valid());
  // 淘汰：预算 2 页
  GlyphCache e(2 * 64 * 64 * 4, 64);
  for (int i = 0; i < 16; ++i) e.insert(K(i), 16, 16);       // 第 0 页填满
  for (int i = 16; i < 32; ++i) e.insert(K(i), 16, 16);      // 第 1 页填满
  assert(e.live_pages() == 2 && e.stats().evictions == 0);
  e.lookup(K(0));                                            // 第 0 页的 K(0) 变新，最旧是 K(1)（第 0 页）
  e.insert(K(32), 16, 16);
  assert(e.live_pages() == 2 && e.stats().page_evictions == 1 && e.stats().evictions == 16);
  assert(!e.lookup(K(0)) && e.lookup(K(20)) && e.lookup(K(32)));
  assert(e.live_pages() * e.page_bytes() <= 2 * 64 * 64 * 4);
  // DPI / 字体变化清空
  assert(c.on_config(100, 1) && c.size() == 0);
  c.insert(K(5), 8, 8); assert(!c.on_config(100, 1) && c.size() == 1);
  assert(c.on_config(150, 1) && c.size() == 0 && !c.lookup(K(5, 150)));
  c.insert(K(5), 8, 8); assert(c.on_config(150, 2) && c.size() == 0);

  // 回退：主字体只有 ASCII，CJK 有汉字，emoji 有 emoji
  HasGlyph has = [](uint16_t f, uint32_t cp) {
    if (f == FACE_PRIMARY) return cp < 0x80;
    if (f == FACE_CJK) return cp < 0x80 || (cp >= 0x4E00 && cp <= 0x9FFF);
    if (f == FACE_EMOJI) return is_emoji_cp(cp) || cp == 0x1F44D || cp == 0x1F3FD;
    return true;
  };
  std::vector<CellIn> row = {{{'a'}}, {{'b'}}, {{0x4E2D}, 2}, {{}, 1, true}, {{0x6587}, 2}, {{}, 1, true},
                             {{0x1F44D, 0x1F3FD}, 2}, {{}, 1, true}, {{'c'}, 1, false, true}, {{0xE000}}};
  auto runs = plan_runs(row, has);
  assert(runs.size() == 5);
  assert(runs[0].face == FACE_PRIMARY && runs[0].col0 == 0 && runs[0].ncols == 2);
  assert(runs[1].face == FACE_CJK && runs[1].col0 == 2 && runs[1].ncols == 4);   // 宽 CJK 各占 2 列
  assert(runs[2].face == FACE_EMOJI && runs[2].col0 == 6 && runs[2].ncols == 2);
  assert(runs[3].face == FACE_PRIMARY && runs[3].bold && runs[3].ncols == 1);
  assert(runs[4].face == FACE_LAST_RESORT && runs[4].col0 == 9);
  // 列数不变：段首尾相接且总和等于格数
  int sum = 0; uint16_t next = 0;
  for (auto &r : runs) { assert(r.col0 == next); next = uint16_t(r.col0 + r.ncols); sum += r.ncols; }
  assert(sum == int(row.size()));
  // 无任何 face 有字形也不改列数
  auto none = plan_runs(row, [](uint16_t, uint32_t) { return false; });
  sum = 0; for (auto &r : none) sum += r.ncols; assert(sum == int(row.size()));
  // fit_glyph
  auto f = fit_glyph(8, 1, 10); assert(f.scale == 1.0f && f.x_offset == 1.0f);
  f = fit_glyph(25, 2, 10); assert(std::fabs(f.scale - 0.8f) < 1e-6 && std::fabs(f.x_offset) < 1e-5);
  f = fit_glyph(20, 2, 10); assert(f.scale == 1.0f && f.x_offset == 0.0f);
  f = fit_glyph(0, 1, 10); assert(f.scale == 1.0f);
  // 评审：溢出 < 1px 不缩（144 DPI：格 13 vs advance 13.195），居中
  f = fit_glyph(13.195f, 1, 13); assert(f.scale == 1.0f && std::fabs(f.x_offset + 0.0975f) < 1e-4);
  f = fit_glyph(13.99f, 1, 13); assert(f.scale == 1.0f);
  f = fit_glyph(14.0f, 1, 13); assert(f.scale < 1.0f);   // 溢出达 1px 才缩
  std::puts("glyph_cache ok");
}
