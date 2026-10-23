/* 平台无关的字形缓存（LRU + 字节预算 + 分页 shelf 图集）与字体回退规划（DESIGN-T06，14.2）。
   列数只由核心的 width/CONTINUATION 决定，字体回退只换 face，绝不改变列数。 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace itren {

struct GlyphKey {
  uint64_t grapheme_hash = 0;  // 字素簇 UTF-8 的 FNV-1a
  uint16_t face_id = 0;
  bool bold = false, italic = false;
  uint16_t dpi_bucket = 100;   // round(scale*100)
  uint8_t cell_span = 1;       // 1 或 2
  bool operator==(const GlyphKey &o) const {
    return grapheme_hash == o.grapheme_hash && face_id == o.face_id && bold == o.bold && italic == o.italic &&
           dpi_bucket == o.dpi_bucket && cell_span == o.cell_span;
  }
};
inline uint64_t hash_grapheme(const std::string &utf8) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : utf8) { h ^= c; h *= 1099511628211ull; }
  return h;
}
inline uint16_t dpi_bucket(float scale) { return uint16_t(std::lround(scale * 100.0f)); }
struct GlyphKeyHash {
  size_t operator()(const GlyphKey &k) const {
    uint64_t h = k.grapheme_hash;
    h ^= (uint64_t(k.face_id) << 1) ^ (uint64_t(k.bold) << 17) ^ (uint64_t(k.italic) << 18) ^
         (uint64_t(k.dpi_bucket) << 20) ^ (uint64_t(k.cell_span) << 36);
    return size_t(h * 0x9E3779B97F4A7C15ull);
  }
};

struct AtlasSlot { int page = -1, x = 0, y = 0, w = 0, h = 0; bool valid() const { return page >= 0; } };
struct CacheStats { uint64_t hits = 0, misses = 0, evictions = 0, page_evictions = 0; };

// 单页 shelf 打包：按行（shelf）从左到右放，放不下开新 shelf
class ShelfPage {
 public:
  explicit ShelfPage(int size) : size_(size) {}
  bool alloc(int w, int h, int &x, int &y) {
    if (w > size_ || h > size_) return false;
    for (auto &s : shelves_)
      if (h <= s.h && s.used + w <= size_) { x = s.used; y = s.y; s.used += w; return true; }
    if (next_y_ + h > size_) return false;
    shelves_.push_back({next_y_, h, w}); x = 0; y = next_y_; next_y_ += h; return true;
  }
  void reset() { shelves_.clear(); next_y_ = 0; }
 private:
  struct Shelf { int y, h, used; };
  int size_, next_y_ = 0;
  std::vector<Shelf> shelves_;
};

// LRU 字形缓存。预算按图集等价字节（每页 size*size*4）计；超预算时淘汰最久未用条目所在的整页
class GlyphCache {
 public:
  explicit GlyphCache(size_t budget_bytes = 64u << 20, int page_size = 2048)
      : budget_(budget_bytes), page_size_(page_size) {}

  const AtlasSlot *lookup(const GlyphKey &k) {
    auto it = map_.find(k);
    if (it == map_.end()) { ++st_.misses; return nullptr; }
    ++st_.hits;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return &it->second.slot;
  }
  // 放入图集并返回槽位；字形大于一页返回无效槽
  AtlasSlot insert(const GlyphKey &k, int w, int h) {
    auto it = map_.find(k);
    if (it != map_.end()) return it->second.slot;
    if (w > page_size_ || h > page_size_ || w <= 0 || h <= 0) return {};
    AtlasSlot s;
    for (int p = 0; p < int(pages_.size()); ++p)
      if (pages_[p] && pages_[p]->alloc(w, h, s.x, s.y)) { s.page = p; break; }
    if (!s.valid()) {
      while (!pages_.empty() && live_pages() * page_bytes() + page_bytes() > budget_) evict_lru_page();
      int p = -1;
      for (int i = 0; i < int(pages_.size()); ++i) if (!pages_[i]) { p = i; break; }
      if (p < 0) { p = int(pages_.size()); pages_.emplace_back(); }
      pages_[p].reset(new ShelfPage(page_size_));
      pages_[p]->alloc(w, h, s.x, s.y); s.page = p;
    }
    s.w = w; s.h = h;
    lru_.push_front(k);
    map_[k] = {s, lru_.begin()};
    return s;
  }
  void clear() { map_.clear(); lru_.clear(); pages_.clear(); }  // DPI / 字体 / 设备丢失
  // 状态变化时清空：DPI 桶或字体配置变化返回 true
  bool on_config(uint16_t dpi_b, uint64_t font_config_id) {
    if (dpi_b == dpi_ && font_config_id == font_) return false;
    dpi_ = dpi_b; font_ = font_config_id; clear(); return true;
  }
  const CacheStats &stats() const { return st_; }
  size_t size() const { return map_.size(); }
  int live_pages() const { int n = 0; for (auto &p : pages_) n += p ? 1 : 0; return n; }
  size_t page_bytes() const { return size_t(page_size_) * page_size_ * 4; }
  int page_size() const { return page_size_; }

 private:
  struct Entry { AtlasSlot slot; std::list<GlyphKey>::iterator lru; };
  void evict_lru_page() {
    if (lru_.empty()) { pages_.clear(); return; }
    int victim = map_[lru_.back()].slot.page;
    for (auto it = lru_.begin(); it != lru_.end();) {
      auto m = map_.find(*it);
      if (m->second.slot.page == victim) { map_.erase(m); it = lru_.erase(it); ++st_.evictions; } else ++it;
    }
    pages_[victim].reset(); ++st_.page_evictions;
  }
  size_t budget_; int page_size_;
  std::unordered_map<GlyphKey, Entry, GlyphKeyHash> map_;
  std::list<GlyphKey> lru_;
  std::vector<std::unique_ptr<ShelfPage>> pages_;
  CacheStats st_;
  uint16_t dpi_ = 0; uint64_t font_ = UINT64_MAX;
};

// ---- 字体回退规划 ----
enum : uint16_t { FACE_PRIMARY = 0, FACE_CJK = 1, FACE_EMOJI = 2, FACE_LAST_RESORT = 3 };
struct FallbackChain {
  std::vector<std::wstring> names = {L"Cascadia Mono", L"Microsoft YaHei UI", L"Segoe UI Emoji", L"Segoe UI Symbol"};
};
// 一行中的一格：来自核心快照。continuation=宽字符尾格（不单独排字）
struct CellIn { std::vector<uint32_t> cps; uint8_t width = 1; bool continuation = false; bool bold = false, italic = false; };
struct FontRun { uint16_t col0, ncols; uint16_t face; bool bold, italic; };
using HasGlyph = std::function<bool(uint16_t face, uint32_t cp)>;

inline bool is_emoji_cp(uint32_t c) {
  return (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F || (c >= 0x1F1E6 && c <= 0x1F1FF);
}
// 选 face：先按首码点类别排优先（emoji→EMOJI 优先），再沿链找第一个包含所有码点（忽略 VS/ZWJ）的 face
inline uint16_t pick_face(const CellIn &c, uint16_t nfaces, const HasGlyph &has) {
  if (c.cps.empty() || (c.cps.size() == 1 && c.cps[0] == ' ')) return FACE_PRIMARY;
  std::vector<uint16_t> order;
  bool emoji = std::any_of(c.cps.begin(), c.cps.end(), is_emoji_cp);
  if (emoji && nfaces > FACE_EMOJI) order.push_back(FACE_EMOJI);
  for (uint16_t f = 0; f < nfaces; ++f) if (std::find(order.begin(), order.end(), f) == order.end()) order.push_back(f);
  for (uint16_t f : order) {
    bool ok = true;
    for (uint32_t cp : c.cps) if (cp != 0x200D && cp != 0xFE0F && cp != 0xFE0E && !has(f, cp)) { ok = false; break; }
    if (ok) return f;
  }
  return uint16_t(nfaces ? nfaces - 1 : 0);  // 最后兜底（豆腐块）
}
// 产出同 face+样式的连续格段；段覆盖的总列数恒等于输入格数（尾格并入前一宽字符所在段）
inline std::vector<FontRun> plan_runs(const std::vector<CellIn> &row, const HasGlyph &has, uint16_t nfaces = 4) {
  std::vector<FontRun> out;
  for (size_t i = 0; i < row.size(); ++i) {
    const CellIn &c = row[i];
    if (c.continuation && !out.empty()) { out.back().ncols++; continue; }
    uint16_t f = pick_face(c, nfaces, has);
    if (!out.empty() && out.back().face == f && out.back().bold == c.bold && out.back().italic == c.italic &&
        out.back().col0 + out.back().ncols == i)
      out.back().ncols++;
    else
      out.push_back({uint16_t(i), 1, f, c.bold, c.italic});
  }
  return out;
}

// 字形 advance 适配到 cell_span*cell_w：只缩不放；居中偏移
struct GlyphFit { float scale, x_offset; };
inline GlyphFit fit_glyph(float advance, uint8_t cell_span, float cell_w) {
  float box = std::max<uint8_t>(cell_span, 1) * cell_w;
  if (advance <= 0) return {1.0f, 0.0f};
  float s = std::min(1.0f, box / advance);
  return {s, (box - advance * s) / 2.0f};
}

}  // namespace itren
