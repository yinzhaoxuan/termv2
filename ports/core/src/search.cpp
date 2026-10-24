// SPDX-License-Identifier: GPL-2.0-only
//
// iTerm2 Windows 派生核心：滚动历史纯文本搜索（SRCH-01）。见 search.h 与 itcore.h 的 itcore_search_* 注释。
#include "search.h"
#include <algorithm>
#include <chrono>
#include <climits>

namespace itcore {
namespace {

struct FoldPair { uint32_t cp, to; };
const FoldPair kFold[] = {
#include "case_fold_table.inc"
};

uint32_t fold_cp(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    size_t lo = 0, hi = sizeof(kFold) / sizeof(kFold[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kFold[mid].cp < cp) lo = mid + 1; else hi = mid;
    }
    if (lo < sizeof(kFold) / sizeof(kFold[0]) && kFold[lo].cp == cp) return kFold[lo].to;
    return cp;
}

bool is_vs(uint32_t cp) { return cp == 0xFE0E || cp == 0xFE0F; }

// 把一个字素簇的码点追加到 out（gid 0 = 空格）
void append_cluster(const GraphemePool &pool, uint32_t gid, bool fold, std::vector<uint32_t> &out) {
    if (gid == 0) { out.push_back(' '); return; }
    size_t n = 0;
    const char *p = pool.get(gid, &n);
    if (!p) { out.push_back(' '); return; }
    const uint8_t *u = reinterpret_cast<const uint8_t *>(p);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0; size_t adv = 1;
        utf8_next(u + i, n - i, &cp, &adv);
        if (!adv) adv = 1;
        i += adv;
        if (fold) {
            if (is_vs(cp)) continue;
            cp = fold_cp(cp);
        }
        out.push_back(cp);
    }
}

}  // namespace

// ---------------- SearchLocator ----------------

void SearchLocator::sync() {
    const Scrollback &sb = s_.scrollback();
    const Grid &g = s_.grid();
    const bool alt = s_.modes().alt;
    ev_ = sb.evicted();
    H_ = alt ? 0 : int64_t(sb.logical_count());
    R_ = g.rows();
    if (!built_ || sb.generation() != gen_ || g.cols() != cols_ || alt != alt_) {
        built_ = true;
        gen_ = sb.generation();
        cols_ = g.cols();
        alt_ = alt;
        hmap_.clear();
        lrow_.assign(size_t(H_), 0);
        lcol_.assign(size_t(H_), 0);
        const int64_t cols = cols_ ? cols_ : 1;
        int64_t row = -1, col = 0;
        for (int64_t i = 0; i < H_; ++i) {
            const LogicalLine &ll = sb.at(size_t(i));
            hmap_[ll.id] = i;
            if (i == 0 || sb.at(size_t(i - 1)).hard) { ++row; col = 0; }
            lrow_[size_t(i)] = row;
            lcol_[size_t(i)] = uint16_t(col);
            for (const Cell &c : ll.cells) {
                int64_t w = c.width ? c.width : 1;
                if (col + w > cols) { ++row; col = 0; }
                col += w;
            }
        }
        hist_rows_ = H_ ? row + 1 : 0;
    }
    gmap_.clear();
    for (int y = 0; y < g.rows(); ++y) gmap_[g.row(y).id] = H_ + y;
}

uint64_t SearchLocator::id_at(int64_t p) const {
    if (p < H_) return s_.scrollback().at(size_t(p)).id;
    return s_.grid().row(int(p - H_)).id;
}

bool SearchLocator::hard_at(int64_t p) const {
    if (p < H_) return s_.scrollback().at(size_t(p)).hard;
    return s_.grid().row(int(p - H_)).wrap == ITCORE_WRAP_HARD;
}

uint64_t SearchLocator::grid_rev(int64_t p) const {
    if (p < H_) return 0;
    return s_.grid().row(int(p - H_)).revision;
}

