// SPDX-License-Identifier: GPL-2.0-only
// 见 screen.h 来源声明。
#include "screen.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace itcore {
namespace {

constexpr uint32_t kColorDefault = 0;           // TT=0
constexpr uint32_t kColorIndexed(uint8_t i) { return 0x01000000u | i; }
constexpr uint32_t kColorRgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0x02000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

int param_or(const Token &t, size_t i, int def) {
    if (i >= t.params.size() || t.params[i].empty() || !t.params[i][0]) return def;
    return *t.params[i][0];
}
void set_param_if_default(Token &t, size_t i, int def) {
    while (t.params.size() <= i) t.params.push_back({std::nullopt});
    if (t.params[i].empty()) t.params[i].push_back(def);
    else if (!t.params[i][0]) t.params[i][0] = def;
}

bool is_combining(uint32_t cp) {
    // 常用组合区 + VS + ZWJ/ZWNJ 等零宽
    if (cp == 0x200C || cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F) return true;
    if (cp >= 0x0300 && cp <= 0x036F) return true;
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;
    if (cp >= 0x20D0 && cp <= 0x20FF) return true;
    if (cp >= 0xFE20 && cp <= 0xFE2F) return true;
    return false;
}

bool is_wide(uint32_t cp) {
    // 精简东亚宽：Hangul、CJK、全角、部分 emoji 区
    if (cp >= 0x1100 && cp <= 0x115F) return true;
    if (cp >= 0x2329 && cp <= 0x232A) return true;
    if (cp >= 0x2E80 && cp <= 0xA4CF) return true;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    if (cp >= 0xFE10 && cp <= 0xFE19) return true;
    if (cp >= 0xFE30 && cp <= 0xFE6F) return true;
    if (cp >= 0xFF00 && cp <= 0xFF60) return true;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return true;
    if (cp >= 0x1F300 && cp <= 0x1F64F) return true;
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;
    if (cp >= 0x20000 && cp <= 0x3FFFD) return true;
    return false;
}

}  // namespace

int cell_width(uint32_t cp) {
    if (cp == 0 || is_combining(cp)) return 0;
    if (is_wide(cp)) return 2;
    return 1;
}

