// SPDX-License-Identifier: GPL-2.0-only
//
// iTerm2 Windows 派生核心：滚动历史纯文本搜索（SRCH-01，草案 itcore_search v0.2）。
// 行为对齐上游 iTerm2 的 SearchEngine / FindContext / LineBuffer findSubstring / iTermCoreSearch
// （按原始逻辑行搜索、软换行拼接、smart case、回绕到起点）；v0.1 不做正则、不做变音符 / 全半角折叠。
#pragma once
#include "itcore.h"
#include "screen.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace itcore {

// 段定位：文档位置 p ∈ [0, count)，主屏 = 历史逻辑段 [0, H) + 网格行 [H, H+rows)；备用屏 H = 0。
// 序号 ord = 累计淘汰数 + p（正常滚动下对同一段不变）。显示行按当前列宽（同 reflow_merged）计。
class SearchLocator {
public:
    explicit SearchLocator(const Screen &s) : s_(s) {}
    void sync();
    uint64_t ev() const { return ev_; }
    int64_t hist() const { return H_; }
    int64_t count() const { return H_ + R_; }
    int64_t hist_rows() const { return hist_rows_; }
    uint64_t id_at(int64_t p) const;
    bool hard_at(int64_t p) const;
    bool find(uint64_t id, int64_t *p) const;
    void chars(int64_t p, std::vector<const Cell *> &out) const;
    struct CellPos { int64_t row; uint16_t col; uint8_t w; };  // row 为绝对行号
    void layout(int64_t p, std::vector<CellPos> &out) const;
    // 绝对行 [r0, r1] → 可能覆盖它们的段位置区间；无交集返回 false
    bool rows_to_pos(int64_t r0, int64_t r1, int64_t *p0, int64_t *p1) const;
    uint64_t grid_rev(int64_t p) const;  // 网格段的 Row.revision；历史段 0
private:
    const Screen &s_;
    uint64_t gen_ = ~0ull, ev_ = 0;
    uint16_t cols_ = 0;
    bool alt_ = false, built_ = false;
    int64_t H_ = 0, R_ = 0, hist_rows_ = 0;
    std::unordered_map<uint64_t, int64_t> hmap_;
    std::unordered_map<uint64_t, int64_t> gmap_;
    std::vector<int64_t> lrow_;
    std::vector<uint16_t> lcol_;
};

class Searcher {
public:
    static int create(Screen &s, const char *needle, size_t len, const itcore_search_options *opt, Searcher **out);
    int step(uint32_t budget_cells, uint32_t budget_us, itcore_search_progress *prog, itcore_search_match *buf,
             uint32_t cap, uint32_t *n_out);
    int next(itcore_search_match *out, int64_t *index) { return move(true, out, index); }
    int prev(itcore_search_match *out, int64_t *index) { return move(false, out, index); }
    int select(int64_t index);
    int ranges(int64_t first_row, uint32_t rows, itcore_search_range *out, uint32_t cap);
    void supersede() { superseded_ = true; }
    // 失效事件：句柄因屏幕失效计数变化而失效、且尚未通知过时返回 true 并记为已通知
    bool take_invalidation_notice();
    bool dead() const { return superseded_ || s_.search_serial() != serial_; }
    static int to_view(const Screen &s, const itcore_search_match *m, int32_t *sr, uint16_t *sc, int32_t *er, uint16_t *ec);
private:
    explicit Searcher(Screen &s) : s_(s), loc_(s) {}
    struct M { uint64_t sseg, eseg; uint32_t soff, eoff; int64_t sord, eord; };
    struct Key { int64_t ord; uint32_t off; };
    static bool less(const M &a, const M &b) { return a.sord != b.sord ? a.sord < b.sord : a.soff < b.soff; }
    void refresh();
    bool relocate(uint64_t seg, int64_t *ord) const;
    bool in_range_id(uint64_t id) const { return id <= max_id_ && !excluded_.count(id); }
    bool in_range(int64_t p) const { return in_range_id(loc_.id_at(p)); }
    void group_at(int64_t p, int64_t *a, int64_t *b) const;
    uint64_t scan_group(int64_t a, int64_t b, std::vector<char> &kill, std::vector<M> &fresh);
    void find_in(int64_t a, int64_t b, std::vector<M> &out, uint64_t *cells) const;
    bool scanned_range(int64_t p0, int64_t p1) const;
    size_t lower(Key k) const;
    int64_t cur_index() const;
    void fill(const M &m, itcore_search_match *out) const;
    int move(bool fwd, itcore_search_match *out, int64_t *index);
    bool done() const { return next_ >= order_.size(); }

    Screen &s_;
    SearchLocator loc_;
    std::vector<uint32_t> needle_;
    bool fold_ = false;
    uint32_t flags_ = 0, max_results_ = 100000, max_cells_ = 1024;
    uint64_t serial_ = 0;
    bool superseded_ = false, restarted_reported_ = false, notice_sent_ = false;
    uint64_t max_id_ = 0;
    std::unordered_set<uint64_t> excluded_;  // begin 时光标行及以下的全空网格行：之后写进去的算新输出
    std::vector<uint64_t> order_, weight_;
    size_t next_ = 0;
    uint64_t total_cells_ = 0, done_cells_ = 0;
    std::unordered_set<uint64_t> scanned_;
    std::unordered_map<uint64_t, uint64_t> rev_;
    std::vector<M> ms_;
    // 参照点：初始为起点锚（next 含、prev 不含），之后为当前结果（两向都不含）
    uint64_t ref_seg_ = 0; uint32_t ref_off_ = 0; int64_t ref_ord_ = 0;
    bool ref_incl_ = true, has_cur_ = false;
    uint32_t pflags_ = 0, dropped_ = 0, changed_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> pending_;  // 尚未经 step 交给调用方的新结果（start 锚）
    uint64_t key_gen_ = ~0ull, key_ev_ = ~0ull, key_rev_ = ~0ull;
};

}  // namespace itcore