bool SearchLocator::find(uint64_t id, int64_t *p) const {
    auto g = gmap_.find(id);
    if (g != gmap_.end()) { *p = g->second; return true; }
    auto h = hmap_.find(id);
    if (h != hmap_.end()) { *p = h->second; return true; }
    return false;
}

void SearchLocator::chars(int64_t p, std::vector<const Cell *> &out) const {
    out.clear();
    if (p < H_) {
        for (const Cell &c : s_.scrollback().at(size_t(p)).cells) out.push_back(&c);
        return;
    }
    const Row &r = s_.grid().row(int(p - H_));
    for (const Cell &c : r.cells) if (!(c.flags & ITCORE_CELL_CONTINUATION)) out.push_back(&c);
    if (r.wrap == ITCORE_WRAP_WIDE)
        while (!out.empty() && out.back()->grapheme_id == 0) out.pop_back();
}

void SearchLocator::layout(int64_t p, std::vector<CellPos> &out) const {
    out.clear();
    if (p < H_) {
        const int64_t cols = cols_ ? cols_ : 1;
        int64_t row = lrow_[size_t(p)], col = lcol_[size_t(p)];
        for (const Cell &c : s_.scrollback().at(size_t(p)).cells) {
            int64_t w = c.width ? c.width : 1;
            if (col + w > cols) { ++row; col = 0; }
            out.push_back({int64_t(ev_) + row, uint16_t(col), uint8_t(w)});
            col += w;
        }
        return;
    }
    const int64_t y = p - H_;
    const Row &r = s_.grid().row(int(y));
    const int64_t abs = int64_t(ev_) + hist_rows_ + y;
    for (size_t x = 0; x < r.cells.size(); ++x) {
        const Cell &c = r.cells[x];
        if (c.flags & ITCORE_CELL_CONTINUATION) continue;
        out.push_back({abs, uint16_t(x), uint8_t(c.width ? c.width : 1)});
    }
}

bool SearchLocator::rows_to_pos(int64_t r0, int64_t r1, int64_t *p0, int64_t *p1) const {
    const int64_t total = hist_rows_ + R_;
    int64_t d0 = r0 - int64_t(ev_), d1 = r1 - int64_t(ev_);
    if (d1 < 0 || d0 >= total || d1 < d0) return false;
    if (d0 < 0) d0 = 0;
    if (d1 >= total) d1 = total - 1;
    auto map = [&](int64_t d) -> int64_t {
        if (d < hist_rows_) {
            auto it = std::upper_bound(lrow_.begin(), lrow_.end(), d);
            int64_t i = int64_t(it - lrow_.begin()) - 1;
            return i < 0 ? 0 : i;
        }
        return H_ + (d - hist_rows_);
    };
    *p0 = map(d0);
    *p1 = map(d1);
    return true;
}

// ---------------- Searcher ----------------