bool utf8_next(const uint8_t *p, size_t n, uint32_t *cp, size_t *adv) {
    if (!n) { *cp = 0xFFFD; *adv = 0; return false; }
    uint8_t b = p[0];
    if (b < 0x80) { *cp = b; *adv = 1; return true; }
    size_t need = (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
    if (!need || n < need) { *cp = 0xFFFD; *adv = 1; return false; }
    uint32_t c = b & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; *adv = 1; return false; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *cp = c; *adv = need; return true;
}

// ---- GraphemePool ----
uint32_t GraphemePool::intern(std::string_view utf8) {
    if (utf8.empty()) return 0;
    std::string key(utf8);
    auto it = to_id_.find(key);
    if (it != to_id_.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(by_id_.size());
    bytes_ += key.size() + 8;
    by_id_.push_back(std::move(key));
    to_id_[by_id_.back()] = id;
    return id;
}
const char *GraphemePool::get(uint32_t id, size_t *len) const {
    if (id >= by_id_.size()) { if (len) *len = 0; return ""; }
    if (len) *len = by_id_[id].size();
    return by_id_[id].c_str();
}
void GraphemePool::clear() { by_id_.assign(1, ""); to_id_.clear(); bytes_ = 0; }

// ---- Cell / Row ----
itcore_cell Cell::to_abi() const {
    return itcore_cell{grapheme_id, width, 0, flags, fg, bg, underline_color, hyperlink_id};
}
Cell Cell::blank() { return Cell{}; }

Cell Pen::make_cell(uint32_t gid, uint8_t w) const {
    Cell c;
    c.grapheme_id = gid; c.width = w; c.flags = flags;
    c.fg = fg; c.bg = bg; c.underline_color = underline_color;
    return c;
}

void Row::resize_cols(uint16_t cols) {
    if (cells.size() < cols) cells.resize(cols, Cell::blank());
    else if (cells.size() > cols) cells.resize(cols);
}

// ---- Grid ----
void Grid::reset(uint16_t cols, uint16_t rows) {
    cols_ = cols; rows_ = rows;
    rows_data_.assign(rows, Row{});
    for (auto &r : rows_data_) {
        r.cells.assign(cols, Cell::blank());
        r.id = next_row_id_++;
        r.revision = 1;
        r.wrap = ITCORE_WRAP_HARD;
    }
    cursor_ = {0, 0};
    scroll_top_ = 0; scroll_bottom_ = rows ? rows - 1 : 0;
    wrap_pending_ = false; origin_ = false;
}
void Grid::resize(uint16_t cols, uint16_t rows) {
    if (cols == cols_ && rows == rows_) return;
    std::vector<Row> old = std::move(rows_data_);
    uint16_t old_cols = cols_, old_rows = rows_;
    Cursor old_c = cursor_;
    reset(cols, rows);
    int copy_rows = std::min<int>(old_rows, rows);
    int copy_cols = std::min<int>(old_cols, cols);
    for (int y = 0; y < copy_rows; ++y) {
        for (int x = 0; x < copy_cols; ++x) rows_data_[y].cells[x] = old[y].cells[x];
        rows_data_[y].wrap = old[y].wrap;
        rows_data_[y].id = old[y].id;
        rows_data_[y].revision = old[y].revision;
    }
    cursor_.x = std::min(old_c.x, (int)cols);
    cursor_.y = std::min(old_c.y, (int)rows - 1);
    scroll_top_ = 0; scroll_bottom_ = rows - 1;
}
void Grid::set_scroll(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= rows_) bottom = rows_ - 1;
    if (top > bottom) { top = 0; bottom = rows_ - 1; }
    scroll_top_ = top; scroll_bottom_ = bottom;
}
void Grid::scroll_up(int n, EvictFn evict, void *ctx) {
    if (n <= 0) return;
    int top = scroll_top_, bot = scroll_bottom_;
    int h = bot - top + 1;
    if (n > h) n = h;
    for (int i = 0; i < n; ++i) {
        Row gone = std::move(rows_data_[top]);
        if (evict) evict(std::move(gone), ctx);
        for (int y = top; y < bot; ++y) rows_data_[y] = std::move(rows_data_[y + 1]);
        rows_data_[bot] = Row{};
        rows_data_[bot].cells.assign(cols_, Cell::blank());
        rows_data_[bot].id = next_row_id_++;
        rows_data_[bot].revision = 1;
        rows_data_[bot].wrap = ITCORE_WRAP_HARD;
    }
}
void Grid::scroll_down(int n) {
    if (n <= 0) return;
    int top = scroll_top_, bot = scroll_bottom_;
    int h = bot - top + 1;
    if (n > h) n = h;
    for (int i = 0; i < n; ++i) {
        for (int y = bot; y > top; --y) rows_data_[y] = std::move(rows_data_[y - 1]);
        rows_data_[top] = Row{};
        rows_data_[top].cells.assign(cols_, Cell::blank());
        rows_data_[top].id = next_row_id_++;
        rows_data_[top].revision = 1;
    }
}
void Grid::clear_all(const Cell &blank) {
    for (auto &r : rows_data_) {
        std::fill(r.cells.begin(), r.cells.end(), blank);
        r.wrap = ITCORE_WRAP_HARD;
        ++r.revision;
    }
    wrap_pending_ = false;
}
void Grid::erase_in_display(int mode, const Cell &blank) {
    auto erase_row = [&](int y, int x0, int x1) {
        for (int x = x0; x < x1; ++x) rows_data_[y].cells[x] = blank;
        ++rows_data_[y].revision;
    };
    if (mode == 0) {
        erase_row(cursor_.y, cursor_.x, cols_);
        for (int y = cursor_.y + 1; y < rows_; ++y) erase_row(y, 0, cols_);
    } else if (mode == 1) {
        for (int y = 0; y < cursor_.y; ++y) erase_row(y, 0, cols_);
        erase_row(cursor_.y, 0, cursor_.x + 1);
    } else {
        clear_all(blank);
    }
}
void Grid::erase_in_line(int mode, const Cell &blank) {
    int x0 = 0, x1 = cols_;
    if (mode == 0) x0 = cursor_.x;
    else if (mode == 1) x1 = cursor_.x + 1;
    for (int x = x0; x < x1; ++x) rows_data_[cursor_.y].cells[x] = blank;
    ++rows_data_[cursor_.y].revision;
}
void Grid::insert_lines(int n, const Cell &blank) {
    (void)blank;
    int y = cursor_.y;
    if (y < scroll_top_ || y > scroll_bottom_) return;
    int bot = scroll_bottom_;
    if (n > bot - y + 1) n = bot - y + 1;
    for (int i = 0; i < n; ++i) {
        for (int r = bot; r > y; --r) rows_data_[r] = std::move(rows_data_[r - 1]);
        rows_data_[y] = Row{};
        rows_data_[y].cells.assign(cols_, Cell::blank());
        rows_data_[y].id = next_row_id_++;
        rows_data_[y].revision = 1;
    }
}
void Grid::delete_lines(int n, const Cell &blank) {
    (void)blank;
    int y = cursor_.y;
    if (y < scroll_top_ || y > scroll_bottom_) return;
    int bot = scroll_bottom_;
    if (n > bot - y + 1) n = bot - y + 1;
    for (int i = 0; i < n; ++i) {
        for (int r = y; r < bot; ++r) rows_data_[r] = std::move(rows_data_[r + 1]);
        rows_data_[bot] = Row{};
        rows_data_[bot].cells.assign(cols_, Cell::blank());
        rows_data_[bot].id = next_row_id_++;
        rows_data_[bot].revision = 1;
    }
}
void Grid::insert_chars(int n, const Cell &blank) {
    auto &cells = rows_data_[cursor_.y].cells;
    if (n > cols_ - cursor_.x) n = cols_ - cursor_.x;
    for (int i = cols_ - 1; i >= cursor_.x + n; --i) cells[i] = cells[i - n];
    for (int i = 0; i < n; ++i) cells[cursor_.x + i] = blank;
    ++rows_data_[cursor_.y].revision;
}
void Grid::delete_chars(int n, const Cell &blank) {
    auto &cells = rows_data_[cursor_.y].cells;
    if (n > cols_ - cursor_.x) n = cols_ - cursor_.x;
    for (int i = cursor_.x; i + n < cols_; ++i) cells[i] = cells[i + n];
    for (int i = cols_ - n; i < cols_; ++i) cells[i] = blank;
    ++rows_data_[cursor_.y].revision;
}
void Grid::clamp_cursor(bool allow_pending) {
    int max_x = allow_pending ? cols_ : cols_ - 1;
    if (cursor_.x < 0) cursor_.x = 0;
    if (cursor_.x > max_x) cursor_.x = max_x;
    int top = origin_top(), bot = origin_bottom();
    if (cursor_.y < top) cursor_.y = top;
    if (cursor_.y > bot) cursor_.y = bot;
}
size_t Grid::mem_bytes() const {
    size_t n = sizeof(*this);
    for (auto &r : rows_data_) n += r.mem_bytes();
    return n;
}

// ---- Scrollback ----
void Scrollback::push(LogicalLine &&line) {
    bytes_ += line.mem_bytes();
    lines_.push_back(std::move(line));
    trim();
}
void Scrollback::trim() {
    while (!lines_.empty() &&
           (lines_.size() > max_lines_ || bytes_ > max_bytes_)) {
        bytes_ -= lines_.front().mem_bytes();
        lines_.pop_front();
    }
}
void Scrollback::clear() { lines_.clear(); bytes_ = 0; }

static void reflow_line(const LogicalLine &ll, uint16_t cols, std::vector<Row> &out, bool last_hard) {
    (void)last_hard;
    if (cols == 0) return;
    size_t i = 0;
    while (i < ll.cells.size() || (i == 0 && ll.cells.empty())) {
        Row r;
        r.cells.assign(cols, Cell::blank());
        r.id = ll.id;
        r.revision = 1;
        size_t col = 0;
        while (i < ll.cells.size() && col < cols) {
            const Cell &c = ll.cells[i];
            uint8_t w = c.width ? c.width : 1;
            if (c.flags & ITCORE_CELL_CONTINUATION) { ++i; continue; }
            if (col + w > cols) break;
            r.cells[col] = c;
            r.cells[col].flags = static_cast<uint16_t>(c.flags & ~ITCORE_CELL_CONTINUATION);
            for (uint8_t k = 1; k < w && col + k < cols; ++k) {
                r.cells[col + k] = Cell::blank();
                r.cells[col + k].flags = ITCORE_CELL_CONTINUATION;
                r.cells[col + k].width = 0;
            }
            col += w;
            ++i;
            // skip continuation sources already consumed above via flag skip
        }
        bool more = i < ll.cells.size();
        if (more) r.wrap = ITCORE_WRAP_SOFT;
        else r.wrap = ll.hard ? ITCORE_WRAP_HARD : ITCORE_WRAP_SOFT;
        out.push_back(std::move(r));
        if (!more) break;
        if (ll.cells.empty()) break;
    }
}

void Scrollback::reflow_into(uint16_t cols, std::vector<Row> &out) const {
    out.clear();
    for (auto &ll : lines_) reflow_line(ll, cols, out, ll.hard);
}
size_t Scrollback::display_rows(uint16_t cols) const {
    std::vector<Row> tmp;
    reflow_into(cols, tmp);
    return tmp.size();
}
bool Scrollback::map_display_row(uint16_t cols, size_t display_row, size_t *logic_i, size_t *cell_off) const {
    size_t shown = 0;
    for (size_t li = 0; li < lines_.size(); ++li) {
        std::vector<Row> part;
        reflow_line(lines_[li], cols, part, lines_[li].hard);
        if (display_row < shown + part.size()) {
            size_t local = display_row - shown;
            *logic_i = li;
            *cell_off = 0;
            // 估算行内偏移：前面 display 行占满的单元格数
            size_t off = 0;
            for (size_t k = 0; k < local; ++k) {
                for (auto &c : part[k].cells)
                    if (!(c.flags & ITCORE_CELL_CONTINUATION) && c.grapheme_id) off++;
                // 更稳：按宽累计
            }
            // 精确：重算
            off = 0;
            size_t i = 0, col = 0; size_t target_row = local; size_t cur_row = 0;
            while (i < lines_[li].cells.size()) {
                if (cur_row == target_row && col == 0) { *cell_off = i; return true; }
                auto &c = lines_[li].cells[i];
                if (c.flags & ITCORE_CELL_CONTINUATION) { ++i; continue; }
                uint8_t w = c.width ? c.width : 1;
                if (col + w > cols) { ++cur_row; col = 0; continue; }
                col += w; ++i;
            }
            *cell_off = 0;
            return true;
        }
        shown += part.size();
    }
    return false;
}

// ---- CSI defaults ----
std::string apply_csi_defaults(Token &t) {
    if (t.type != TokenType::Csi) return "";
    // 仅无中间字节或已知中间时匹配；私有前缀分支
    char f = t.final;
    char priv = t.priv;
    std::string inter = t.inter;
    auto no_inter = inter.empty();

    if (priv == '?' && f == 'n' && no_inter) {
        set_param_if_default(t, 0, 0);
        return "DECDSR";
    }
    if (priv == '?' && (f == 'h' || f == 'l') && no_inter) return f == 'h' ? "DECSET" : "DECRST";
    if (priv == '>' && f == 'c' && no_inter) { set_param_if_default(t, 0, 0); return "DA2"; }
    if (priv != 0) return "";  // 其它私有：消费但不填默认

    if (!no_inter) {
        if (inter == " " && f == 'q') { set_param_if_default(t, 0, 0); return "DECSCUSR"; }
        return "";
    }

    switch (f) {
    case 'A': set_param_if_default(t, 0, 1); return "CUU";
    case 'B': set_param_if_default(t, 0, 1); return "CUD";
    case 'C': set_param_if_default(t, 0, 1); return "CUF";
    case 'D': set_param_if_default(t, 0, 1); return "CUB";
    case 'E': set_param_if_default(t, 0, 1); return "CNL";
    case 'F': set_param_if_default(t, 0, 1); return "CPL";
    case 'G': set_param_if_default(t, 0, 1); return "CHA";
    case 'H': set_param_if_default(t, 0, 1); set_param_if_default(t, 1, 1); return "CUP";
    case 'f': set_param_if_default(t, 0, 1); set_param_if_default(t, 1, 1); return "HVP";
    case 'I': set_param_if_default(t, 0, 1); return "CHT";
    case 'J': set_param_if_default(t, 0, 0); return "ED";
    case 'K': set_param_if_default(t, 0, 0); return "EL";
    case 'L': set_param_if_default(t, 0, 1); return "IL";
    case 'M': set_param_if_default(t, 0, 1); return "DL";
    case '@': set_param_if_default(t, 0, 1); return "ICH";
    case 'P': set_param_if_default(t, 0, 1); return "DCH";
    case 'S': set_param_if_default(t, 0, 1); return "SU";
    case 'T': set_param_if_default(t, 0, 1); return "SD";
    case 'X': set_param_if_default(t, 0, 1); return "ECH";
    case 'd': set_param_if_default(t, 0, 1); return "VPA";
    case 'e': set_param_if_default(t, 0, 1); return "VPR";
    case 'a': set_param_if_default(t, 0, 1); return "HPR";
    case 'b': set_param_if_default(t, 0, 1); return "REP";
    case 'c': set_param_if_default(t, 0, 0); return "DA";
    case 'n': set_param_if_default(t, 0, 0); return "DSR";
    case 'r': return "DECSTBM";  // 默认在应用时填满屏
    case 'm':
        if (t.params.empty()) t.params.push_back({0});
        for (size_t i = 0; i < t.params.size(); ++i) set_param_if_default(t, i, 0);
        return "SGR";
    case 'g': set_param_if_default(t, 0, 0); return "TBC";
    case 'h': return "SM";
    case 'l': return "RM";
    case 's': return "SCOSC";
    case 'u': return "SCORC";
    default: return "";
    }
}

// ---- Screen ----
Screen::Screen(uint16_t cols, uint16_t rows, uint32_t sb_lines, uint64_t mem_limit)
    : scrollback_(sb_lines ? sb_lines : 10000, mem_limit ? mem_limit : (64ull << 20)) {
    if (!cols) cols = 80;
    if (!rows) rows = 24;
    main_.reset(cols, rows);
    alt_.reset(cols, rows);
}

void Screen::evict_thunk(Row &&r, void *ctx) {
    static_cast<Screen *>(ctx)->evict_to_scrollback(std::move(r));
}

void Screen::evict_to_scrollback(Row &&r) {
    if (modes_.alt) return;  // 备用屏不进历史
    // 将显示行并入逻辑行：软换行拼到上一逻辑行，硬换行结束。
    if (!scrollback_.empty() && !scrollback_.at(scrollback_.logical_count() - 1).hard) {
        // 不可变 at；改为重新压：简化策略——每显示行独立逻辑行，软标记保留在 wrap 上。
    }
    LogicalLine ll;
    ll.id = r.id ? r.id : next_logic_id_++;
    ll.hard = (r.wrap == ITCORE_WRAP_HARD);
    for (auto &c : r.cells) {
        if (c.flags & ITCORE_CELL_CONTINUATION) continue;
        if (c.grapheme_id == 0 && c.flags == 0) {
            // 保留尾部空白压缩：跳过纯空，但中间空格保留——简化：全部推入非空及中间
        }
        ll.cells.push_back(c);
    }
    // 去掉尾部空白
    while (!ll.cells.empty() && ll.cells.back().grapheme_id == 0 && ll.cells.back().flags == 0)
        ll.cells.pop_back();
    // 合并软换行到前一行
    if (!ll.hard && !scrollback_.empty()) {
        // 无法直接改 deque 尾；用临时：pop 逻辑不可用。改为始终 push，硬=false 表示与下一行同逻辑。
        // 重排时连续 soft 合并：见 reflow — 当前 reflow 按单 LogicalLine。
        // 修正：软换行时合并进新结构——把 soft 行 cells 追加到“开放”逻辑行。
    }
    // 简化且可测：用开放缓冲
    // 这里改为：硬换行 push；软换行累积到 pending_（成员）。为少改头文件，用 static 不行。
    // 重新策略：每行都 push 为独立 LogicalLine，soft 的 hard=false；reflow_into 把连续 soft+最终 hard 合并。
    scrollback_.push(std::move(ll));
}

// 改进 reflow：合并连续 soft 逻辑行
static void reflow_merged(const Scrollback &sb, uint16_t cols, std::vector<Row> &out) {
    out.clear();
    LogicalLine acc;
    for (size_t i = 0; i < sb.logical_count(); ++i) {
        auto &ll = sb.at(i);
        if (acc.cells.empty() && acc.id == 0) acc.id = ll.id;
        for (auto &c : ll.cells) acc.cells.push_back(c);
        if (ll.hard) {
            acc.hard = true;
            reflow_line(acc, cols, out, true);
            acc = LogicalLine{};
        } else {
            acc.hard = false;
        }
    }
    if (!acc.cells.empty() || acc.id) {
        acc.hard = false;
        reflow_line(acc, cols, out, false);
    }
}

void Screen::resize(uint16_t cols, uint16_t rows) {
    if (!cols || !rows) return;
    // 主屏：把当前网格软/硬行卸到 scrollback 再重排回填（简化：仅改网格尺寸，scrollback 重排用于快照）
    main_.resize(cols, rows);
    alt_.resize(cols, rows);
    bump();
}

void Screen::write_utf8_text(const std::string &text) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(text.data());
    size_t n = text.size(), i = 0;
    while (i < n) {
        uint32_t cp; size_t adv;
        utf8_next(p + i, n - i, &cp, &adv);
        write_char(cp, p + i, adv);
        i += adv ? adv : 1;
    }
}

