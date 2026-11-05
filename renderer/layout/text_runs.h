// SPDX-License-Identifier: GPL-3.0-or-later
/* DS-18 字形运行渲染的平台无关部分：分段、塑形项规划、按格定位、塑形缓存、光标隔离、文本计时统计。
   不依赖 DirectWrite：平台层把分析与塑形结果转成这里的结构，再按返回的绘制片段调用 DrawGlyphRun。
   单位一律是物理像素（DS-18 §3），带 Px 后缀。 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace itren {

// ---- 宽于格的字形：只缩不放、居中（原 glyph_cache.h）----
// 溢出不足 1px（格宽按 round(advance) 取整造成，如 144 DPI 下格 13 vs advance 13.195）不缩，
// 以免主字体整体被缩小发虚；此时偏移为负的半个溢出量，仍居中。
inline constexpr float kGlyphFitTolerancePx = 1.0f;
struct GlyphFit { float scale, x_offset; };
inline GlyphFit fit_box(float advancePx, float boxPx) {
  if (advancePx <= 0) return {1.0f, 0.0f};
  if (advancePx - boxPx < kGlyphFitTolerancePx) return {1.0f, (boxPx - advancePx) / 2.0f};
  const float s = std::min(1.0f, boxPx / advancePx);
  return {s, (boxPx - advancePx * s) / 2.0f};
}
inline GlyphFit fit_glyph(float advance, uint8_t cell_span, float cell_w) {
  return fit_box(advance, std::max<uint8_t>(cell_span, 1) * cell_w);
}

// ---- §4.1 分段 ----
struct ColRange {
  uint16_t col0 = 0, ncols = 0;
  bool contains(uint16_t c) const { return c >= col0 && c < col0 + ncols; }
  bool operator==(const ColRange &) const = default;
};
struct TextSpan {
  uint32_t begin = 0, length = 0;
  uint32_t end() const { return begin + length; }
  bool operator==(const TextSpan &) const = default;
};

struct TextCellIn {           // 每列一项（含尾格）
  std::u16string text;        // 字素的 UTF-16；空格子为 u" " 或空串；尾格为空串
  uint8_t width = 1;          // 1 或 2；尾格为 0
  uint8_t format = 0;         // 0..3：bold | italic << 1
  uint32_t fg = 0, bg = 0;    // 解析后的前景、背景
  bool visible = true;        // 闪烁隐藏相位、SGR 8 隐藏时为 false
};

struct TextRun {
  ColRange cols;
  uint8_t format = 0;
  uint32_t fg = 0, bg = 0;
  std::u16string text;
  std::vector<uint16_t> colOfUnit;   // text 每个 UTF-16 单元所在的列（绝对列）
  // 段内核心格（不含尾格）按顺序编号 0..n-1，称为「格序号」。下面两个数组按格序号索引。
  std::vector<TextSpan> cellSpans;   // 每个核心格在 text 中的区间
  std::vector<uint16_t> cellCols;    // 每个核心格的首列（绝对列）
};

// 同 format / fg / bg 的连续可见格合成一段；isolate 范围（块光标，§7）单独成段；全空格段丢弃。
inline std::vector<TextRun> segmentRow(const std::vector<TextCellIn> &cells, std::optional<ColRange> isolate) {
  std::vector<TextRun> out;
  TextRun cur;
  bool open = false, curIsolated = false;
  auto close = [&] {
    if (!open) return;
    const bool blank = std::all_of(cur.text.begin(), cur.text.end(), [](char16_t c) { return c == u' '; });
    if (!blank) out.push_back(std::move(cur));
    cur = TextRun{};
    open = false;
  };
  bool lastHeadTaken = false;  // 上一个核心格是否进了段（尾格据此决定是否并入）
  for (size_t i = 0; i < cells.size(); ++i) {
    const auto &c = cells[i];
    const uint16_t col = uint16_t(i);
    if (c.width == 0) {  // 尾格：并入宿主所在的段
      if (lastHeadTaken && open && cur.cols.col0 + cur.cols.ncols == col) ++cur.cols.ncols;
      continue;
    }
    lastHeadTaken = false;
    const bool iso = isolate && isolate->contains(col);
    if (!c.visible) { close(); continue; }
    if (open && (iso || curIsolated || cur.format != c.format || cur.fg != c.fg || cur.bg != c.bg)) close();
    if (!open) {
      cur.cols = {col, 0}; cur.format = c.format; cur.fg = c.fg; cur.bg = c.bg;
      open = true; curIsolated = iso;
    }
    const std::u16string text = c.text.empty() ? std::u16string(u" ") : c.text;
    cur.cellSpans.push_back({uint32_t(cur.text.size()), uint32_t(text.size())});
    cur.cellCols.push_back(col);
    cur.text += text;
    cur.colOfUnit.insert(cur.colOfUnit.end(), text.size(), col);
    ++cur.cols.ncols;
    lastHeadTaken = true;
  }
  close();
  return out;
}

// ---- §4.2 方向：AnalyzeBidi 的解析层级 → RTL 格 ----
// levels：每个 UTF-16 单元的解析层级（不重排）。格里任何一个单元层级为奇数，该格为 RTL 格。
inline std::vector<bool> rtlCells(const std::vector<uint8_t> &levels, const std::vector<TextSpan> &cells) {
  std::vector<bool> out(cells.size(), false);
  for (size_t k = 0; k < cells.size(); ++k)
    for (uint32_t u = cells[k].begin; u < cells[k].end() && u < levels.size(); ++u)
      if (levels[u] & 1) { out[k] = true; break; }
  return out;
}

// ---- §4.2 / §5.0 塑形项规划 ----
struct ScriptRange { TextSpan span; uint16_t script = 0; uint8_t shapes = 0; };  // 对应 DWRITE_SCRIPT_ANALYSIS
struct FontRange { TextSpan span; uint16_t font = 0; };                          // font：平台层的字体表下标
struct ItemRange {
  TextSpan span;
  uint16_t scriptRange = 0;  // scripts 的下标（保留 script 与 shapes）
  uint16_t font = 0;
  bool operator==(const ItemRange &) const = default;
};
struct ItemPlan {
  std::vector<ItemRange> items;
  std::vector<uint16_t> legacyCells;  // 格序号，升序
};

// 两组覆盖整段文本的区间求交（任一方缺失的单元不产生交集）。
inline std::vector<ItemRange> intersectRanges(const std::vector<ScriptRange> &scripts, const std::vector<FontRange> &fonts) {
  std::vector<ItemRange> out;
  size_t i = 0, j = 0;
  while (i < scripts.size() && j < fonts.size()) {
    const uint32_t b = std::max(scripts[i].span.begin, fonts[j].span.begin);
    const uint32_t e = std::min(scripts[i].span.end(), fonts[j].span.end());
    if (b < e) out.push_back({{b, e - b}, uint16_t(i), fonts[j].font});
    if (scripts[i].span.end() < fonts[j].span.end()) ++i;
    else if (fonts[j].span.end() < scripts[i].span.end()) ++j;
    else { ++i; ++j; }
  }
  return out;
}

// 不变量：每个不降级的核心格，其 UTF-16 区间完整落在一个项里（§5.0）。
// hasAll(font, cell)：字体是否含该格全部码点（零宽格式字符由调用方忽略）。
inline ItemPlan planItems(const std::vector<TextSpan> &cells, const std::vector<ScriptRange> &scripts,
                          const std::vector<FontRange> &fonts, const std::vector<bool> &rtl,
                          const std::function<bool(uint16_t font, uint16_t cell)> &hasAll) {
  ItemPlan plan;
  const uint32_t n = cells.empty() ? 0 : cells.back().end();
  constexpr int kNone = -1;
  std::vector<int> scriptOf(n, kNone), fontOf(n, kNone);
  for (size_t s = 0; s < scripts.size(); ++s)
    for (uint32_t u = scripts[s].span.begin; u < scripts[s].span.end() && u < n; ++u) scriptOf[u] = int(s);
  for (const auto &f : fonts)
    for (uint32_t u = f.span.begin; u < f.span.end() && u < n; ++u) fontOf[u] = f.font;

  std::vector<bool> legacy(cells.size(), false);
  for (size_t k = 0; k < cells.size(); ++k) {
    const TextSpan sp = cells[k];
    // 1. 按格统一字体：格内出现的字体按出现顺序尝试
    std::vector<int> seen;
    bool covered = true;
    for (uint32_t u = sp.begin; u < sp.end(); ++u) {
      if (fontOf[u] == kNone || scriptOf[u] == kNone) { covered = false; break; }
      if (std::find(seen.begin(), seen.end(), fontOf[u]) == seen.end()) seen.push_back(fontOf[u]);
    }
    if (!covered) { legacy[k] = true; continue; }
    if (seen.size() > 1) {
      auto it = std::find_if(seen.begin(), seen.end(), [&](int f) { return hasAll(uint16_t(f), uint16_t(k)); });
      if (it == seen.end()) { legacy[k] = true; continue; }
      for (uint32_t u = sp.begin; u < sp.end(); ++u) fontOf[u] = *it;
    }
    // 3. 对齐检查：格内跨 script 区间（项边界会落在格内部）→ 降级，不做 script 合并
    for (uint32_t u = sp.begin + 1; u < sp.end(); ++u)
      if (scriptOf[u] != scriptOf[sp.begin]) { legacy[k] = true; break; }
  }
  // 2 + 4. 按 script 区间、字体切分，RTL 格自成一项，挖掉降级格
  bool open = false;
  ItemRange cur;
  auto flush = [&] { if (open && cur.span.length) plan.items.push_back(cur); open = false; };
  for (size_t k = 0; k < cells.size(); ++k) {
    if (legacy[k]) { plan.legacyCells.push_back(uint16_t(k)); flush(); continue; }
    const bool isRtl = k < rtl.size() && rtl[k];
    const TextSpan sp = cells[k];
    if (isRtl) flush();
    for (uint32_t u = sp.begin; u < sp.end(); ++u) {
      const uint16_t s = uint16_t(scriptOf[u]), f = uint16_t(fontOf[u]);
      if (open && (cur.scriptRange != s || cur.font != f)) flush();
      if (!open) { cur = {{u, 0}, s, f}; open = true; }
      ++cur.span.length;
    }
    if (isRtl) flush();
  }
  flush();
  return plan;
}

// ---- §8 塑形结果（与列坐标无关）----
struct ShapedItem {
  TextSpan span;                       // 在段文本中的范围
  std::shared_ptr<void> face;          // 平台字体面（删除器负责 Release）；测试可为空
  float emPx = 0;
  uint16_t script = 0; uint8_t shapes = 0;
  std::vector<uint16_t> glyphIndices;
  std::vector<float> advancesPx, offsetXPx, offsetYPx;
  std::vector<uint16_t> clusterMap;    // 项内每个 UTF-16 单元 → 首个字形下标
};
struct ShapedRun {
  std::vector<ShapedItem> items;
  std::vector<uint16_t> legacyCells;   // §5.0 的降级格：格序号，升序
};

// ---- §5 定位 ----
struct DrawPiece {
  enum Kind { Glyphs, LegacyCell } kind = Glyphs;
  // Glyphs
  uint16_t item = 0;
  uint32_t glyphBegin = 0, glyphEnd = 0;
  float originXPx = 0;                 // 第一个字形的绝对笔位（相对行首，不含内边距）
  std::vector<float> advancesPx;       // advances[i] = 笔位[i+1] − 笔位[i]；最后一个取自然 advance
  float scale = 1.f;                   // ≠1：超宽单元，绕 originXPx 水平缩放
  // LegacyCell
  uint16_t cell = 0;                   // 格序号；文字取 run.text 的 cellSpans[cell]
  // 两者都有
  ColRange clip;
};
struct ShapedRunView {
  const std::vector<ShapedItem> &items;
  const std::vector<uint16_t> &legacyCells;
};

namespace detail {
inline uint16_t cellWidthAt(const std::vector<uint8_t> &widthOfCol, uint16_t col) {
  return col < widthOfCol.size() && widthOfCol[col] ? widthOfCol[col] : 1;
}
inline bool clusterMapValid(const ShapedItem &it) {
  if (it.clusterMap.size() != it.span.length || it.glyphIndices.empty()) return false;
  if (it.advancesPx.size() != it.glyphIndices.size()) return false;
  for (size_t u = 0; u < it.clusterMap.size(); ++u) {
    if (it.clusterMap[u] >= it.glyphIndices.size()) return false;
    if (u && it.clusterMap[u] < it.clusterMap[u - 1]) return false;
  }
  return it.clusterMap[0] == 0;
}
}  // namespace detail

// widthOfCol：按绝对列索引的核心宽度（尾格 0）；cellWPx：格宽像素。
// 输出按列排序；每个核心格只出现在一个片段里。
inline std::vector<DrawPiece> placeOnGrid(const ShapedRunView &shaped, const TextRun &run,
                                          const std::vector<uint8_t> &widthOfCol, float cellWPx) {
  struct Ordered { uint16_t col; DrawPiece piece; };
  std::vector<Ordered> out;
  // 单元→格序号
  std::vector<uint16_t> cellOfUnit(run.text.size(), 0);
  for (size_t k = 0; k < run.cellSpans.size(); ++k)
    for (uint32_t u = run.cellSpans[k].begin; u < run.cellSpans[k].end() && u < cellOfUnit.size(); ++u)
      cellOfUnit[u] = uint16_t(k);
  auto lastCol = [&](uint16_t k) { return uint16_t(run.cellCols[k] + detail::cellWidthAt(widthOfCol, run.cellCols[k]) - 1); };
  auto legacy = [&](uint16_t k) {
    DrawPiece p; p.kind = DrawPiece::LegacyCell; p.cell = k;
    p.clip = {run.cellCols[k], detail::cellWidthAt(widthOfCol, run.cellCols[k])};
    out.push_back({run.cellCols[k], std::move(p)});
  };
  for (uint16_t k : shaped.legacyCells) if (k < run.cellCols.size()) legacy(k);

  for (size_t ii = 0; ii < shaped.items.size(); ++ii) {
    const ShapedItem &it = shaped.items[ii];
    if (it.span.end() > run.text.size() || it.span.length == 0) continue;
    if (!detail::clusterMapValid(it)) {  // 防御：塑形结果不可用 → 这个项的格按旧方式
      uint16_t prev = UINT16_MAX;
      for (uint32_t u = it.span.begin; u < it.span.end(); ++u)
        if (cellOfUnit[u] != prev) { prev = cellOfUnit[u]; legacy(prev); }
      continue;
    }
    const uint32_t nGlyphs = uint32_t(it.glyphIndices.size());
    // 簇：clusterMap 相同的连续单元
    struct Unit { uint16_t cellA, cellB; uint32_t g0, g1; };
    std::vector<Unit> units;
    for (uint32_t u = 0; u < it.span.length;) {
      uint32_t v = u + 1;
      while (v < it.span.length && it.clusterMap[v] == it.clusterMap[u]) ++v;
      const uint32_t g0 = it.clusterMap[u], g1 = v < it.span.length ? it.clusterMap[v] : nGlyphs;
      const uint16_t ca = cellOfUnit[it.span.begin + u], cb = cellOfUnit[it.span.begin + v - 1];
      if (!units.empty() && ca <= units.back().cellB) {  // 与上一个簇共享格 → 同一定位单元
        units.back().cellB = std::max(units.back().cellB, cb);
        units.back().g1 = std::max(units.back().g1, g1);
      } else {
        units.push_back({ca, cb, g0, g1});
      }
      u = v;
    }
    // 每个单元的笔位与缩放
    std::vector<float> pen(nGlyphs, 0.f);
    std::vector<float> unitScale(units.size(), 1.f);
    std::vector<uint32_t> unitOfGlyph(nGlyphs, 0);
    for (size_t ui = 0; ui < units.size(); ++ui) {
      const Unit &un = units[ui];
      const uint16_t c0 = run.cellCols[un.cellA], c1 = lastCol(un.cellB);
      const float boxPx = float(c1 - c0 + 1) * cellWPx;
      float natural = 0;
      for (uint32_t g = un.g0; g < un.g1; ++g) natural += it.advancesPx[g];
      const GlyphFit fit = fit_box(natural, boxPx);
      unitScale[ui] = fit.scale;
      float x = float(c0) * cellWPx + (fit.scale < 1.f ? 0.f : fit.x_offset);
      for (uint32_t g = un.g0; g < un.g1; ++g) { pen[g] = x; x += it.advancesPx[g]; unitOfGlyph[g] = uint32_t(ui); }
    }
    // 片段：非超宽单元连续合并；超宽单元单独成片段
    auto emit = [&](size_t uA, size_t uB, bool scaled) {  // 单元 [uA, uB]
      DrawPiece p; p.kind = DrawPiece::Glyphs; p.item = uint16_t(ii);
      p.glyphBegin = units[uA].g0; p.glyphEnd = units[uB].g1;
      if (p.glyphBegin >= p.glyphEnd) return;
      p.originXPx = pen[p.glyphBegin];
      for (uint32_t g = p.glyphBegin; g < p.glyphEnd; ++g)
        p.advancesPx.push_back(g + 1 < p.glyphEnd ? pen[g + 1] - pen[g] : it.advancesPx[g]);
      if (scaled) {
        p.scale = unitScale[uA];
        const uint16_t c0 = run.cellCols[units[uA].cellA];
        p.clip = {c0, uint16_t(lastCol(units[uA].cellB) - c0 + 1)};
      } else {
        p.clip = run.cols;
      }
      out.push_back({run.cellCols[units[uA].cellA], std::move(p)});
    };
    size_t start = 0;
    for (size_t ui = 0; ui <= units.size(); ++ui) {
      const bool end = ui == units.size();
      const bool scaled = !end && unitScale[ui] < 1.f;
      if (end || scaled) {
        if (ui > start) emit(start, ui - 1, false);
        if (scaled) emit(ui, ui, true);
        start = ui + 1;
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Ordered &a, const Ordered &b) { return a.col < b.col; });
  std::vector<DrawPiece> pieces;
  pieces.reserve(out.size());
  for (auto &o : out) pieces.push_back(std::move(o.piece));
  return pieces;
}

// ---- §7 块光标隔离 ----
struct IsolateState {
  bool active = false;
  uint16_t row = 0;
  ColRange cols;
  bool operator==(const IsolateState &) const = default;
};
// 隔离状态变化时要重画的行（旧行与新行）；不变时为空。
inline std::vector<uint16_t> isolateDirtyRows(const IsolateState &prev, const IsolateState &next) {
  std::vector<uint16_t> rows;
  if (prev == next) return rows;
  if (prev.active) rows.push_back(prev.row);
  if (next.active && !(prev.active && prev.row == next.row)) rows.push_back(next.row);
  return rows;
}

// ---- §8 塑形缓存 ----
struct ShapeKey {
  std::u16string text;
  uint8_t format = 0;
  bool ligatures = false;
  std::vector<uint16_t> cellLengths;   // 每个核心格的 UTF-16 长度，按顺序；与列号无关
  bool operator==(const ShapeKey &) const = default;
};
inline ShapeKey shapeKeyFor(const TextRun &run, bool ligatures) {
  ShapeKey k{run.text, run.format, ligatures, {}};
  k.cellLengths.reserve(run.cellSpans.size());
  for (const auto &s : run.cellSpans) k.cellLengths.push_back(uint16_t(s.length));
  return k;
}
struct ShapeKeyHash {
  size_t operator()(const ShapeKey &k) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (char16_t c : k.text) mix(c);
    mix(k.format); mix(k.ligatures ? 1 : 2);
    for (uint16_t l : k.cellLengths) mix(0x10000u + l);
    return size_t(h);
  }
};
inline size_t shapedRunBytes(const ShapeKey &k, const ShapedRun &r) {
  size_t n = k.text.size() * 2 + k.cellLengths.size() * 2 + r.legacyCells.size() * 2 + 64;
  for (const auto &it : r.items) n += size_t(it.span.length) * 2 + it.glyphIndices.size() * 14 + 64;
  return n;
}

struct ShapeCacheStats { uint64_t hits = 0, misses = 0, evictions = 0, bypassed = 0; };

class ShapeCache {
 public:
  explicit ShapeCache(size_t budgetBytes = 4u << 20) : budget_(budgetBytes) {}
  std::shared_ptr<const ShapedRun> find(const ShapeKey &k) {
    auto it = map_.find(k);
    if (it == map_.end()) { ++st_.misses; return nullptr; }
    ++st_.hits;
    lru_.splice(lru_.begin(), lru_, it->second.pos);
    return it->second.value;
  }
  // 单项超过预算的 1/4 不进缓存（bypassed）；调用方照常使用自己手里的结果。
  void put(ShapeKey k, std::shared_ptr<const ShapedRun> v) {
    if (!v) return;
    const size_t cost = shapedRunBytes(k, *v);
    if (cost > budget_ / 4) { ++st_.bypassed; return; }
    if (auto it = map_.find(k); it != map_.end()) { bytes_ -= it->second.cost; lru_.erase(it->second.pos); map_.erase(it); }
    while (!lru_.empty() && bytes_ + cost > budget_) {
      auto victim = map_.find(lru_.back());
      bytes_ -= victim->second.cost;
      map_.erase(victim);
      lru_.pop_back();
      ++st_.evictions;
    }
    lru_.push_front(k);
    map_.emplace(std::move(k), Entry{std::move(v), lru_.begin(), cost});
    bytes_ += cost;
  }
  void clear() { map_.clear(); lru_.clear(); bytes_ = 0; }
  const ShapeCacheStats &stats() const { return st_; }
  void resetStats() { st_ = {}; }
  size_t size() const { return map_.size(); }
  size_t bytes() const { return bytes_; }

 private:
  struct Entry { std::shared_ptr<const ShapedRun> value; std::list<ShapeKey>::iterator pos; size_t cost; };
  size_t budget_, bytes_ = 0;
  std::list<ShapeKey> lru_;
  std::unordered_map<ShapeKey, Entry, ShapeKeyHash> map_;
  ShapeCacheStats st_;
};

// ---- 一段的绘制计划（底层与光标覆盖层共用，§7）----
struct RunPlan {
  std::shared_ptr<const ShapedRun> shaped;  // 空：整段旧方式（塑形失败）
  std::vector<DrawPiece> pieces;
  bool legacyWhole() const { return !shaped; }
};
struct PlanCounters { uint64_t shapeFailures = 0, legacyCells = 0; };
// shaper：平台塑形（分析 + 回退 + GetGlyphs），失败返回空。缓存命中时不调用。
inline RunPlan planRun(const TextRun &run, const std::vector<uint8_t> &widthOfCol, float cellWPx, bool ligatures,
                       ShapeCache &cache, const std::function<std::shared_ptr<const ShapedRun>(const TextRun &)> &shaper,
                       PlanCounters &counters) {
  RunPlan plan;
  ShapeKey key = shapeKeyFor(run, ligatures);
  plan.shaped = cache.find(key);
  if (!plan.shaped) {
    plan.shaped = shaper ? shaper(run) : nullptr;
    if (!plan.shaped) { ++counters.shapeFailures; return plan; }
    cache.put(std::move(key), plan.shaped);
  }
  plan.pieces = placeOnGrid({plan.shaped->items, plan.shaped->legacyCells}, run, widthOfCol, cellWPx);
  counters.legacyCells += plan.shaped->legacyCells.size();
  return plan;
}

// 光标格的计划随隔离状态保存；覆盖层只在状态与行内容都没变时使用，不重新塑形。
struct IsolatedPlan {
  IsolateState state;
  uint64_t rowRevision = 0;
  std::optional<RunPlan> plan;
  TextRun run;
  bool usableFor(const IsolateState &s, uint64_t revision) const { return plan && state == s && rowRevision == revision; }
  void invalidate() { plan.reset(); }
};

// ---- §9 文本计时与 render.stats ----
// 时钟可注入（纳秒）。只有 begin/end 之间的时间计入本帧样本。
class TextTimer {
 public:
  explicit TextTimer(std::function<uint64_t()> nowNs) : now_(std::move(nowNs)) {}
  void begin() { if (!running_) { t0_ = now_(); running_ = true; } }
  void end() { if (running_) { acc_ += now_() - t0_; running_ = false; } }
  uint64_t takeFrameNs() { end(); const uint64_t v = acc_; acc_ = 0; return v; }
 private:
  std::function<uint64_t()> now_;
  uint64_t t0_ = 0, acc_ = 0;
  bool running_ = false;
};

enum class FrameClass { NoText, Partial, Full };
inline FrameClass classifyFrame(uint32_t rowsRedrawn, uint32_t rowsVisible) {
  if (rowsRedrawn == 0) return FrameClass::NoText;
  return rowsRedrawn >= rowsVisible ? FrameClass::Full : FrameClass::Partial;
}

struct Percentiles { uint64_t count = 0, p50 = 0, p95 = 0, max = 0; };
inline Percentiles percentiles(std::vector<uint64_t> v) {
  Percentiles p; p.count = v.size();
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {  // 最近秩：第 ceil(q·n) 个（从 1 计）
    const size_t rank = std::max<size_t>(1, size_t(std::ceil(q * double(v.size()))));
    return v[std::min(rank, v.size()) - 1];
  };
  p.p50 = at(0.5); p.p95 = at(0.95); p.max = v.back();
  return p;
}

struct RenderStatsSummary {
  uint64_t noTextFrames = 0;
  Percentiles partial, full;     // 纳秒
  uint64_t rowsRedrawn = 0;
  ShapeCacheStats cache;         // 本窗口的增量
  uint64_t shapeFailures = 0, legacyCells = 0;
  bool cold = false;
};

// 窗口：10 秒或 1000 个样本，先到者为准；add 返回窗口满时的汇总并清零。
class RenderStatsWindow {
 public:
  static constexpr uint64_t kWindowNs = 10'000'000'000ull;
  static constexpr size_t kMaxSamples = 1000;
  void markCold() { cold_ = true; }
  std::optional<RenderStatsSummary> add(uint64_t nowNs, uint32_t rowsRedrawn, uint32_t rowsVisible, uint64_t textNs,
                                        const ShapeCacheStats &cacheTotal, const PlanCounters &counters) {
    if (!started_) { start_ = nowNs; started_ = true; cacheBase_ = cacheTotal; countersBase_ = counters; }
    switch (classifyFrame(rowsRedrawn, rowsVisible)) {
      case FrameClass::NoText: ++noText_; break;
      case FrameClass::Partial: partial_.push_back(textNs); break;
      case FrameClass::Full: full_.push_back(textNs); break;
    }
    rows_ += rowsRedrawn;
    const size_t samples = noText_ + partial_.size() + full_.size();
    if (nowNs - start_ < kWindowNs && samples < kMaxSamples) return std::nullopt;
    RenderStatsSummary s;
    s.noTextFrames = noText_; s.partial = percentiles(partial_); s.full = percentiles(full_);
    s.rowsRedrawn = rows_; s.cold = cold_;
    s.cache.hits = cacheTotal.hits - cacheBase_.hits; s.cache.misses = cacheTotal.misses - cacheBase_.misses;
    s.cache.evictions = cacheTotal.evictions - cacheBase_.evictions; s.cache.bypassed = cacheTotal.bypassed - cacheBase_.bypassed;
    s.shapeFailures = counters.shapeFailures - countersBase_.shapeFailures;
    s.legacyCells = counters.legacyCells - countersBase_.legacyCells;
    *this = RenderStatsWindow{};
    return s;
  }
 private:
  bool started_ = false, cold_ = false;
  uint64_t start_ = 0, noText_ = 0, rows_ = 0;
  std::vector<uint64_t> partial_, full_;
  ShapeCacheStats cacheBase_;
  PlanCounters countersBase_;
};

struct RenderStatsContext {
  const char *path = "glyphrun";  // glyphrun / drawtext
  bool ligatures = false;
  float dpiScale = 1.f;
  uint16_t cols = 0, rows = 0;
};
inline std::string formatRenderStats(const RenderStatsSummary &s, const RenderStatsContext &c) {
  auto us = [](uint64_t ns) { return std::to_string(ns / 1000); };
  auto pct = [&](const Percentiles &p, const char *name) {  // name=样本数/p50/p95/max（微秒）
    return std::string(" ") + name + "=" + std::to_string(p.count) + "/" + us(p.p50) + "/" + us(p.p95) + "/" + us(p.max);
  };
  const uint64_t lookups = s.cache.hits + s.cache.misses;
  std::string hit = lookups ? std::to_string(s.cache.hits * 100 / lookups) + "%" : "-";
  char dpi[16];
  std::snprintf(dpi, sizeof dpi, "%.2f", double(c.dpiScale));
  return std::string("path=") + c.path + " lig=" + (c.ligatures ? "1" : "0") + " dpi=" + dpi +
         " grid=" + std::to_string(c.cols) + "x" + std::to_string(c.rows) + " cold=" + (s.cold ? "1" : "0") +
         " notext=" + std::to_string(s.noTextFrames) + pct(s.partial, "partial_us") + pct(s.full, "full_us") +
         " rows=" + std::to_string(s.rowsRedrawn) + " cache=" + hit + "(" + std::to_string(s.cache.hits) + "/" +
         std::to_string(lookups) + ",bypass " + std::to_string(s.cache.bypassed) + ")" +
         " shape_fail=" + std::to_string(s.shapeFailures) + " legacy_cells=" + std::to_string(s.legacyCells);
}

}  // namespace itren