int Searcher::create(Screen &s, const char *needle, size_t len, const itcore_search_options *opt, Searcher **out) {
    if (!needle || !len || !opt || !out || opt->struct_size < sizeof(itcore_search_options)) return ITCORE_SEARCH_EINVAL;
    const uint32_t known = ITCORE_SEARCH_F_REGEX | ITCORE_SEARCH_F_BACKWARD | ITCORE_SEARCH_F_WRAP;
    if (opt->flags & ~known) return ITCORE_SEARCH_EINVAL;
    if (opt->case_mode > ITCORE_SEARCH_CASE_INSENSITIVE) return ITCORE_SEARCH_EINVAL;
    if (opt->flags & ITCORE_SEARCH_F_REGEX) return ITCORE_SEARCH_EUNSUPPORTED;

    std::vector<uint32_t> raw;
    const uint8_t *u = reinterpret_cast<const uint8_t *>(needle);
    for (size_t i = 0; i < len;) {
        uint32_t cp = 0; size_t adv = 1;
        utf8_next(u + i, len - i, &cp, &adv);
        if (!adv) adv = 1;
        i += adv;
        raw.push_back(cp);
    }
    bool fold = opt->case_mode == ITCORE_SEARCH_CASE_INSENSITIVE;
    if (opt->case_mode == ITCORE_SEARCH_CASE_SMART) {
        fold = true;
        for (uint32_t cp : raw) if (fold_cp(cp) != cp) { fold = false; break; }
    }

    // 不区分大小写 / smart 折叠时去掉 VS15/16；折叠后 needle 为空则 EINVAL（区分大小写模式原样保留）
    std::vector<uint32_t> folded;
    for (uint32_t cp : raw) {
        if (fold) {
            if (is_vs(cp)) continue;
            cp = fold_cp(cp);
        }
        folded.push_back(cp);
    }
    if (fold && folded.empty()) return ITCORE_SEARCH_EINVAL;

    Searcher *h = new Searcher(s);
    h->fold_ = fold;
    h->needle_ = std::move(folded);
    h->flags_ = opt->flags;
    h->max_results_ = opt->max_results ? opt->max_results : 100000;
    h->max_cells_ = opt->max_match_cells ? opt->max_match_cells : 1024;
    h->serial_ = s.search_serial();

    SearchLocator &L = h->loc_;
    L.sync();
    const int64_t n = L.count();
    uint64_t mx = 0;
    for (int64_t p = 0; p < n; ++p) mx = std::max(mx, L.id_at(p));
    h->max_id_ = mx;
    {
        const Grid &g = s.grid();
        for (int y = s.grid().cursor().y; y < g.rows(); ++y) {
            bool blank = true;
            for (const Cell &c : g.row(y).cells) if (c.grapheme_id) { blank = false; break; }
            if (blank) h->excluded_.insert(g.row(y).id);
        }
    }

    // 起点
    int64_t sp = -1;
    uint32_t soff = 0;
    const bool back = opt->flags & ITCORE_SEARCH_F_BACKWARD;
    if (opt->start_seg && L.find(opt->start_seg, &sp)) soff = opt->start_off;
    else if (back) { sp = n - 1; soff = UINT32_MAX; }
    else { sp = L.hist(); soff = 0; }
    h->ref_seg_ = L.id_at(sp);
    h->ref_off_ = soff;
    h->ref_ord_ = int64_t(L.ev()) + sp;
    h->ref_incl_ = true;

    // 逻辑行分组（文档顺序），记首段 id 与格数
    std::vector<uint64_t> gid, gw;
    int64_t sg = 0;
    std::vector<const Cell *> cv;
    for (int64_t p = 0; p < n;) {
        int64_t a = p, b = p;
        while (!L.hard_at(b) && b + 1 < n) ++b;
        uint64_t w = 0;
        for (int64_t q = a; q <= b; ++q) { L.chars(q, cv); w += cv.size() ? cv.size() : 1; }
        if (sp >= a && sp <= b) sg = int64_t(gid.size());
        gid.push_back(L.id_at(a));
        gw.push_back(w);
        p = b + 1;
    }
    const int64_t gn = int64_t(gid.size());
    auto add = [&](int64_t i) { h->order_.push_back(gid[size_t(i)]); h->weight_.push_back(gw[size_t(i)]); h->total_cells_ += gw[size_t(i)]; };
    if (!back) {
        for (int64_t i = sg; i < gn; ++i) add(i);
        for (int64_t i = 0; i < sg; ++i) add(i);
    } else {
        for (int64_t i = sg; i >= 0; --i) add(i);
        for (int64_t i = gn - 1; i > sg; --i) add(i);
    }
    h->key_gen_ = s.scrollback().generation();
    h->key_ev_ = L.ev();
    h->key_rev_ = s.revision();
    *out = h;
    return 0;
}

bool Searcher::take_invalidation_notice() {
    if (superseded_ || notice_sent_ || s_.search_serial() == serial_) return false;
    notice_sent_ = true;
    return true;
}