void Screen::write_char(uint32_t cp, const uint8_t *utf8, size_t utf8_len) {
    auto &g = grid();
    if (is_combining(cp) && has_last_) {
        // 合并进上一字素：新建组合串
        size_t olen = 0;
        const char *og = pool_.get(last_printed_.grapheme_id, &olen);
        std::string merged(og, olen);
        merged.append(reinterpret_cast<const char *>(utf8), utf8_len);
        uint32_t gid = pool_.intern(merged);
        // 写回光标前一格
        int x = g.cursor().x, y = g.cursor().y;
        if (g.wrap_pending()) { /* 仍在行末 */ }
        else if (x > 0) {
            int tx = x - 1;
            while (tx > 0 && (g.row(y).cells[tx].flags & ITCORE_CELL_CONTINUATION)) --tx;
            g.row(y).cells[tx].grapheme_id = gid;
            ++g.row(y).revision;
            last_printed_.grapheme_id = gid;
            bump();
        }
        return;
    }
    int w = cell_width(cp);
    if (w <= 0) w = 1;
    if (cp == 0xFFFD) w = 1;

    if (g.wrap_pending() && modes_.wrap) {
        g.row(g.cursor().y).wrap = ITCORE_WRAP_SOFT;
        g.set_wrap_pending(false);
        linefeed(false);
        g.cursor().x = 0;
    }

    Cell cell = pen_.make_cell(pool_.intern(std::string_view(reinterpret_cast<const char *>(utf8), utf8_len)),
                               static_cast<uint8_t>(w));
    if (modes_.insert) g.insert_chars(w, Cell::blank());

    auto place = [&](int x, int y) {
        if (x + w > g.cols()) return false;
        g.row(y).cells[x] = cell;
        for (int k = 1; k < w; ++k) {
            Cell cont = Cell::blank();
            cont.flags = ITCORE_CELL_CONTINUATION;
            cont.width = 0;
            g.row(y).cells[x + k] = cont;
        }
        ++g.row(y).revision;
        return true;
    };

    if (g.cursor().x >= g.cols()) {
        if (modes_.wrap) {
            g.row(g.cursor().y).wrap = ITCORE_WRAP_SOFT;
            linefeed(false);
            g.cursor().x = 0;
        } else {
            g.cursor().x = g.cols() - (w > 1 ? w : 1);
            if (g.cursor().x < 0) g.cursor().x = 0;
        }
    }
    // 宽字符贴右边界：若放不下则软换行（CORE-03）
    if (g.cursor().x + w > g.cols()) {
        if (modes_.wrap) {
            g.row(g.cursor().y).wrap = ITCORE_WRAP_WIDE;
            linefeed(false);
            g.cursor().x = 0;
        } else {
            return;  // 丢弃超出部分（DEC 行为近似）
        }
    }
    place(g.cursor().x, g.cursor().y);
    g.cursor().x += w;
    if (g.cursor().x >= g.cols()) {
        g.cursor().x = g.cols();
        g.set_wrap_pending(modes_.wrap);
    }
    last_printed_ = cell;
    has_last_ = true;
    bump();
}

