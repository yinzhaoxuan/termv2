/* 搜索高亮层（平台无关，草案 itcore_search v0.2 §2.6）：独立于 cell flag 的渲染层，
   每帧按视口的命中区间建表，resolve_cell 逐格查询后叠色。核心网格不改。 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace itren {

// 镜像草案 itcore_search_range（/workspace/qa/itcore_search-草案-v0.2.md §1.3 itcore_search_ranges_in_view）：
// row 为绝对行号，start_col 含、end_col 不含，均按格计。itcore.h 加入该类型后可直接逐字段拷贝过来。
struct SearchRange {
  int64_t row = 0;
  uint16_t start_col = 0;
  uint16_t end_col = 0;     // 开区间；宽字符命中时越过尾格（首格 + 2）
  bool is_current = false;
};

enum class SearchHit : uint8_t { None = 0, Other = 1, Current = 2 };

// 默认配色：上游 iTerm2 的查找命中是黄底黑字；上游的当前命中同时被设成选区，
// 这里当前命中没有设成选区时用更醒目的橙底黑字。以后从配置读取。
struct SearchColors {
  uint32_t other_fg = 0x000000, other_bg = 0xFFFF00;
  uint32_t current_fg = 0x000000, current_bg = 0xFF9632;
};
inline constexpr SearchColors kDefaultSearchColors{};

// 视口高亮表：rows×cols 个字节，kind() 为 O(1)。
// 宽字符：区间按格给出，覆盖首格时 end_col 已越过尾格；渲染按首格的颜色画整个宽字符，
// 所以只覆盖尾格的区间（不应出现）对显示无影响。
class SearchHighlight {
 public:
  // first_row：视口首行的绝对行号（同 itcore_search_ranges_in_view 的 first_row）。
  // 输入可无序、可重叠、可越界、可为空区间：越界部分裁掉，空区间忽略，重叠时 Current 优先。
  void build(const SearchRange *ranges, size_t n, int64_t first_row, uint16_t rows, uint16_t cols) {
    rows_ = rows; cols_ = cols;
    kind_.assign(size_t(rows) * cols, uint8_t(SearchHit::None));
    row_any_.assign(rows, 0);
    any_ = false;
    for (size_t i = 0; i < n && ranges; ++i) {
      const SearchRange &r = ranges[i];
      if (r.row < first_row || r.row - first_row >= int64_t(rows)) continue;
      const uint16_t y = uint16_t(r.row - first_row);
      const uint16_t a = r.start_col, b = std::min<uint16_t>(r.end_col, cols);
      if (a >= b) continue;
      const uint8_t k = uint8_t(r.is_current ? SearchHit::Current : SearchHit::Other);
      uint8_t *p = &kind_[size_t(y) * cols];
      for (uint16_t x = a; x < b; ++x) p[x] = std::max(p[x], k);
      row_any_[y] = 1; any_ = true;
    }
  }
  void build(const std::vector<SearchRange> &v, int64_t first_row, uint16_t rows, uint16_t cols) {
    build(v.data(), v.size(), first_row, rows, cols);
  }
  // RESTARTED / clearSearch：清掉整层（保留尺寸，之后 kind() 一律 None）
  void clear() { std::fill(kind_.begin(), kind_.end(), uint8_t(0)); std::fill(row_any_.begin(), row_any_.end(), uint8_t(0)); any_ = false; }

  SearchHit kind(uint16_t row, uint16_t col) const {
    if (!any_ || row >= rows_ || col >= cols_) return SearchHit::None;
    return SearchHit(kind_[size_t(row) * cols_ + col]);
  }
  bool any() const { return any_; }
  bool row_any(uint16_t row) const { return row < rows_ && row_any_[row]; }
  uint16_t rows() const { return rows_; }
  uint16_t cols() const { return cols_; }

  // 新旧两层之间需要重画的行（视口内行号）。尺寸不同则全部行。
  static std::vector<uint16_t> dirty_rows(const SearchHighlight &old_h, const SearchHighlight &new_h) {
    std::vector<uint16_t> out;
    const uint16_t rows = new_h.rows_;
    if (old_h.rows_ != new_h.rows_ || old_h.cols_ != new_h.cols_) {
      for (uint16_t y = 0; y < rows; ++y) out.push_back(y);
      return out;
    }
    for (uint16_t y = 0; y < rows; ++y) {
      if (!old_h.row_any_[y] && !new_h.row_any_[y]) continue;
      const size_t o = size_t(y) * new_h.cols_;
      if (!std::equal(old_h.kind_.begin() + o, old_h.kind_.begin() + o + new_h.cols_, new_h.kind_.begin() + o)) out.push_back(y);
    }
    return out;
  }

 private:
  uint16_t rows_ = 0, cols_ = 0;
  std::vector<uint8_t> kind_;
  std::vector<uint8_t> row_any_;
  bool any_ = false;
};

}  // namespace itren