bool Searcher::relocate(uint64_t seg, int64_t *ord) const {
    int64_t p = *ord - int64_t(loc_.ev());
    if (p >= 0 && p < loc_.count() && loc_.id_at(p) == seg) return true;
    if (!loc_.find(seg, &p)) return false;
    *ord = int64_t(loc_.ev()) + p;
    return true;
}

void Searcher::refresh() {
    loc_.sync();
    const uint64_t gen = s_.scrollback().generation(), ev = loc_.ev(), rev = s_.revision();
    if (gen == key_gen_ && ev == key_ev_ && rev == key_rev_) return;
    key_gen_ = gen; key_ev_ = ev; key_rev_ = rev;
    const int64_t cur = has_cur_ ? cur_index() : -1;
    (void)cur;
    size_t w = 0;
    for (size_t i = 0; i < ms_.size(); ++i) {
        M m = ms_[i];
        if (!relocate(m.sseg, &m.sord) || !relocate(m.eseg, &m.eord)) { ++dropped_; continue; }
        ms_[w++] = m;
    }
    ms_.resize(w);
    if (!std::is_sorted(ms_.begin(), ms_.end(), less)) std::stable_sort(ms_.begin(), ms_.end(), less);
    if (!relocate(ref_seg_, &ref_ord_)) {
        // 参照段已淘汰 / 消失：当前结果作废，next 从最老的存活位置开始
        has_cur_ = false;
        ref_incl_ = true;
        ref_ord_ = int64_t(ev);
        ref_off_ = 0;
        ref_seg_ = loc_.count() ? loc_.id_at(0) : 0;
    } else if (has_cur_ && cur_index() < 0) {
        has_cur_ = false;  // 当前结果所在行被改写掉；参照点保留在原位置
    }
}

size_t Searcher::lower(Key k) const {
    return size_t(std::lower_bound(ms_.begin(), ms_.end(), k, [](const M &m, const Key &k) {
        return m.sord != k.ord ? m.sord < k.ord : m.soff < k.off;
    }) - ms_.begin());
}

int64_t Searcher::cur_index() const {
    if (!has_cur_) return -1;
    size_t i = lower({ref_ord_, ref_off_});
    if (i < ms_.size() && ms_[i].sseg == ref_seg_ && ms_[i].soff == ref_off_) return int64_t(i);
    return -1;
}

void Searcher::group_at(int64_t p, int64_t *a, int64_t *b) const {
    const int64_t n = loc_.count();
    int64_t x = p, y = p;
    while (x > 0 && in_range(x - 1) && !loc_.hard_at(x - 1)) --x;
    while (!loc_.hard_at(y) && y + 1 < n && in_range(y + 1)) ++y;
    *a = x; *b = y;
}