void Screen::linefeed(bool hard) {
    auto &g = grid();
    if (hard) g.row(g.cursor().y).wrap = ITCORE_WRAP_HARD;
    if (g.cursor().y == g.scroll_bottom()) {
        g.scroll_up(1, &Screen::evict_thunk, this);
    } else if (g.cursor().y < g.rows() - 1) {
        ++g.cursor().y;
    }
    g.set_wrap_pending(false);
    bump();
}
void Screen::carriage_return() {
    grid().cursor().x = 0;
    grid().set_wrap_pending(false);
}
void Screen::backspace() {
    auto &g = grid();
    g.set_wrap_pending(false);
    if (g.cursor().x > 0) --g.cursor().x;
    while (g.cursor().x > 0 && (g.row(g.cursor().y).cells[g.cursor().x].flags & ITCORE_CELL_CONTINUATION))
        --g.cursor().x;
}
void Screen::tab() {
    auto &g = grid();
    int x = g.cursor().x;
    int next = ((x / 8) + 1) * 8;
    if (next >= g.cols()) next = g.cols() - 1;
    g.cursor().x = next;
}
void Screen::index() {
    auto &g = grid();
    if (g.cursor().y == g.scroll_bottom()) g.scroll_up(1, &Screen::evict_thunk, this);
    else if (g.cursor().y < g.rows() - 1) ++g.cursor().y;
    bump();
}
void Screen::reverse_index() {
    auto &g = grid();
    if (g.cursor().y == g.scroll_top()) g.scroll_down(1);
    else if (g.cursor().y > 0) --g.cursor().y;
    bump();
}

