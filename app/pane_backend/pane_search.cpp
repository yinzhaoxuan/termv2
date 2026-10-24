#include "pane_search.h"
#include <algorithm>
#include <climits>

namespace it2app {

void PaneSearch::end() {
    if (h_) itcore_search_end(h_);
    h_ = nullptr;
    core_ = nullptr;
    needle_.clear();
    status_ = ITCORE_SEARCH_DONE;
    total_ = current_ = -1;
    flags_ = 0;
    pendingDir_.reset();
    last_ = {};
}

int PaneSearch::step(uint32_t cells, uint32_t us) {
    itcore_search_progress p{};
    p.struct_size = sizeof(p);
    uint32_t n = 0;
    const int st = itcore_search_step(h_, cells, us, &p, nullptr, 0, &n);
    if (st == ITCORE_SEARCH_DONE || st == ITCORE_SEARCH_RUNNING) {
        status_ = st;
        total_ = p.total;
        current_ = p.current;
        flags_ |= p.flags & ITCORE_SEARCH_PF_NEW_CONTENT;  // 新输出一经出现即保持到本轮结束
    }
    return st;
}

PaneSearch::State PaneSearch::restartedState() {
    end();  // 失效句柄：释放；UI 收到 restarted 后用同一 needle 重新搜
    State s;
    s.restarted = true;
    last_ = s;
    return s;
}

void PaneSearch::applyMove(int st, const itcore_search_match &m, int64_t idx, State &s) {
    s.pending = false;
    s.hasView = false;
    if (st == ITCORE_SEARCH_DONE || st == ITCORE_SEARCH_RUNNING) {
        s.found = true;
        s.index = int(idx < 0 ? 0 : idx > INT_MAX ? INT_MAX : idx);
        current_ = idx;
        int32_t r = 0; uint16_t c = 0;
        if (itcore_search_match_to_view(core_, &m, &r, &c, nullptr, nullptr) == 0) { s.hasView = true; s.viewRow = r; s.viewCol = c; }
    } else if (st == ITCORE_SEARCH_PENDING) {
        s.found = false;
        s.pending = true;
    } else {  // NOT_FOUND
        s.found = false;
    }
}

PaneSearch::State PaneSearch::find(itcore_core *core, std::string_view needle, const Options &opt, bool backward) {
    if (!core || needle.empty() || opt.regex) { end(); return {}; }
    if (h_ && core != core_) end();
    const bool fresh = !h_ || needle != needle_ || !(opt == opt_);
    if (fresh) {
        end();
        itcore_search_options o{};
        o.struct_size = sizeof(o);
        o.case_mode = opt.caseMode;
        o.flags = (opt.wrap ? uint32_t(ITCORE_SEARCH_F_WRAP) : 0u) | (backward ? uint32_t(ITCORE_SEARCH_F_BACKWARD) : 0u);
        if (itcore_search_begin(core, needle.data(), needle.size(), &o, &h_) != 0 || !h_) { h_ = nullptr; return {}; }
        core_ = core;
        needle_.assign(needle);
        opt_ = opt;
        status_ = ITCORE_SEARCH_RUNNING;
    }
    const int st = step(kFindCells, kFindUs);
    if (st == ITCORE_SEARCH_RESTARTED || st == ITCORE_SEARCH_RESTART) return restartedState();
    itcore_search_match m{};
    int64_t idx = -1;
    const int mv = backward ? itcore_search_prev(h_, &m, &idx) : itcore_search_next(h_, &m, &idx);
    if (mv == ITCORE_SEARCH_RESTART || mv == ITCORE_SEARCH_RESTARTED) return restartedState();
    State s;
    applyMove(mv, m, idx, s);
    if (s.pending) pendingDir_ = backward; else pendingDir_.reset();
    s.total = int(total_ < 0 ? -1 : total_ > INT_MAX ? INT_MAX : total_);
    s.hasNewOutput = (flags_ & ITCORE_SEARCH_PF_NEW_CONTENT) != 0;
    last_ = s;
    return s;
}

std::optional<PaneSearch::State> PaneSearch::tick(uint32_t budgetCells, uint32_t budgetUs) {
    if (!h_) return std::nullopt;
    const int st = step(budgetCells, budgetUs);
    if (st == ITCORE_SEARCH_RESTARTED || st == ITCORE_SEARCH_RESTART) return restartedState();
    State s = last_;
    s.moved = false;
    if (pendingDir_) {
        itcore_search_match m{};
        int64_t idx = -1;
        const int mv = *pendingDir_ ? itcore_search_prev(h_, &m, &idx) : itcore_search_next(h_, &m, &idx);
        if (mv == ITCORE_SEARCH_RESTART) return restartedState();
        if (mv != ITCORE_SEARCH_PENDING) {
            pendingDir_.reset();
            applyMove(mv, m, idx, s);
            s.moved = s.found;
        }
    } else if (s.found) {
        if (current_ < 0) { s.found = false; s.hasView = false; s.index = 0; }   // 当前结果被淘汰 / 改写
        else s.index = int(current_ > INT_MAX ? INT_MAX : current_);              // 前面的结果增减后序号随之变
    }
    s.total = int(total_ < 0 ? -1 : total_ > INT_MAX ? INT_MAX : total_);
    s.hasNewOutput = (flags_ & ITCORE_SEARCH_PF_NEW_CONTENT) != 0;
    if (s == last_) return std::nullopt;
    last_ = s;
    return s;
}

std::vector<itcore_search_range> PaneSearch::ranges(int32_t firstViewRow, uint32_t rows) const {
    std::vector<itcore_search_range> out;
    if (!h_ || !core_ || !rows) return out;
    const int64_t first = int64_t(itcore_scrollback_overflow(core_)) + int64_t(itcore_history_rows(core_)) + firstViewRow;
    out.resize(64);
    int n = itcore_search_ranges_in_view(h_, first, rows, out.data(), uint32_t(out.size()));
    if (n > int(out.size())) {
        out.resize(size_t(n));
        n = itcore_search_ranges_in_view(h_, first, rows, out.data(), uint32_t(out.size()));
    }
    out.resize(n > 0 ? std::min<size_t>(size_t(n), out.size()) : 0);
    return out;
}

}  // namespace it2app