void Searcher::find_in(int64_t a, int64_t b, std::vector<M> &out, uint64_t *cells) const {
    struct HC { int64_t p; uint32_t off; uint8_t w; uint32_t gid; size_t c0, c1; };
    std::vector<uint32_t> cps;
    std::vector<HC> hc;
    std::vector<const Cell *> cv;
    const GraphemePool &pool = s_.pool();
    for (int64_t p = a; p <= b; ++p) {
        loc_.chars(p, cv);
        *cells += cv.size() ? cv.size() : 1;
        for (size_t i = 0; i < cv.size(); ++i) {
            size_t c0 = cps.size();
            append_cluster(pool, cv[i]->grapheme_id, fold_, cps);
            hc.push_back({p, uint32_t(i), uint8_t(cv[i]->width ? cv[i]->width : 1), cv[i]->grapheme_id, c0, cps.size()});
        }
    }
    if (loc_.hard_at(b))  // 硬换行结尾的逻辑行：尾部空白格不参与匹配
        while (!hc.empty() && hc.back().gid == 0) { cps.resize(hc.back().c0); hc.pop_back(); }
    const size_t n = needle_.size();
    if (!n) return;
    const int64_t ev = int64_t(loc_.ev());
    for (size_t i = 0; i < hc.size();) {
        const size_t s = hc[i].c0;
        if (hc[i].c1 == s || s + n > cps.size() || cps[s] != needle_[0] ||
            !std::equal(needle_.begin(), needle_.end(), cps.begin() + std::ptrdiff_t(s))) { ++i; continue; }
        const size_t end = s + n;
        size_t k = i;
        uint64_t w = 0;
        while (k < hc.size() && hc[k].c1 < end) { w += hc[k].w; ++k; }
        if (k >= hc.size() || hc[k].c1 != end) { ++i; continue; }  // 止于簇中间
        w += hc[k].w;
        if (w > max_cells_) { ++i; continue; }
        M m;
        m.sseg = loc_.id_at(hc[i].p); m.soff = hc[i].off; m.sord = ev + hc[i].p;
        m.eseg = loc_.id_at(hc[k].p); m.eoff = hc[k].off; m.eord = ev + hc[k].p;
        out.push_back(m);
        i = k + 1;
    }
}

uint64_t Searcher::scan_group(int64_t a, int64_t b, std::vector<char> &kill, std::vector<M> &fresh) {
    std::vector<M> found;
    uint64_t cells = 0;
    find_in(a, b, found, &cells);
    const int64_t ev = int64_t(loc_.ev());
    // 旧结果：起点落在 [a, b] 内的
    size_t lo = lower({ev + a, 0}), hi = lower({ev + b + 1, 0});
    size_t removed = 0, kept = 0;
    std::vector<char> matched(found.size(), 0);
    for (size_t i = lo; i < hi; ++i) {
        if (kill[i]) continue;
        const M &o = ms_[i];
        bool same = false;
        for (size_t j = 0; j < found.size(); ++j)
            if (!matched[j] && found[j].sseg == o.sseg && found[j].soff == o.soff && found[j].eseg == o.eseg &&
                found[j].eoff == o.eoff) { matched[j] = 1; same = true; break; }
        if (same) { ++kept; continue; }
        kill[i] = 1;
        ++removed;
    }
    dropped_ += uint32_t(removed);
    for (size_t j = 0; j < found.size(); ++j) {
        if (matched[j]) continue;
        if (ms_.size() + fresh.size() - removed >= max_results_) { pflags_ |= ITCORE_SEARCH_PF_TRUNCATED; break; }
        fresh.push_back(found[j]);
        pending_.push_back({found[j].sseg, found[j].soff});
        ++changed_;
    }
    for (int64_t p = a; p <= b; ++p) {
        uint64_t id = loc_.id_at(p);
        scanned_.insert(id);
        if (p >= loc_.hist()) rev_[id] = loc_.grid_rev(p);
        else rev_.erase(id);
    }
    return cells;
}