void Screen::do_cup(int row1, int col1) {
    auto &g = grid();
    int y = row1 - 1, x = col1 - 1;
    if (g.origin_mode()) y += g.scroll_top();
    g.cursor().x = x; g.cursor().y = y;
    g.set_wrap_pending(false);
    g.clamp_cursor(false);
}

void Screen::do_sgr(const Token &t) {
    auto apply_one = [&](int p, const std::vector<std::optional<int32_t>> &subs) {
        switch (p) {
        case 0: pen_ = Pen{}; break;
        case 1: pen_.flags |= ITCORE_CELL_BOLD; break;
        case 3: pen_.flags |= ITCORE_CELL_ITALIC; break;
        case 4: pen_.flags |= ITCORE_CELL_UNDERLINE; break;
        case 7: pen_.flags |= ITCORE_CELL_INVERSE; break;
        case 22: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_BOLD); break;
        case 23: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_ITALIC); break;
        case 24: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_UNDERLINE); break;
        case 27: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_INVERSE); break;
        case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
            pen_.fg = kColorIndexed(static_cast<uint8_t>(p - 30)); break;
        case 39: pen_.fg = kColorDefault; break;
        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
            pen_.bg = kColorIndexed(static_cast<uint8_t>(p - 40)); break;
        case 49: pen_.bg = kColorDefault; break;
        case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
            pen_.fg = kColorIndexed(static_cast<uint8_t>(p - 90 + 8)); break;
        case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
            pen_.bg = kColorIndexed(static_cast<uint8_t>(p - 100 + 8)); break;
        case 38: case 48: {
            uint32_t *dest = (p == 38) ? &pen_.fg : &pen_.bg;
            // 分号式：38;5;N 或 38;2;R;G;B —— 参数在后续 params；冒号子参数在 subs
            if (subs.size() >= 2 && subs[1]) {
                int mode = *subs[1];
                if (mode == 5 && subs.size() >= 3 && subs[2])
                    *dest = kColorIndexed(static_cast<uint8_t>(*subs[2] & 0xFF));
                else if (mode == 2 && subs.size() >= 5 && subs[2] && subs[3] && subs[4])
                    *dest = kColorRgb((uint8_t)*subs[2], (uint8_t)*subs[3], (uint8_t)*subs[4]);
            }
            break;
        }
        default: break;
        }
    };
    // 分号形式的 38;5;N：跨多个 params
    for (size_t i = 0; i < t.params.size(); ++i) {
        int p = t.params[i].empty() || !t.params[i][0] ? 0 : *t.params[i][0];
        if ((p == 38 || p == 48) && t.params[i].size() == 1) {
            // 看后续 ;5;N
            if (i + 2 < t.params.size()) {
                int mode = param_or(t, i + 1, -1);
                if (mode == 5) {
                    int idx = param_or(t, i + 2, 0);
                    *(p == 38 ? &pen_.fg : &pen_.bg) = kColorIndexed(static_cast<uint8_t>(idx & 0xFF));
                    i += 2; continue;
                }
                if (mode == 2 && i + 4 < t.params.size()) {
                    *(p == 38 ? &pen_.fg : &pen_.bg) = kColorRgb(
                        (uint8_t)param_or(t, i + 2, 0), (uint8_t)param_or(t, i + 3, 0),
                        (uint8_t)param_or(t, i + 4, 0));
                    i += 4; continue;
                }
            }
        }
        apply_one(p, t.params[i]);
    }
}

void Screen::set_mode(const Token &t, bool set) {
    for (size_t i = 0; i < t.params.size(); ++i) {
        int p = param_or(t, i, 0);
        if (t.priv == '?') {
            switch (p) {
            case 7: modes_.wrap = set; break;
            case 6: grid().set_origin(set); if (set) do_cup(1, 1); break;
            case 25: modes_.cursor_visible = set; break;
            case 47: if (set) enter_alt(false); else leave_alt(false); break;
            case 1047: if (set) enter_alt(true); else leave_alt(true); break;
            case 1049: if (set) enter_alt(true); else leave_alt(true); break;
            case 1048: // 仅保存/恢复光标
                if (set) {
                    auto &s = modes_.alt ? saved_alt_ : saved_main_;
                    s.cur = grid().cursor(); s.fg = pen_.fg; s.bg = pen_.bg; s.flags = pen_.flags; s.valid = true;
                } else {
                    auto &s = modes_.alt ? saved_alt_ : saved_main_;
                    if (s.valid) {
                        grid().cursor() = s.cur; pen_.fg = s.fg; pen_.bg = s.bg; pen_.flags = s.flags;
                        grid().clamp_cursor(false);
                    }
                }
                break;
            case 2004: modes_.bracketed_paste = set; break;
            default: break;
            }
        } else {
            if (p == 4) modes_.insert = set;
        }
    }
    bump();
}