int Searcher::step(uint32_t budget_cells, uint32_t budget_us, itcore_search_progress *prog, itcore_search_match *buf,
                   uint32_t cap, uint32_t *n_out) {
    if (n_out) *n_out = 0;
    if (!prog || prog->struct_size < sizeof(itcore_search_progress) || (cap && !buf)) return ITCORE_SEARCH_EINVAL;
    auto fill_dead = [&](int st) {
        prog->status = st; prog->progress = 0; prog->total = -1; prog->current = -1;
        prog->flags = pflags_; prog->dropped = 0; prog->changed = 0;
        return st;
    };
    if (superseded_) return fill_dead(ITCORE_SEARCH_RESTART);
    if (s_.search_serial() != serial_) {
        if (!restarted_reported_) { restarted_reported_ = true; return fill_dead(ITCORE_SEARCH_RESTARTED); }
        return fill_dead(ITCORE_SEARCH_RESTART);
    }
    const auto t0 = std::chrono::steady_clock::now();
    refresh();
    const int64_t n = loc_.count();
    std::vector<char> kill(ms_.size(), 0);
    std::vector<M> fresh;
    uint64_t used = 0;
    // 1) 校验：已扫网格行被改写 → 重扫所在逻辑行
    std::unordered_set<uint64_t> redone;
    for (int64_t p = loc_.hist(); p < n; ++p) {
        uint64_t id = loc_.id_at(p);
        if (id > max_id_) { pflags_ |= ITCORE_SEARCH_PF_NEW_CONTENT; continue; }
        if (excluded_.count(id)) {
            const Row &r = s_.grid().row(int(p - loc_.hist()));
            for (const Cell &c : r.cells) if (c.grapheme_id) { pflags_ |= ITCORE_SEARCH_PF_NEW_CONTENT; break; }
            continue;
        }
        auto it = rev_.find(id);
        if (it == rev_.end() || it->second == loc_.grid_rev(p) || redone.count(id)) continue;
        int64_t a, b;
        group_at(p, &a, &b);
        for (int64_t q = a; q <= b; ++q) redone.insert(loc_.id_at(q));
        used += scan_group(a, b, kill, fresh);
    }
    // 2) 继续扫描
    auto over = [&]() {
        if (budget_cells && used >= budget_cells) return true;
        if (budget_us) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            if (us >= int64_t(budget_us)) return true;
        }
        return false;
    };
    bool did_one = false;
    while (next_ < order_.size()) {
        if (did_one && over()) break;
        if (pflags_ & ITCORE_SEARCH_PF_TRUNCATED) break;
        const uint64_t id = order_[next_];
        done_cells_ += weight_[next_];
        ++next_;
        int64_t p;
        if (scanned_.count(id) || !loc_.find(id, &p) || !in_range(p)) continue;
        int64_t a, b;
        group_at(p, &a, &b);
        used += scan_group(a, b, kill, fresh);
        did_one = true;
    }
    if (pflags_ & ITCORE_SEARCH_PF_TRUNCATED) { next_ = order_.size(); done_cells_ = total_cells_; }
    // 3) 合并
    if (!fresh.empty() || std::find(kill.begin(), kill.end(), 1) != kill.end()) {
        const int64_t cur = cur_index();
        const bool cur_killed = cur >= 0 && kill[size_t(cur)];
        size_t w = 0;
        for (size_t i = 0; i < ms_.size(); ++i) if (!kill[i]) ms_[w++] = ms_[i];
        ms_.resize(w);
        std::sort(fresh.begin(), fresh.end(), less);
        size_t mid = ms_.size();
        ms_.insert(ms_.end(), fresh.begin(), fresh.end());
        std::inplace_merge(ms_.begin(), ms_.begin() + std::ptrdiff_t(mid), ms_.end(), less);
        if (cur_killed) has_cur_ = false;
    }
    // 4) 交付新结果（仍存活的，按文档顺序）
    uint32_t nout = 0;
    if (!pending_.empty()) {
        std::vector<std::pair<int64_t, std::pair<uint64_t, uint32_t>>> live;
        for (auto &pa : pending_) {
            int64_t p;
            if (!loc_.find(pa.first, &p)) continue;
            size_t i = lower({int64_t(loc_.ev()) + p, pa.second});
            if (i < ms_.size() && ms_[i].sseg == pa.first && ms_[i].soff == pa.second)
                live.push_back({int64_t(i), pa});
        }
        std::sort(live.begin(), live.end());
        live.erase(std::unique(live.begin(), live.end()), live.end());
        pending_.clear();
        for (auto &l : live) {
            if (nout < cap) fill(ms_[size_t(l.first)], &buf[nout++]);
            else pending_.push_back(l.second);
        }
    }
    if (n_out) *n_out = nout;
    const int st = done() ? ITCORE_SEARCH_DONE : ITCORE_SEARCH_RUNNING;
    prog->status = st;
    prog->progress = done() ? 1.0f : (total_cells_ ? float(double(done_cells_) / double(total_cells_)) : 1.0f);
    if (prog->progress > 1.0f) prog->progress = 1.0f;
    prog->total = done() ? int64_t(ms_.size()) : -1;
    prog->current = cur_index();
    prog->flags = pflags_;
    prog->dropped = dropped_;
    prog->changed = changed_;
    dropped_ = 0;
    changed_ = 0;
    return st;
}