void Screen::enter_alt(bool clear) {
    if (!modes_.alt) {
        saved_main_.cur = main_.cursor();
        saved_main_.fg = pen_.fg; saved_main_.bg = pen_.bg; saved_main_.flags = pen_.flags;
        saved_main_.valid = true;
        modes_.alt = true;
    }
    if (clear) alt_.clear_all(Cell::blank());
    bump();
}
void Screen::leave_alt(bool clear) {
    if (modes_.alt) {
        modes_.alt = false;
        if (clear) alt_.clear_all(Cell::blank());
        if (saved_main_.valid) {
            main_.cursor() = saved_main_.cur;
            pen_.fg = saved_main_.fg; pen_.bg = saved_main_.bg; pen_.flags = saved_main_.flags;
            main_.clamp_cursor(false);
        }
    }
    bump();
}

void Screen::apply(const Token &t, std::string *reply) {
    switch (t.type) {
    case TokenType::Text:
        write_utf8_text(t.text);
        break;
    case TokenType::C0:
        switch (t.c) {
        case 0x07: break;  // BEL：事件层处理
        case 0x08: backspace(); break;
        case 0x09: tab(); break;
        case 0x0A: case 0x0B: case 0x0C: linefeed(true); break;
        case 0x0D: carriage_return(); break;
        default: break;
        }
        break;
    case TokenType::Esc:
        if (t.inter.empty()) {
            if (t.final == '7') {  // DECSC
                auto &s = modes_.alt ? saved_alt_ : saved_main_;
                s.cur = grid().cursor(); s.fg = pen_.fg; s.bg = pen_.bg; s.flags = pen_.flags; s.valid = true;
            } else if (t.final == '8') {  // DECRC
                auto &s = modes_.alt ? saved_alt_ : saved_main_;
                if (s.valid) {
                    grid().cursor() = s.cur; pen_.fg = s.fg; pen_.bg = s.bg; pen_.flags = s.flags;
                    grid().clamp_cursor(false);
                }
            } else if (t.final == 'D') index();
            else if (t.final == 'M') reverse_index();
            else if (t.final == 'E') { carriage_return(); linefeed(true); }
        }
        break;
    case TokenType::Csi: {
        Token t2 = t;
        std::string cmd = apply_csi_defaults(t2);
        auto &g = grid();
        Cell blank = Cell::blank();
        if (cmd == "CUU") { g.cursor().y -= param_or(t2, 0, 1); g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CUD" || cmd == "VPR") { g.cursor().y += param_or(t2, 0, 1); g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CUF" || cmd == "HPR") { g.cursor().x += param_or(t2, 0, 1); g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CUB") { g.cursor().x -= param_or(t2, 0, 1); g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CNL") { g.cursor().y += param_or(t2, 0, 1); g.cursor().x = 0; g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CPL") { g.cursor().y -= param_or(t2, 0, 1); g.cursor().x = 0; g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "CHA") { g.cursor().x = param_or(t2, 0, 1) - 1; g.set_wrap_pending(false); g.clamp_cursor(false); }
        else if (cmd == "VPA") { do_cup(param_or(t2, 0, 1), g.cursor().x + 1); }
        else if (cmd == "CUP" || cmd == "HVP") do_cup(param_or(t2, 0, 1), param_or(t2, 1, 1));
        else if (cmd == "ED") {
            int m = param_or(t2, 0, 0);
            if (m == 3) scrollback_.clear();
            g.erase_in_display(m == 3 ? 2 : m, blank);
        }
        else if (cmd == "EL") g.erase_in_line(param_or(t2, 0, 0), blank);
        else if (cmd == "IL") g.insert_lines(param_or(t2, 0, 1), blank);
        else if (cmd == "DL") g.delete_lines(param_or(t2, 0, 1), blank);
        else if (cmd == "ICH") g.insert_chars(param_or(t2, 0, 1), blank);
        else if (cmd == "DCH") g.delete_chars(param_or(t2, 0, 1), blank);
        else if (cmd == "ECH") {
            int n = param_or(t2, 0, 1);
            for (int i = 0; i < n && g.cursor().x + i < g.cols(); ++i)
                g.row(g.cursor().y).cells[g.cursor().x + i] = blank;
            ++g.row(g.cursor().y).revision;
        }
        else if (cmd == "SU") g.scroll_up(param_or(t2, 0, 1), &Screen::evict_thunk, this);
        else if (cmd == "SD") g.scroll_down(param_or(t2, 0, 1));
        else if (cmd == "SGR") do_sgr(t2);
        else if (cmd == "DECSTBM") {
            int top = 1, bot = g.rows();
            if (!t2.params.empty()) top = param_or(t2, 0, 1);
            if (t2.params.size() >= 2) bot = param_or(t2, 1, g.rows());
            if (top == 0) top = 1;
            if (bot == 0) bot = g.rows();
            g.set_scroll(top - 1, bot - 1);
            do_cup(1, 1);
        }
        else if (cmd == "DECSET") set_mode(t2, true);
        else if (cmd == "DECRST") set_mode(t2, false);
        else if (cmd == "SM") set_mode(t2, true);
        else if (cmd == "RM") set_mode(t2, false);
        else if (cmd == "DSR") {
            int p = param_or(t2, 0, 0);
            if (p == 5 && reply) *reply += "\x1b[0n";
            else if (p == 6 && reply) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                              g.cursor().y + 1, std::min(g.cursor().x + 1, (int)g.cols()));
                *reply += buf;
            }
        }
        else if (cmd == "DA" && reply && param_or(t2, 0, 0) == 0)
            *reply += "\x1b[?1;2c";  // VT100+AVO 拟定身份（设计 14.3）；未冻结
        else if (cmd == "REP" && has_last_) {
            int n = param_or(t2, 0, 1);
            size_t len = 0; const char *s = pool_.get(last_printed_.grapheme_id, &len);
            for (int i = 0; i < n; ++i) write_utf8_text(std::string(s, len));
        }
        else if (cmd == "SCOSC") {
            auto &s = modes_.alt ? saved_alt_ : saved_main_;
            s.cur = g.cursor(); s.valid = true;
        } else if (cmd == "SCORC") {
            auto &s = modes_.alt ? saved_alt_ : saved_main_;
            if (s.valid) { g.cursor() = s.cur; g.clamp_cursor(false); }
        }
        bump();
        break;
    }
    default: break;
    }
}

Screen::SnapshotData Screen::take_snapshot(int32_t first_row, uint16_t row_count) const {
    SnapshotData sn;
    sn.revision = revision_;
    sn.cols = grid().cols();
    sn.rows = grid().rows();
    sn.cursor_col = std::min(grid().cursor().x, (int)sn.cols - 1);
    if (sn.cursor_col < 0) sn.cursor_col = 0;
    sn.cursor_row = grid().cursor().y;
    sn.cursor_visible = modes_.cursor_visible;
    sn.pool_copy = pool_;

    std::vector<Row> hist;
    if (!modes_.alt) reflow_merged(scrollback_, sn.cols, hist);

    // 视口行
    std::vector<Row> view;
    for (int y = 0; y < sn.rows; ++y) view.push_back(grid().row(y));

    // 绝对坐标：[-hist, 0..rows)
    // first_row 相对视口顶
    int64_t start = static_cast<int64_t>(first_row);
    int64_t hist_n = static_cast<int64_t>(hist.size());
    for (uint16_t i = 0; i < row_count; ++i) {
        int64_t abs = start + i;  // 相对视口
        Row r;
        r.cells.assign(sn.cols, Cell::blank());
        if (abs < 0) {
            int64_t hi = hist_n + abs;
            if (hi >= 0 && hi < hist_n) r = hist[static_cast<size_t>(hi)];
        } else if (abs < sn.rows) {
            r = view[static_cast<size_t>(abs)];
        }
        r.resize_cols(sn.cols);
        sn.rows_data.push_back(std::move(r));
    }
    return sn;
}

size_t Screen::mem_bytes() const {
    return pool_.bytes() + main_.mem_bytes() + alt_.mem_bytes() + scrollback_.bytes() + 256;
}

}  // namespace itcore