bool Searcher::scanned_range(int64_t p0, int64_t p1) const {
    if (done()) return true;
    if (p0 < 0) p0 = 0;
    if (p1 >= loc_.count()) p1 = loc_.count() - 1;
    for (int64_t p = p0; p <= p1; ++p) {
        uint64_t id = loc_.id_at(p);
        if (in_range_id(id) && !scanned_.count(id)) return false;
    }
    return true;
}

void Searcher::fill(const M &m, itcore_search_match *out) const {
    std::vector<SearchLocator::CellPos> lay;
    const int64_t ev = int64_t(loc_.ev());
    *out = itcore_search_match{};
    out->start_seg = m.sseg; out->start_off = m.soff;
    out->end_seg = m.eseg; out->end_off = m.eoff;
    loc_.layout(m.sord - ev, lay);
    if (m.soff < lay.size()) { out->start_abs_row = lay[m.soff].row; out->start_col = lay[m.soff].col; }
    loc_.layout(m.eord - ev, lay);
    if (m.eoff < lay.size()) { out->end_abs_row = lay[m.eoff].row; out->end_col = uint16_t(lay[m.eoff].col + lay[m.eoff].w); }
}

int Searcher::move(bool fwd, itcore_search_match *out, int64_t *index) {
    if (dead()) return ITCORE_SEARCH_RESTART;
    refresh();
    const int64_t ev = int64_t(loc_.ev());
    const int64_t n = loc_.count();
    const int64_t rp = ref_ord_ - ev;
    const int run = done() ? ITCORE_SEARCH_DONE : ITCORE_SEARCH_RUNNING;
    int64_t idx;
    bool wrapped = false;
    if (fwd) {
        size_t i = lower({ref_ord_, ref_off_});
        if (!ref_incl_ && i < ms_.size() && ms_[i].sord == ref_ord_ && ms_[i].soff == ref_off_) ++i;
        if (i < ms_.size()) {
            if (!scanned_range(rp, ms_[i].sord - ev)) return ITCORE_SEARCH_PENDING;
            idx = int64_t(i);
        } else {
            if (!scanned_range(rp, n - 1)) return ITCORE_SEARCH_PENDING;
            if (!(flags_ & ITCORE_SEARCH_F_WRAP)) return ITCORE_SEARCH_NOT_FOUND;
            if (ms_.empty()) return scanned_range(0, rp) ? ITCORE_SEARCH_NOT_FOUND : ITCORE_SEARCH_PENDING;
            if (!scanned_range(0, ms_[0].sord - ev)) return ITCORE_SEARCH_PENDING;
            idx = 0;
            wrapped = true;
        }
    } else {
        int64_t i = int64_t(lower({ref_ord_, ref_off_})) - 1;
        if (i >= 0) {
            if (!scanned_range(ms_[size_t(i)].sord - ev, rp)) return ITCORE_SEARCH_PENDING;
            idx = i;
        } else {
            if (!scanned_range(0, rp)) return ITCORE_SEARCH_PENDING;
            if (!(flags_ & ITCORE_SEARCH_F_WRAP)) return ITCORE_SEARCH_NOT_FOUND;
            if (ms_.empty()) return scanned_range(rp, n - 1) ? ITCORE_SEARCH_NOT_FOUND : ITCORE_SEARCH_PENDING;
            if (!scanned_range(ms_.back().sord - ev, n - 1)) return ITCORE_SEARCH_PENDING;
            idx = int64_t(ms_.size()) - 1;
            wrapped = true;
        }
    }
    const M &m = ms_[size_t(idx)];
    ref_seg_ = m.sseg; ref_off_ = m.soff; ref_ord_ = m.sord;
    ref_incl_ = false;
    has_cur_ = true;
    if (wrapped) pflags_ |= ITCORE_SEARCH_PF_WRAPPED;
    if (out) fill(m, out);
    if (index) *index = idx;
    return run;
}

int Searcher::select(int64_t index) {
    if (dead()) return ITCORE_SEARCH_RESTART;
    refresh();
    if (index < 0 || index >= int64_t(ms_.size())) return -1;
    const M &m = ms_[size_t(index)];
    s_.sel_set(SelPos{m.sseg, m.soff}, SelPos{m.eseg, m.eoff});
    return 0;
}

int Searcher::ranges(int64_t first_row, uint32_t rows, itcore_search_range *out, uint32_t cap) {
    if (dead()) return ITCORE_SEARCH_RESTART;
    if (cap && !out) return ITCORE_SEARCH_EINVAL;
    refresh();
    if (!rows || ms_.empty()) return 0;
    const int64_t r0 = first_row, r1 = first_row + int64_t(rows) - 1;
    int64_t p0, p1;
    if (!loc_.rows_to_pos(r0, r1, &p0, &p1)) return 0;
    for (uint32_t k = 0; p0 > 0 && !loc_.hard_at(p0 - 1) && k < max_cells_; ++k) --p0;  // 从上一行续过来的命中
    const int64_t ev = int64_t(loc_.ev());
    const int64_t cur = cur_index();
    int count = 0;
    std::vector<SearchLocator::CellPos> lay;
    for (size_t i = lower({ev + p0, 0}); i < ms_.size() && ms_[i].sord <= ev + p1; ++i) {
        const M &m = ms_[i];
        bool open = false;
        itcore_search_range r{};
        auto flush = [&]() {
            if (open && r.row >= r0 && r.row <= r1) {
                if (uint32_t(count) < cap) out[count] = r;
                ++count;
            }
            open = false;
        };
        for (int64_t p = m.sord - ev; p <= m.eord - ev; ++p) {
            loc_.layout(p, lay);
            size_t a = (p == m.sord - ev) ? m.soff : 0;
            size_t b = (p == m.eord - ev) ? size_t(m.eoff) + 1 : lay.size();
            for (size_t c = a; c < b && c < lay.size(); ++c) {
                const auto &cp = lay[c];
                if (open && cp.row == r.row && cp.col == r.end_col) { r.end_col = uint16_t(cp.col + cp.w); continue; }
                flush();
                r = itcore_search_range{};
                r.row = cp.row; r.start_col = cp.col; r.end_col = uint16_t(cp.col + cp.w);
                r.is_current = int64_t(i) == cur ? 1 : 0;
                open = true;
            }
        }
        flush();
    }
    return count;
}

int Searcher::to_view(const Screen &s, const itcore_search_match *m, int32_t *sr, uint16_t *sc, int32_t *er,
                      uint16_t *ec) {
    if (!m) return -1;
    SearchLocator L(s);
    L.sync();
    int64_t ps, pe;
    if (!L.find(m->start_seg, &ps) || !L.find(m->end_seg, &pe)) return -1;
    std::vector<SearchLocator::CellPos> a, b;
    L.layout(ps, a);
    L.layout(pe, b);
    if (m->start_off >= a.size() || m->end_off >= b.size()) return -1;
    const int64_t base = int64_t(L.ev()) + L.hist_rows();
    if (sr) *sr = int32_t(a[m->start_off].row - base);
    if (sc) *sc = a[m->start_off].col;
    if (er) *er = int32_t(b[m->end_off].row - base);
    if (ec) *ec = uint16_t(b[m->end_off].col + b[m->end_off].w);
    return 0;
}

}  // namespace itcore
