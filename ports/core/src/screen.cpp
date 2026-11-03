// SPDX-License-Identifier: GPL-2.0-only
// 见 screen.h 来源声明。
#include "screen.h"
#include "ucd_width.h"
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

// 身份与版本见 docs/identity-14.3.md。仓库没有 project(VERSION)，沿用设计系列 0.1。
constexpr const char kTermName[] = "termv2";
constexpr const char kTermVersion[] = "0.1";

void set_underline_style(uint16_t &flags, int style) {
    flags = uint16_t(flags & ~(ITCORE_CELL_UNDERLINE | ITCORE_CELL_UL_STYLE_MASK));
    if (style >= ITCORE_UL_SINGLE && style <= ITCORE_UL_DASHED)
        flags = uint16_t(flags | ITCORE_CELL_UNDERLINE | (uint16_t(style) << ITCORE_CELL_UL_STYLE_SHIFT));
}

// 冒号形式。subs[0] 是 38/48/58 本身；其后的子参数含 null（解析器保留空字段）。
// 对齐 iTerm2 VT100TerminalColorValueFromCSI：子参数 ≥5 时下标 2 是色彩空间（可空），
// RGB 在 3..5；恰好 4 个则没有色彩空间，RGB 在 2..4。任一缺失则不改颜色。
bool color_from_colon(const std::vector<std::optional<int32_t>> &subs, uint32_t *out) {
    if (subs.size() < 2 || !subs[1]) return false;
    const int nsub = int(subs.size()) - 1;
    const int mode = *subs[1];
    auto at = [&](size_t idx) -> const std::optional<int32_t> * {
        return idx < subs.size() ? &subs[idx] : nullptr;
    };
    if (mode == 5) {
        const auto *v = at(2);
        if (!v || !*v) return false;
        *out = kColorIndexed(uint8_t(**v));
        return true;
    }
    if (mode != 2 || nsub < 4) return false;
    const size_t base = nsub >= 5 ? 3 : 2;
    const auto *r = at(base), *g = at(base + 1), *b = at(base + 2);
    if (!r || !*r || !g || !*g || !b || !*b) return false;
    *out = kColorRgb(uint8_t(**r), uint8_t(**g), uint8_t(**b));
    return true;
}

// 分号形式 38/48/58。返回需要再跳过的参数个数（for 循环还会 ++i）；0 表示不按颜色消费。
// 模式 2/5 即使分量不够也吃掉已有分量，避免 2/0 漏进后面的 SGR（GAP-14）。
// 未知模式不消费，与 iTerm2 一致。
int semicolon_color_span(const Token &t, size_t i, uint32_t *out, bool *wrote) {
    *wrote = false;
    if (i + 1 >= t.params.size()) return 0;
    const auto &mode_p = t.params[i + 1];
    if (mode_p.size() != 1 || !mode_p[0]) return 0;
    const int mode = *mode_p[0];
    auto at = [&](size_t j) -> const std::optional<int32_t> * {
        if (j >= t.params.size() || t.params[j].size() != 1) return nullptr;
        return &t.params[j][0];
    };
    if (mode == 5) {
        if (const auto *v = at(i + 2); v && *v) {
            *out = kColorIndexed(uint8_t(**v));
            *wrote = true;
            return 2;
        }
        return 1;
    }
    if (mode == 2) {
        int avail = 0;
        for (; avail < 3; ++avail)
            if (!at(i + 2 + size_t(avail))) break;
        if (avail >= 3) {
            const auto *r = at(i + 2), *g = at(i + 3), *b = at(i + 4);
            if (r && *r && g && *g && b && *b) {
                *out = kColorRgb(uint8_t(**r), uint8_t(**g), uint8_t(**b));
                *wrote = true;
            }
            return 4;
        }
        return 1 + avail;
    }
    return 0;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string hex_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) { o += kHex[c >> 4]; o += kHex[c & 0xF]; }
    return o;
}
bool hex_decode(std::string_view h, std::string *out) {
    if (h.empty() || (h.size() % 2) != 0) return false;
    out->clear();
    out->reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = hex_nibble(h[i]), b = hex_nibble(h[i + 1]);
        if (a < 0 || b < 0) return false;
        *out += char((a << 4) | b);
    }
    return true;
}

const char *xtcap_value(std::string_view name) {
    if (name == "TN" || name == "name") return kTermName;
    if (name == "Co" || name == "colors") return "256";
    if (name == "RGB") return "8";  // 真彩色每通道 8 位；不是开关位
    return nullptr;
}

void append_sgr_color(std::string *o, uint32_t c, int which) {
    const uint32_t tt = c >> 24;
    if (tt == 0) return;
    *o += ';';
    if (tt == 1) {
        const int idx = int(c & 0xFF);
        if (which == 2) {
            *o += "58:5:" + std::to_string(idx);
            return;
        }
        if (idx < 8) { *o += std::to_string((which ? 40 : 30) + idx); return; }
        if (idx < 16) { *o += std::to_string((which ? 100 : 90) + (idx - 8)); return; }
        *o += which ? "48:5:" : "38:5:";
        *o += std::to_string(idx);
        return;
    }
    if (tt == 2) {
        *o += which == 2 ? "58:2:" : which == 1 ? "48:2:" : "38:2:";
        *o += std::to_string((c >> 16) & 0xFF);
        *o += ':';
        *o += std::to_string((c >> 8) & 0xFF);
        *o += ':';
        *o += std::to_string(c & 0xFF);
    }
}

}  // namespace

int cell_width(uint32_t cp) { return ucd::width(cp); }

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

// ---- PoolStore / PoolRef / GraphemePool ----
PoolStore::PoolStore() {
    for (auto &c : chunks_) c.store(nullptr, std::memory_order_relaxed);
}
PoolStore::~PoolStore() {
    for (auto &c : chunks_) delete[] c.load(std::memory_order_relaxed);
}
const std::string *PoolStore::at(uint32_t id) const {
    std::string *c = chunks_[id >> kChunkBits].load(std::memory_order_acquire);
    return c ? &c[id & (kChunk - 1)] : nullptr;
}
uint32_t PoolStore::append(std::string &&s) {
    uint32_t id = size_.load(std::memory_order_relaxed);
    uint32_t ci = id >> kChunkBits;
    if (ci >= kMaxChunks) return 0;
    std::string *c = chunks_[ci].load(std::memory_order_relaxed);
    if (!c) {
        c = new std::string[kChunk];
        chunks_[ci].store(c, std::memory_order_release);
    }
    c[id & (kChunk - 1)] = std::move(s);
    size_.store(id + 1, std::memory_order_release);  // 发布：此后读端可见且不再修改
    return id;
}
const char *PoolRef::get(uint32_t id, size_t *len) const {
    const std::string *e = (store && id < limit) ? store->at(id) : nullptr;
    if (!e) { if (len) *len = 0; return ""; }
    if (len) *len = e->size();
    return e->c_str();
}

GraphemePool::GraphemePool() : store_(std::make_shared<PoolStore>()) { store_->append(std::string()); }
uint32_t GraphemePool::intern(std::string_view utf8) {
    if (utf8.empty()) return 0;
    std::string key(utf8);
    auto it = to_id_.find(key);
    if (it != to_id_.end()) return it->second;
    // 池满：最后一个槽位预留给 U+FFFD，之后所有新字素/链接都返回它（不再退化为 0/空格）。
    if (fffd_id_ || store_->size() + 1 >= cap_) {
        if (!fffd_id_) {
            static const std::string kFffd("\xEF\xBF\xBD");
            auto f = to_id_.find(kFffd);
            if (f != to_id_.end()) fffd_id_ = f->second;
            else {
                fffd_id_ = store_->append(std::string(kFffd));
                bytes_ += entry_bytes(kFffd.size());
                to_id_.emplace(kFffd, fffd_id_);
            }
        }
        return fffd_id_;
    }
    uint32_t id = store_->append(std::string(key));
    if (!id) return 0;
    bytes_ += entry_bytes(key.size());
    to_id_.emplace(std::move(key), id);
    return id;
}
const char *GraphemePool::get(uint32_t id, size_t *len) const {
    return PoolRef{store_, store_->size(), epoch_}.get(id, len);
}
void GraphemePool::clear() {
    store_ = std::make_shared<PoolStore>();
    store_->append(std::string());
    to_id_.clear(); bytes_ = 0; ++epoch_; fffd_id_ = 0;
}
std::unordered_map<uint32_t, uint32_t> GraphemePool::compact(const std::vector<uint32_t> &live) {
    std::unordered_map<uint32_t, uint32_t> remap;
    auto old = store_;
    uint32_t lim = old->size();
    auto ns = std::make_shared<PoolStore>();
    ns->append(std::string());
    std::unordered_map<std::string, uint32_t> ids;
    size_t bytes = 0;
    remap[0] = 0;
    for (uint32_t id : live) {
        if (!id || id >= lim || remap.count(id)) continue;
        const std::string *s = old->at(id);
        if (!s) continue;
        uint32_t nid = ns->append(std::string(*s));
        remap[id] = nid;
        ids.emplace(*s, nid);
        bytes += entry_bytes(s->size());
    }
    store_ = std::move(ns);
    to_id_ = std::move(ids);
    bytes_ = bytes;
    fffd_id_ = 0;
    ++epoch_;
    return remap;
}
uint32_t GraphemePool::intern_hyperlink(std::string_view url, std::string_view id) {
    if (url.empty()) return 0;
    std::string k("\0H", 2);
    k.append(id); k.push_back('\0'); k.append(url);
    return intern(k);
}
bool GraphemePool::decode_hyperlink(const char *s, size_t n, std::string_view *url, std::string_view *id) {
    if (n < 3 || s[0] != '\0' || s[1] != 'H') return false;
    std::string_view v(s + 2, n - 2);
    size_t z = v.find('\0');
    if (z == std::string_view::npos) return false;
    if (id) *id = v.substr(0, z);
    if (url) *url = v.substr(z + 1);
    return true;
}

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
        r.revision = next_rev();
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
        rows_data_[bot].revision = next_rev();
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
        rows_data_[top].revision = next_rev();
    }
}
void Grid::clear_all(const Cell &blank) {
    for (auto &r : rows_data_) {
        std::fill(r.cells.begin(), r.cells.end(), blank);
        r.wrap = ITCORE_WRAP_HARD;
        r.revision = next_rev();
    }
    wrap_pending_ = false;
}
void Grid::erase_in_display(int mode, const Cell &blank) {
    auto erase_row = [&](int y, int x0, int x1) {
        for (int x = x0; x < x1; ++x) rows_data_[y].cells[x] = blank;
        rows_data_[y].revision = next_rev();
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
    rows_data_[cursor_.y].revision = next_rev();
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
        rows_data_[y].revision = next_rev();
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
        rows_data_[bot].revision = next_rev();
    }
}
void Grid::insert_chars(int n, const Cell &blank) {
    auto &cells = rows_data_[cursor_.y].cells;
    if (n > cols_ - cursor_.x) n = cols_ - cursor_.x;
    for (int i = cols_ - 1; i >= cursor_.x + n; --i) cells[i] = cells[i - n];
    for (int i = 0; i < n; ++i) cells[cursor_.x + i] = blank;
    rows_data_[cursor_.y].revision = next_rev();
    repair_wide_orphans(cursor_.y);
}
void Grid::delete_chars(int n, const Cell &blank) {
    auto &cells = rows_data_[cursor_.y].cells;
    if (n > cols_ - cursor_.x) n = cols_ - cursor_.x;
    for (int i = cursor_.x; i + n < cols_; ++i) cells[i] = cells[i + n];
    for (int i = cols_ - n; i < cols_; ++i) cells[i] = blank;
    rows_data_[cursor_.y].revision = next_rev();
    repair_wide_orphans(cursor_.y);
}
void Grid::repair_wide_orphans(int y) {
    if (y < 0 || y >= rows_) return;
    auto &cells = rows_data_[y].cells;
    const Cell blank = Cell::blank();
    for (int x = 0; x < cols_;) {
        Cell &c = cells[x];
        if (c.flags & ITCORE_CELL_CONTINUATION) {
            // 孤立尾格：无基字覆盖
            c = blank;
            rows_data_[y].revision = next_rev();
            ++x;
            continue;
        }
        int w = c.width ? c.width : 1;
        if (w < 1) w = 1;
        if (x + w > cols_) {
            // 宽字溢出：降为窄并清尾
            c.width = 1;
            c.flags = static_cast<uint16_t>(c.flags & ~ITCORE_CELL_CONTINUATION);
            for (int k = x + 1; k < cols_; ++k) cells[k] = blank;
            rows_data_[y].revision = next_rev();
            ++x;
            continue;
        }
        for (int k = 1; k < w; ++k) {
            if (!(cells[x + k].flags & ITCORE_CELL_CONTINUATION)) {
                Cell cont = blank;
                cont.flags = ITCORE_CELL_CONTINUATION;
                cont.width = 0;
                cells[x + k] = cont;
                rows_data_[y].revision = next_rev();
            }
        }
        // 若下一格是 CONTINUATION 但超出本字 width，清掉孤儿
        int nx = x + w;
        while (nx < cols_ && (cells[nx].flags & ITCORE_CELL_CONTINUATION)) {
            cells[nx] = blank;
            rows_data_[y].revision = next_rev();
            ++nx;
        }
        x += w;
    }
}
void Grid::clear_for_write(int x, int y, int write_width, const Cell &blank) {
    if (y < 0 || y >= rows_ || x < 0 || x >= cols_) return;
    auto &cells = rows_data_[y].cells;
    // 若目标是 CONTINUATION：回溯到基字并整段清空
    int base = x;
    if (cells[x].flags & ITCORE_CELL_CONTINUATION) {
        while (base > 0 && (cells[base].flags & ITCORE_CELL_CONTINUATION)) --base;
        int ow = cells[base].width ? cells[base].width : 1;
        for (int i = 0; i < ow && base + i < cols_; ++i) cells[base + i] = blank;
        rows_data_[y].revision = next_rev();
    }
    // 覆盖区间内每个基字及其尾格
    int end = x + (write_width > 0 ? write_width : 1);
    if (end > cols_) end = cols_;
    for (int i = x; i < end; ) {
        if (cells[i].flags & ITCORE_CELL_CONTINUATION) {
            int b = i;
            while (b > 0 && (cells[b].flags & ITCORE_CELL_CONTINUATION)) --b;
            int ow = cells[b].width ? cells[b].width : 1;
            for (int k = 0; k < ow && b + k < cols_; ++k) cells[b + k] = blank;
            rows_data_[y].revision = next_rev();
            i = b + ow;
            continue;
        }
        int ow = cells[i].width ? cells[i].width : 1;
        if (ow < 1) ow = 1;
        // 窄盖宽：清掉多出的尾格
        if (ow > 1 && i + ow > end) {
            for (int k = end; k < i + ow && k < cols_; ++k) cells[k] = blank;
            rows_data_[y].revision = next_rev();
        }
        for (int k = 0; k < ow && i + k < cols_; ++k) {
            if (i + k >= x && i + k < end) cells[i + k] = blank;
        }
        i += ow;
    }
    // 写 CONTINUATION 场景：确保区间左侧若被切到中段已清净（上面已处理）
    rows_data_[y].revision = next_rev();
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
    const bool extend = !lines_.empty() && !lines_.back().hard;
    bytes_ += line.mem_bytes();
    ++gen_;
    lines_.push_back(std::move(line));
    if (display_rows_valid_) append_display_group(extend);
    trim();
}
void Scrollback::trim() {
    while (!lines_.empty() &&
           (lines_.size() > max_lines_ || bytes_ > max_bytes_)) {
        bytes_ -= lines_.front().mem_bytes();
        lines_.pop_front();
        if (display_rows_valid_) trim_display_group();
        ++evicted_;
    }
}
void Scrollback::clear() {
    ++gen_;
    lines_.clear(); bytes_ = 0;
    display_groups_.clear(); display_rows_total_ = 0;
    display_rows_valid_ = true;
}

static void reflow_line(const LogicalLine &ll, uint16_t cols, std::vector<Row> &out, bool last_hard,
                        const std::vector<SelPos> *src = nullptr, std::vector<std::vector<SelPos>> *opos = nullptr,
                        SelPos empty_pos = {}) {
    (void)last_hard;
    if (cols == 0) return;
    size_t i = 0;
    while (i < ll.cells.size() || (i == 0 && ll.cells.empty())) {
        std::vector<SelPos> rp;
        if (opos) rp.assign(cols, empty_pos);
        size_t row_first = i;
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
            if (opos && src) for (uint8_t k = 0; k < w && col + k < cols; ++k) rp[col + k] = (*src)[i];
            col += w;
            ++i;
            // skip continuation sources already consumed above via flag skip
        }
        if (opos && src) {
            SelPos tail = empty_pos;
            if (i < ll.cells.size()) tail = (*src)[i];
            else if (i > row_first) { tail = (*src)[i - 1]; tail.off += 1; }
            else if (!ll.cells.empty()) { tail = src->back(); tail.off += 1; }
            for (size_t k = col; k < cols; ++k) rp[k] = tail;
            opos->push_back(std::move(rp));
        }
        bool more = i < ll.cells.size();
        if (more) r.wrap = ITCORE_WRAP_SOFT;
        else r.wrap = ll.hard ? ITCORE_WRAP_HARD : ITCORE_WRAP_SOFT;
        out.push_back(std::move(r));
        if (!more) break;
        if (ll.cells.empty()) break;
    }
}

size_t Scrollback::group_display_rows(size_t first, size_t count, std::deque<DisplayGroup> &groups) const {
    ++debug_hist_rows_group_recompute_count;
    LogicalLine merged;
    std::vector<Row> rows;
    size_t total = 0, start = first;
    for (size_t i = first; i < first + count; ++i) {
        const auto &line = lines_[i];
        merged.cells.insert(merged.cells.end(), line.cells.begin(), line.cells.end());
        merged.hard = line.hard;
        if (!line.hard && i + 1 < first + count) continue;
        rows.clear();
        reflow_line(merged, display_rows_cols_, rows, merged.hard);
        groups.push_back({i + 1 - start, rows.size()});
        total += rows.size();
        merged.cells.clear();
        start = i + 1;
    }
    return total;
}
void Scrollback::append_display_group(bool extend) {
    // 脏尾可包含多个已闭合组，统一留到下一次查询拆分/重排。
    if (!extend && (display_groups_.empty() || !display_groups_.back().dirty))
        display_groups_.push_back({});
    auto &group = display_groups_.back();
    ++group.lines;
    group.dirty = true;
}
void Scrollback::trim_display_group() {
    auto &group = display_groups_.front();
    if (--group.lines == 0) {
        display_rows_total_ -= group.rows;
        display_groups_.pop_front();
    } else {
        group.dirty = true;
    }
}
void Scrollback::recount_display_group(bool front) const {
    const auto group = front ? display_groups_.front() : display_groups_.back();
    std::deque<DisplayGroup> groups;
    const size_t first = front ? 0 : lines_.size() - group.lines;
    const size_t rows = group_display_rows(first, group.lines, groups);
    display_rows_total_ -= group.rows;
    display_rows_total_ += rows;
    if (front) {
        display_groups_.pop_front();
        for (auto i = groups.rbegin(); i != groups.rend(); ++i) display_groups_.push_front(*i);
    } else {
        display_groups_.pop_back();
        for (const auto &g : groups) display_groups_.push_back(g);
    }
}
void Scrollback::rebuild_display_groups(uint16_t cols) const {
    display_groups_.clear();
    display_rows_cols_ = cols;
    display_rows_total_ = group_display_rows(0, lines_.size(), display_groups_);
    display_rows_valid_ = true;
}
size_t Scrollback::merged_display_rows(uint16_t cols, bool *recomputed) const {
    const bool rebuild = !display_rows_valid_ || display_rows_cols_ != cols;
    if (rebuild) rebuild_display_groups(cols);
    if (!display_groups_.empty() && display_groups_.front().dirty) recount_display_group(true);
    if (!display_groups_.empty() && display_groups_.back().dirty) recount_display_group(false);
    if (recomputed) *recomputed = rebuild;
    return display_rows_total_;
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
    if (priv == '?' && f == 'p' && inter == "$") { set_param_if_default(t, 0, 0); return "DECRQM"; }
    if (priv == 0 && f == 'p' && inter == "$") { set_param_if_default(t, 0, 0); return "DECRQM_ANSI"; }
    if (priv == '>' && f == 'c' && no_inter) { set_param_if_default(t, 0, 0); return "DA2"; }
    if (priv == '=' && f == 'c' && no_inter) { set_param_if_default(t, 0, 0); return "DA3"; }
    if (priv == '>' && f == 'q' && no_inter) { set_param_if_default(t, 0, 0); return "XTVERSION"; }
    if (priv != 0) return "";  // 其它私有：消费但不填默认

    if (!no_inter) {
        if (inter == " " && f == 'q') { set_param_if_default(t, 0, 0); return "DECSCUSR"; }
        if (inter == "!" && f == 'p') return "DECSTR";
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
    case 'Z': set_param_if_default(t, 0, 1); return "CBT";
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
    main_.set_rev_counter(&row_rev_);
    alt_.set_rev_counter(&row_rev_);
    set_pool_budget(std::max<uint64_t>(1u << 20, (mem_limit ? mem_limit : (64ull << 20)) / 8));
    main_.reset(cols, rows);
    alt_.reset(cols, rows);
    reset_tabs();
}

void Screen::reset_tabs() {
    tabs_.assign(main_.cols(), false);
    for (size_t x = 8; x < tabs_.size(); x += 8) tabs_[x] = true;
}

void Screen::evict_thunk(Row &&r, void *ctx) {
    static_cast<Screen *>(ctx)->evict_to_scrollback(std::move(r));
}

void Screen::evict_to_scrollback(Row &&r) {
    if (modes_.alt) return;  // 备用屏不进历史
    // 每个出屏行保存为一段；hard=false 表示重排时与下一段合并。
    LogicalLine ll;
    ll.id = r.id ? r.id : next_logic_id_++;
    ll.hard = (r.wrap == ITCORE_WRAP_HARD);
    for (auto &c : r.cells) {
        if (c.flags & ITCORE_CELL_CONTINUATION) continue;
        ll.cells.push_back(c);
    }
    // 去掉尾部空白
    while (!ll.cells.empty() && ll.cells.back().grapheme_id == 0 && ll.cells.back().flags == 0)
        ll.cells.pop_back();
    scrollback_.push(std::move(ll));
}

// 改进 reflow：合并连续 soft 逻辑行
static void reflow_merged(const Scrollback &sb, uint16_t cols, std::vector<Row> &out,
                          std::vector<std::vector<SelPos>> *opos = nullptr) {
    out.clear();
    if (opos) opos->clear();
    LogicalLine acc;
    std::vector<SelPos> src;
    SelPos empty{};
    for (size_t i = 0; i < sb.logical_count(); ++i) {
        auto &ll = sb.at(i);
        if (acc.cells.empty() && acc.id == 0) { acc.id = ll.id; empty = SelPos{ll.id, 0}; }
        for (size_t k = 0; k < ll.cells.size(); ++k) {
            acc.cells.push_back(ll.cells[k]);
            if (opos) src.push_back(SelPos{ll.id, uint32_t(k)});
        }
        if (ll.hard) {
            acc.hard = true;
            reflow_line(acc, cols, out, true, &src, opos, empty);
            acc = LogicalLine{};
            src.clear();
        } else {
            acc.hard = false;
        }
    }
    // 用段边界判断开放尾组，空 soft 段即使 id=0 也占一显示行。
    if (!sb.empty() && !sb.at(sb.logical_count() - 1).hard) {
        acc.hard = false;
        reflow_line(acc, cols, out, false, &src, opos, empty);
    }
}

size_t Scrollback::merged_display_rows_full_for_test(uint16_t cols) const {
    std::vector<Row> rows;
    reflow_merged(*this, cols, rows);
    return rows.size();
}

void Screen::resize(uint16_t cols, uint16_t rows) {
    if (!cols || !rows) return;
    if (cols != main_.cols() || rows != main_.rows()) ++search_serial_;
    if (cols != main_.cols()) scrollback_.invalidate_display_rows();
    // 主屏：把当前网格软/硬行卸到 scrollback 再重排回填（简化：仅改网格尺寸，scrollback 重排用于快照）
    main_.resize(cols, rows);
    alt_.resize(cols, rows);
    size_t old = tabs_.size();
    tabs_.resize(cols, false);
    for (size_t x = old; x < tabs_.size(); ++x) tabs_[x] = x % 8 == 0 && x;
    bump();
}

void Screen::write_utf8_text(const std::string &text) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(text.data());
    size_t n = text.size(), i = 0;
    while (i < n) {
        uint32_t cp; size_t adv;
        utf8_next(p + i, n - i, &cp, &adv);
        // DEC 特殊图形（上游 VT100CharacterSet / charsetToUnicode）：0x5F..0x7E → 画线字符
        if (charset_[gl_] == '0' && cp >= 0x5F && cp <= 0x7E) {
            static const uint16_t kDec[] = {0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
                0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD,
                0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};
            uint32_t u = kDec[cp - 0x5F];
            uint8_t b[3]; size_t bn;
            if (u < 0x800) { b[0] = uint8_t(0xC0 | (u >> 6)); b[1] = uint8_t(0x80 | (u & 63)); bn = 2; }
            else { b[0] = uint8_t(0xE0 | (u >> 12)); b[1] = uint8_t(0x80 | ((u >> 6) & 63)); b[2] = uint8_t(0x80 | (u & 63)); bn = 3; }
            write_char(u, b, bn);
            i += adv ? adv : 1;
            continue;
        }
        write_char(cp, p + i, adv);
        i += adv ? adv : 1;
    }
}

void Screen::write_char(uint32_t cp, const uint8_t *utf8, size_t utf8_len) {
    auto &g = grid();
    if (join_cluster(cp, utf8, utf8_len)) return;
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
    cell.hyperlink_id = link_id_;
    if (modes_.insert) g.insert_chars(w, Cell::blank());

    auto place = [&](int x, int y) {
        if (x + w > g.cols()) return false;
        g.clear_for_write(x, y, w, Cell::blank());
        g.row(y).cells[x] = cell;
        for (int k = 1; k < w; ++k) {
            Cell cont = Cell::blank();
            cont.flags = ITCORE_CELL_CONTINUATION;
            cont.width = 0;
            cont.hyperlink_id = cell.hyperlink_id;
            g.row(y).cells[x + k] = cont;
        }
        g.row(y).revision = g.next_rev();
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
    last_base_cp_ = last_cp_ = cp;
    last_cluster_cps_ = 1;
    bump();
}

// 字素簇合并（上游 StringToScreenChars）：组合/ZWJ+Extended_Pictographic/区域指示符成对并入前一格；
// Default_Ignorable 丢弃；VS16（主屏，同 vs16SupportedInPrimaryScreen=YES/vs16Supported=NO）与
// 肤色修饰序列把窄 emoji 扩成 2 格。单簇最多 kMaxClusterCps 个码点，超出丢弃（防池膨胀）。
bool Screen::join_cluster(uint32_t cp, const uint8_t *utf8, size_t utf8_len) {
    constexpr int kMaxClusterCps = 32;
    if (ucd::is_ignorable(cp)) return true;
    bool join = false;
    if (has_last_) {
        if (ucd::is_combining(cp)) join = true;
        else if (last_cp_ == 0x200D && ucd::is_ext_pict(cp)) join = true;
        else if (ucd::is_regional_indicator(cp) && ucd::is_regional_indicator(last_base_cp_) &&
                 last_cluster_cps_ == 1) join = true;
    }
    if (!join) return false;
    auto &g = grid();
    int y = g.cursor().y;
    int tx = -1;
    if (g.wrap_pending()) {
        // 评审：wrap_pending 时回写本行最后一个非 CONTINUATION 格，不触发换行
        tx = g.cols() - 1;
        while (tx > 0 && (g.row(y).cells[tx].flags & ITCORE_CELL_CONTINUATION)) --tx;
        if (tx < 0 || (g.row(y).cells[tx].flags & ITCORE_CELL_CONTINUATION)) tx = -1;
    } else if (g.cursor().x > 0) {
        tx = g.cursor().x - 1;
        while (tx > 0 && (g.row(y).cells[tx].flags & ITCORE_CELL_CONTINUATION)) --tx;
    }
    if (tx < 0) return true;
    if (last_cluster_cps_ >= kMaxClusterCps) return true;
    size_t olen = 0;
    const char *og = pool_.get(last_printed_.grapheme_id, &olen);
    std::string merged(og, olen);
    merged.append(reinterpret_cast<const char *>(utf8), utf8_len);
    uint32_t gid = pool_.intern(merged);
    Row &row = g.row(y);
    row.cells[tx].grapheme_id = gid;
    last_printed_.grapheme_id = gid;
    last_cp_ = cp;
    ++last_cluster_cps_;
    bool widen = row.cells[tx].width == 1 &&
                 ((cp == 0xFE0F && !modes_.alt && ucd::is_vs16_base(last_base_cp_)) ||
                  (ucd::is_modifier(cp) && ucd::is_modifier_base(last_base_cp_)));
    if (widen && !g.wrap_pending() && tx + 1 < g.cols() && g.cursor().x == tx + 1) {
        g.clear_for_write(tx + 1, y, 1, Cell::blank());
        row.cells[tx].width = 2;
        Cell cont = Cell::blank();
        cont.flags = ITCORE_CELL_CONTINUATION;
        cont.width = 0;
        cont.hyperlink_id = row.cells[tx].hyperlink_id;
        row.cells[tx + 1] = cont;
        g.cursor().x = tx + 2;
        if (g.cursor().x >= g.cols()) { g.cursor().x = g.cols(); g.set_wrap_pending(modes_.wrap); }
        last_printed_.width = 2;
    }
    row.revision = grid().next_rev();
    bump();
    return true;
}

// OSC 8（上游 VT100Terminal executeLink）：URL 超过 maxURLLength 忽略；空 URL 结束链接。
void Screen::set_hyperlink(std::string_view params, std::string_view url) {
    constexpr size_t kMaxUrl = 2097152;  // iTermAdvancedSettingsModel maxURLLength
    if (url.size() > kMaxUrl) return;
    if (url.empty()) { link_id_ = 0; return; }
    std::string_view id;
    size_t i = 0;
    while (i <= params.size()) {
        size_t e = params.find(':', i);
        if (e == std::string_view::npos) e = params.size();
        auto kv = params.substr(i, e - i);
        if (kv.substr(0, 3) == "id=") id = kv.substr(3);
        i = e + 1;
    }
    link_id_ = pool_.intern_hyperlink(url, id);
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
    int x = std::min(g.cursor().x, g.cols() - 1) + 1;
    while (x < g.cols() - 1 && !(size_t(x) < tabs_.size() && tabs_[x])) ++x;
    g.cursor().x = std::min(x, g.cols() - 1);
    g.set_wrap_pending(false);
}
void Screen::back_tab() {
    auto &g = grid();
    int x = std::min(g.cursor().x, g.cols() - 1) - 1;
    while (x > 0 && !(size_t(x) < tabs_.size() && tabs_[x])) --x;
    g.cursor().x = std::max(x, 0);
    g.set_wrap_pending(false);
}
void Screen::save_cursor() {
    auto &s = modes_.alt ? saved_alt_ : saved_main_;
    s.cur = grid().cursor(); s.fg = pen_.fg; s.bg = pen_.bg; s.underline_color = pen_.underline_color; s.flags = pen_.flags;
    s.g[0] = charset_[0]; s.g[1] = charset_[1]; s.gl = gl_; s.valid = true;
}
void Screen::restore_cursor() {
    auto &s = modes_.alt ? saved_alt_ : saved_main_;
    if (!s.valid) { grid().cursor() = {0, 0}; pen_ = Pen{}; charset_[0] = charset_[1] = 'B'; gl_ = 0; return; }
    grid().cursor() = s.cur; pen_.fg = s.fg; pen_.bg = s.bg; pen_.underline_color = s.underline_color; pen_.flags = s.flags;
    charset_[0] = s.g[0]; charset_[1] = s.g[1]; gl_ = s.gl;
    grid().set_wrap_pending(false);
    grid().clamp_cursor(false);
}
// 上游 VT100Terminal softReset 子集（xterm DECSTR）：不清屏、不动光标位置
void Screen::soft_reset() {
    modes_.cursor_visible = true;
    modes_.insert = false;
    modes_.app_cursor = false;
    modes_.app_keypad = false;
    modes_.wrap = true;
    modes_.cursor_style = 0;      // 上游 softReset 复位光标样式
    if (modes_.sync_output) ++sync_ends_;
    modes_.sync_output = false;   // 关 ?2026，避免画面冻结
    modes_.focus_events = false;  // 关 ?1004
    modes_.alternate_scroll = false;  // 上游 softReset 清 alternateScrollMode（?1007）
    grid().set_origin(false);
    grid().set_scroll(0, grid().rows() - 1);
    pen_ = Pen{};
    charset_[0] = charset_[1] = 'B'; gl_ = 0;
    auto &s = modes_.alt ? saved_alt_ : saved_main_;
    s = SavedCursor{};
    grid().set_wrap_pending(false);
}
// DECALN：整屏填 E，复位滚动区，光标回原点（上游 screenFillWithE / vttest）
void Screen::decaln() {
    auto &g = grid();
    g.set_origin(false);
    g.set_scroll(0, g.rows() - 1);
    Cell e = Cell::blank(); e.grapheme_id = pool_.intern("E");
    for (int y = 0; y < g.rows(); ++y) {
        for (auto &c : g.row(y).cells) c = e;
        g.row(y).wrap = ITCORE_WRAP_HARD;
        g.row(y).revision = g.next_rev();
    }
    g.cursor() = {0, 0};
    g.set_wrap_pending(false);
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
        case 4: {
            // 无子参数 → 单线。0 关；1..5 单/双/波浪/点/虚。未知子参数只置下划线，样式保留。
            if (subs.size() <= 1 || !subs[1]) set_underline_style(pen_.flags, ITCORE_UL_SINGLE);
            else if (*subs[1] == 0) set_underline_style(pen_.flags, 0);
            else if (*subs[1] >= 1 && *subs[1] <= 5) set_underline_style(pen_.flags, *subs[1]);
            else pen_.flags = uint16_t(pen_.flags | ITCORE_CELL_UNDERLINE);
            break;
        }
        case 7: pen_.flags |= ITCORE_CELL_INVERSE; break;
        case 2: pen_.flags |= ITCORE_CELL_DIM; break;
        case 5: case 6: pen_.flags |= ITCORE_CELL_BLINK; break;   // 6 快闪按 5 处理（上游同）
        case 8: pen_.flags |= ITCORE_CELL_HIDDEN; break;
        case 9: pen_.flags |= ITCORE_CELL_STRIKE; break;
        case 21: set_underline_style(pen_.flags, ITCORE_UL_DOUBLE); break;  // 双下划线（ECMA-48 / iTerm2）
        case 25: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_BLINK); break;
        case 28: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_HIDDEN); break;
        case 29: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_STRIKE); break;
        case 22: pen_.flags = static_cast<uint16_t>(pen_.flags & ~(ITCORE_CELL_BOLD | ITCORE_CELL_DIM)); break;
        case 23: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_ITALIC); break;
        case 24:
            pen_.flags = static_cast<uint16_t>(pen_.flags & ~(ITCORE_CELL_UNDERLINE | ITCORE_CELL_UL_STYLE_MASK));
            break;
        case 27: pen_.flags = static_cast<uint16_t>(pen_.flags & ~ITCORE_CELL_INVERSE); break;
        // 上划线：固定的 iTerm2 提交里 executeSGR 没有 53/55，按 ECMA-48 与 GAP-14 实现。颜色跟前景。
        case 53: pen_.flags = uint16_t(pen_.flags | ITCORE_CELL_OVERLINE); break;
        case 55: pen_.flags = uint16_t(pen_.flags & ~ITCORE_CELL_OVERLINE); break;
        case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
            pen_.fg = kColorIndexed(static_cast<uint8_t>(p - 30)); break;
        case 39: pen_.fg = kColorDefault; break;
        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
            pen_.bg = kColorIndexed(static_cast<uint8_t>(p - 40)); break;
        case 49: pen_.bg = kColorDefault; break;
        case 59: pen_.underline_color = kColorDefault; break;
        case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
            pen_.fg = kColorIndexed(static_cast<uint8_t>(p - 90 + 8)); break;
        case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
            pen_.bg = kColorIndexed(static_cast<uint8_t>(p - 100 + 8)); break;
        case 38: case 48: case 58: {
            if (subs.size() > 1) {
                uint32_t col = 0;
                if (color_from_colon(subs, &col)) {
                    if (p == 38) pen_.fg = col;
                    else if (p == 48) pen_.bg = col;
                    else pen_.underline_color = col;
                }
            }
            break;
        }
        default: break;
        }
    };
    for (size_t i = 0; i < t.params.size(); ++i) {
        const auto &subs = t.params[i];
        int p = subs.empty() || !subs[0] ? 0 : *subs[0];
        if ((p == 38 || p == 48 || p == 58) && subs.size() == 1) {
            uint32_t col = 0;
            bool wrote = false;
            int follow = semicolon_color_span(t, i, &col, &wrote);
            if (follow > 0) {
                if (wrote) {
                    if (p == 38) pen_.fg = col;
                    else if (p == 48) pen_.bg = col;
                    else pen_.underline_color = col;
                }
                i += size_t(follow);
                continue;
            }
        }
        apply_one(p, subs);
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
            case 1048: if (set) save_cursor(); else restore_cursor(); break;  // 与 DECSC/DECRC 同语义（含字符集）
            case 4: modes_.smooth_scroll = set; break;  // DECSCLM，与 ANSI 4（IRM）互不相干
            case 2004: modes_.bracketed_paste = set; break;
            case 5:  // DECSCNM：默认前景/背景对调，需整屏重画
                if (modes_.reverse_screen != set) { modes_.reverse_screen = set; touch_all(); }
                break;
            case 1004: modes_.focus_events = set; break;
            case 2026: if (modes_.sync_output && !set) ++sync_ends_; modes_.sync_output = set; break;
            case 1: modes_.app_cursor = set; break;  // DECCKM
            case 9:
                if (set) modes_.mouse_mode = (modes_.mouse_mode & ~(ITCORE_MOUSE_MODE_NORMAL|ITCORE_MOUSE_MODE_BUTTON|ITCORE_MOUSE_MODE_ANY)) | ITCORE_MOUSE_MODE_X10;
                else modes_.mouse_mode &= ~ITCORE_MOUSE_MODE_X10;
                break;
            case 1000:
                if (set) modes_.mouse_mode = (modes_.mouse_mode & ~(ITCORE_MOUSE_MODE_X10|ITCORE_MOUSE_MODE_BUTTON|ITCORE_MOUSE_MODE_ANY)) | ITCORE_MOUSE_MODE_NORMAL;
                else modes_.mouse_mode &= ~ITCORE_MOUSE_MODE_NORMAL;
                break;
            case 1002:
                if (set) modes_.mouse_mode = (modes_.mouse_mode & ~(ITCORE_MOUSE_MODE_X10|ITCORE_MOUSE_MODE_NORMAL|ITCORE_MOUSE_MODE_ANY)) | ITCORE_MOUSE_MODE_BUTTON;
                else modes_.mouse_mode &= ~ITCORE_MOUSE_MODE_BUTTON;
                break;
            case 1003:
                if (set) modes_.mouse_mode = (modes_.mouse_mode & ~(ITCORE_MOUSE_MODE_X10|ITCORE_MOUSE_MODE_NORMAL|ITCORE_MOUSE_MODE_BUTTON)) | ITCORE_MOUSE_MODE_ANY;
                else modes_.mouse_mode &= ~ITCORE_MOUSE_MODE_ANY;
                break;
            case 1006:
                if (set) modes_.mouse_mode |= ITCORE_MOUSE_MODE_SGR;
                else modes_.mouse_mode &= ~ITCORE_MOUSE_MODE_SGR;
                break;
            case 1007: modes_.alternate_scroll = set; break;  // Alternate Scroll：备用屏滚轮→方向键
            default: break;
            }
        } else {
            if (p == 4) modes_.insert = set;
            else if (p == 20) modes_.lnm = set;
        }
    }
    bump();
}

void Screen::enter_alt(bool clear) {
    if (!modes_.alt) {
        sel_clear();
        ++search_serial_;
        saved_main_.cur = main_.cursor();
        saved_main_.fg = pen_.fg; saved_main_.bg = pen_.bg; saved_main_.underline_color = pen_.underline_color;
        saved_main_.flags = pen_.flags;
        saved_main_.valid = true;
        modes_.alt = true;
    }
    if (clear) alt_.clear_all(Cell::blank());
    bump();
}
void Screen::leave_alt(bool clear) {
    if (modes_.alt) {
        sel_clear();
        ++search_serial_;
        modes_.alt = false;
        if (clear) alt_.clear_all(Cell::blank());
        if (saved_main_.valid) {
            main_.cursor() = saved_main_.cur;
            pen_.fg = saved_main_.fg; pen_.bg = saved_main_.bg; pen_.underline_color = saved_main_.underline_color;
            pen_.flags = saved_main_.flags;
            main_.clamp_cursor(false);
        }
    }
    bump();
}

std::string Screen::sgr_decrqss() const {
    // 报告笔状态本身，不因 INVERSE 交换颜色（格内颜色尚未交换，再送一遍能还原）。
    std::string o = "0";
    auto add = [&](const char *s) { o += ';'; o += s; };
    const uint16_t f = pen_.flags;
    if (f & ITCORE_CELL_BOLD) add("1");
    if (f & ITCORE_CELL_DIM) add("2");
    if (f & ITCORE_CELL_ITALIC) add("3");
    if (f & ITCORE_CELL_UNDERLINE) {
        const int st = (f & ITCORE_CELL_UL_STYLE_MASK) >> ITCORE_CELL_UL_STYLE_SHIFT;
        if (st == ITCORE_UL_DOUBLE) add("21");
        else if (st == ITCORE_UL_WAVY) add("4:3");
        else if (st == ITCORE_UL_DOTTED) add("4:4");
        else if (st == ITCORE_UL_DASHED) add("4:5");
        else add("4");
    }
    if (f & ITCORE_CELL_BLINK) add("5");
    if (f & ITCORE_CELL_INVERSE) add("7");
    if (f & ITCORE_CELL_HIDDEN) add("8");
    if (f & ITCORE_CELL_STRIKE) add("9");
    if (f & ITCORE_CELL_OVERLINE) add("53");
    append_sgr_color(&o, pen_.fg, 0);
    append_sgr_color(&o, pen_.bg, 1);
    append_sgr_color(&o, pen_.underline_color, 2);
    return o;
}

void Screen::reply_dcs(const Token &t, std::string *reply) const {
    if (!reply) return;
    const std::string &s = t.text;
    if (s.size() >= 2 && s[0] == '+' && s[1] == 'q') {
        if (t.trunc) { *reply += "\x1bP0+r\x1b\\"; return; }
        const std::string_view body(s.data() + 2, s.size() - 2);
        if (body.empty()) { *reply += "\x1bP0+r\x1b\\"; return; }
        size_t i = 0;
        while (i <= body.size()) {
            const size_t semi = body.find(';', i);
            const std::string_view hex = body.substr(i, semi == std::string_view::npos ? std::string_view::npos : semi - i);
            std::string name;
            const char *val = nullptr;
            if (hex_decode(hex, &name)) val = xtcap_value(name);
            if (val)
                *reply += "\x1bP1+r" + hex_encode(name) + "=" + hex_encode(val) + "\x1b\\";
            else
                *reply += "\x1bP0+r\x1b\\";  // 未知或不合法：不带回名字，避免沉默
            if (semi == std::string_view::npos) break;
            i = semi + 1;
            if (i == body.size()) { *reply += "\x1bP0+r\x1b\\"; break; }
        }
        return;
    }
    if (s.size() >= 2 && s[0] == '$' && s[1] == 'q') {
        if (t.trunc) { *reply += "\x1bP0$r\x1b\\"; return; }
        const std::string pt = s.substr(2);
        auto ok = [&](const std::string &payload) {
            *reply += "\x1bP1$r" + payload + pt + "\x1b\\";
        };
        if (pt == "m") ok(sgr_decrqss());
        else if (pt == "r") {
            ok(std::to_string(grid().scroll_top() + 1) + ";" + std::to_string(grid().scroll_bottom() + 1));
        } else if (pt == " q") ok(std::to_string(modes_.cursor_style));
        else if (pt == "\"q") ok("0");  // DECSCA：未实现字符保护，全部可擦
        else *reply += "\x1bP0$r\x1b\\";
    }
}

void Screen::apply(const Token &t, std::string *reply) {
    apply_one(t, reply);
    maybe_compact();
}

void Screen::maybe_compact() {
    if (want_compact_ || pool_.bytes() > pool_threshold_) compact_pool();
}

void Screen::full_reset() {
    uint16_t cols = main_.cols(), rows = main_.rows();
    if (modes_.sync_output) ++sync_ends_;
    ++search_serial_;
    modes_ = ScreenModes{};
    pen_ = Pen{};
    saved_main_ = SavedCursor{}; saved_alt_ = SavedCursor{};
    if (sel_.active) ++sel_serial_;
    sel_ = Selection{};
    scrollback_.clear();
    main_.reset(cols, rows);
    alt_.reset(cols, rows);
    link_id_ = 0;
    charset_[0] = charset_[1] = 'B'; gl_ = 0;
    reset_tabs();
    has_last_ = false;
    last_printed_ = Cell::blank();
    last_x_ = last_y_ = -1;
    last_base_cp_ = last_cp_ = 0; last_cluster_cps_ = 0;
    want_compact_ = true;
    bump();
}

void Screen::compact_pool() {
    want_compact_ = false;
    std::vector<uint32_t> live;
    auto add = [&](const Cell &c) {
        if (c.grapheme_id) live.push_back(c.grapheme_id);
        if (c.hyperlink_id) live.push_back(c.hyperlink_id);
    };
    main_.for_each_row([&](Row &r) { for (auto &c : r.cells) add(c); });
    alt_.for_each_row([&](Row &r) { for (auto &c : r.cells) add(c); });
    scrollback_.for_each_line([&](const LogicalLine &l) { for (const auto &c : l.cells) add(c); });
    if (has_last_) add(last_printed_);
    if (link_id_) live.push_back(link_id_);
    auto m = pool_.compact(live);
    auto map = [&](uint32_t &id) { if (id) { auto it = m.find(id); id = it == m.end() ? 0 : it->second; } };
    auto fix = [&](Cell &c) { map(c.grapheme_id); map(c.hyperlink_id); };
    main_.for_each_row([&](Row &r) { for (auto &c : r.cells) fix(c); });
    alt_.for_each_row([&](Row &r) { for (auto &c : r.cells) fix(c); });
    scrollback_.for_each_cell_id(map);
    fix(last_printed_);
    map(link_id_);
    main_.touch_all_rows();
    alt_.touch_all_rows();
    // 滞回：存活量本身很大时避免每个 token 都压缩
    pool_threshold_ = std::max(pool_budget_ / 4 * 3, pool_.bytes() * 2);
    ++compactions_;
    bump();
}

void Screen::apply_one(const Token &t, std::string *reply) {
    switch (t.type) {
    case TokenType::Text:
        write_utf8_text(t.text);
        break;
    case TokenType::C0:
        switch (t.c) {
        case 0x07: break;  // BEL：事件层处理
        case 0x08: backspace(); break;
        case 0x09: tab(); break;
        case 0x0A: case 0x0B: case 0x0C: linefeed(true); if (modes_.lnm) grid().cursor().x = 0; break;
        case 0x0D: carriage_return(); break;
        case 0x0E: gl_ = 1; break;  // SO → G1
        case 0x0F: gl_ = 0; break;  // SI → G0
        default: break;
        }
        break;
    case TokenType::Esc:
        if (t.inter.empty()) {
            if (t.final == '7') save_cursor();          // DECSC
            else if (t.final == '8') restore_cursor();  // DECRC
            else if (t.final == 'H') { int x = std::min(grid().cursor().x, grid().cols() - 1); if (size_t(x) < tabs_.size()) tabs_[x] = true; }  // HTS
            else if (t.final == 'D') index();
            else if (t.final == 'M') reverse_index();
            else if (t.final == 'E') { carriage_return(); linefeed(true); }
            else if (t.final == 'c') full_reset();  // RIS
        } else if (t.inter == "#" && t.final == '8') {
            decaln();
        } else if ((t.inter == "(" || t.inter == ")") && t.final) {
            // SCS：只区分 DEC 特殊图形 '0'，其余（B/A/U/K…）按 ASCII
            charset_[t.inter == "(" ? 0 : 1] = t.final == '0' ? '0' : 'B';
        }
        break;
    case TokenType::Csi: {
        Token t2 = t;
        std::string cmd = apply_csi_defaults(t2);
        auto &g = grid();
        Cell blank = Cell::blank();
        if (cmd == "CHT") { for (int i = param_or(t2, 0, 1); i > 0 && g.cursor().x < g.cols() - 1; --i) tab(); }
        else if (cmd == "CBT") { for (int i = param_or(t2, 0, 1); i > 0 && g.cursor().x > 0; --i) back_tab(); }
        else if (cmd == "TBC") {
            int m = param_or(t2, 0, 0);
            if (m == 0) { int x = std::min(g.cursor().x, g.cols() - 1); if (size_t(x) < tabs_.size()) tabs_[x] = false; }
            else if (m == 3) std::fill(tabs_.begin(), tabs_.end(), false);
        }
        else if (cmd == "DECSTR") soft_reset();
        else if (cmd == "DECSCUSR") { int p = param_or(t2, 0, 0); if (p >= 0 && p <= 6) modes_.cursor_style = p; }
        else if (cmd == "DECDSR" && reply) {
            // 上游 executeDecDeviceStatusReport 子集
            int p = param_or(t2, 0, 0);
            if (p == 6) *reply += "\x1b[?" + std::to_string(g.cursor().y - g.origin_top() + 1) + ";" + std::to_string(std::min(g.cursor().x + 1, (int)g.cols())) + "R";
            else if (p == 15) *reply += "\x1b[?13n";        // 无打印机
            else if (p == 25) *reply += "\x1b[?20n";        // UDK 未锁
            else if (p == 26) *reply += "\x1b[?27;1;0;0n";  // 北美键盘
            else if (p == 53 || p == 55) *reply += "\x1b[?53n";  // 无定位器
        }
        else if (cmd == "CUU") { g.cursor().y -= param_or(t2, 0, 1); g.set_wrap_pending(false); g.clamp_cursor(false); }
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
            if (m == 3) { ++search_serial_; scrollback_.clear(); want_compact_ = true; if (sel_.active && !sel_.alt) sel_clear(); }
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
            g.row(g.cursor().y).revision = g.next_rev();
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
        else if (cmd == "DECRQM") {
            // DECRPM：CSI ? Ps ; Pm $ y（Pm 0 未识别 / 1 置位 / 2 复位）
            int p = param_or(t2, 0, 0);
            if (reply) *reply += "\x1b[?" + std::to_string(p) + ";" + std::to_string(dec_mode_state(p)) + "$y";
        }
        else if (cmd == "DECRQM_ANSI") {
            int p = param_or(t2, 0, 0);
            if (reply) *reply += "\x1b[" + std::to_string(p) + ";" + std::to_string(ansi_mode_state(p)) + "$y";
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
                              g.cursor().y - g.origin_top() + 1, std::min(g.cursor().x + 1, (int)g.cols()));
                *reply += buf;
            }
        }
        else if (cmd == "DA2" && reply && param_or(t2, 0, 0) == 0)
            *reply += "\x1b[>1;0;0c";  // VT220 次级标识，补丁号 0（docs/identity-14.3.md）
        else if (cmd == "DA" && reply && param_or(t2, 0, 0) == 0)
            *reply += "\x1b[?62;22c";  // VT220 + ANSI 颜色；不宣告未实现的特性
        else if (cmd == "DA3" && reply && param_or(t2, 0, 0) == 0)
            *reply += "\x1bP!|" + hex_encode(kTermName) + "\x1b\\";  // 稳定单元号，不抬高 VT 级别
        else if (cmd == "XTVERSION" && reply && param_or(t2, 0, 0) == 0)
            *reply += std::string("\x1bP>|") + kTermName + " " + kTermVersion + "\x1b\\";
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
    case TokenType::Dcs:
        reply_dcs(t, reply);
        break;
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
    // 视口偏移（first_row<0 看历史）：光标按视口相对行给出，移出视口则不可见
    int64_t crow = int64_t(grid().cursor().y) - first_row;
    bool cin = crow >= 0 && crow < row_count;
    sn.cursor_row = cin ? int(crow) : 0;
    sn.cursor_visible = modes_.cursor_visible && cin;
    sn.mouse_mode = modes_.mouse_mode;
    sn.reverse_screen = modes_.reverse_screen;
    sn.cursor_style = modes_.cursor_style;
    sn.pool = pool_.ref();  // 增量引用：O(1)，不拷贝
    sn.epoch = sn.pool.epoch;

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
            if (hi >= 0 && hi < hist_n) {
                r = hist[static_cast<size_t>(hi)];
                // 历史行按位置取内容；用当前全局计数作 revision，保证单调且内容不变时不变
                r.revision = row_rev_;
            }
        } else if (abs < sn.rows) {
            r = view[static_cast<size_t>(abs)];
        }
        r.resize_cols(sn.cols);
        sn.rows_data.push_back(std::move(r));
    }
    // 选区叠加：选中格带 ITCORE_CELL_SELECTED，并翻转 INVERSE 以便现有渲染器直接画出
    if (sel_.active && sel_.alt == modes_.alt) {
        auto segs = sel_segments();
        SegPos a, b;
        if (sel_range(segs, &a, &b)) {
            std::unordered_map<uint64_t, size_t> ord;
            for (size_t k = 0; k < segs.size(); ++k) ord[segs[k].id] = k;
            auto pos = view_positions(first_row, row_count);
            for (size_t y = 0; y < sn.rows_data.size() && y < pos.size(); ++y) {
                auto &cells = sn.rows_data[y].cells;
                for (size_t x = 0; x < cells.size() && x < pos[y].size(); ++x) {
                    auto it = ord.find(pos[y][x].seg);
                    if (it == ord.end()) continue;
                    std::pair<size_t, uint32_t> k{it->second, pos[y][x].off};
                    if (k < a || b < k) continue;
                    cells[x].flags = uint16_t(cells[x].flags | ITCORE_CELL_SELECTED);
                    cells[x].flags = uint16_t(cells[x].flags ^ ITCORE_CELL_INVERSE);
                }
            }
        }
    }
    return sn;
}

// ---- DECRQM / 模式查询 ----
int Screen::dec_mode_state(int mode) const {
    auto st = [](bool on) { return on ? 1 : 2; };
    const uint32_t mm = modes_.mouse_mode;
    switch (mode) {
    case 1: return st(modes_.app_cursor);
    case 4: return st(modes_.smooth_scroll);
    case 7: return st(modes_.wrap);
    case 25: return st(modes_.cursor_visible);
    case 47: case 1047: case 1049: return st(modes_.alt);
    case 9: return st(mm & ITCORE_MOUSE_MODE_X10);
    case 1000: return st(mm & ITCORE_MOUSE_MODE_NORMAL);
    case 1002: return st(mm & ITCORE_MOUSE_MODE_BUTTON);
    case 1003: return st(mm & ITCORE_MOUSE_MODE_ANY);
    case 1006: return st(mm & ITCORE_MOUSE_MODE_SGR);
    case 1007: return st(modes_.alternate_scroll);
    case 2004: return st(modes_.bracketed_paste);
    case 5: return st(modes_.reverse_screen);
    case 1004: return st(modes_.focus_events);
    case 2026: return st(modes_.sync_output);
    default: return 0;
    }
}

// 上游 VT100Terminal 的 ANSI DECRQM：只回答实现了的模式，其余 Pm=0
int Screen::ansi_mode_state(int mode) const {
    switch (mode) {
    case 4: return modes_.insert ? 1 : 2;
    case 20: return modes_.lnm ? 1 : 2;
    default: return 0;
    }
}

size_t Screen::history_display_rows() const {
    if (modes_.alt) return 0;
    bool recomputed = false;
    const size_t rows = scrollback_.merged_display_rows(grid().cols(), &recomputed);
    if (recomputed) ++debug_hist_rows_recompute_count;
    return rows;
}

// ---- 选区 ----
// 段序列：主屏 = 历史逻辑段（淘汰顺序）+ 网格行；备用屏 = 仅网格行。每段的字符不含宽字符尾格。
std::vector<Screen::Seg> Screen::segments(bool alt) const {
    std::vector<Seg> v;
    if (!alt) {
        for (size_t i = 0; i < scrollback_.logical_count(); ++i) {
            auto &ll = scrollback_.at(i);
            Seg sg{ll.id, {}, ll.hard};
            for (auto &c : ll.cells) sg.chars.push_back(&c);
            v.push_back(std::move(sg));
        }
    }
    const Grid &g = alt ? alt_ : main_;
    for (int y = 0; y < g.rows(); ++y) {
        auto &r = g.row(y);
        Seg sg{r.id, {}, r.wrap == ITCORE_WRAP_HARD};
        for (auto &c : r.cells) if (!(c.flags & ITCORE_CELL_CONTINUATION)) sg.chars.push_back(&c);
        if (r.wrap == ITCORE_WRAP_WIDE)  // 宽字符放不下时行尾补的空格不算内容（上游同）
            while (!sg.chars.empty() && sg.chars.back()->grapheme_id == 0) sg.chars.pop_back();
        v.push_back(std::move(sg));
    }
    return v;
}

std::vector<std::vector<SelPos>> Screen::view_positions(int32_t first_row, uint16_t n) const {
    std::vector<std::vector<SelPos>> out;
    const uint16_t cols = grid().cols();
    std::vector<Row> hist;
    std::vector<std::vector<SelPos>> hpos;
    if (!modes_.alt) reflow_merged(scrollback_, cols, hist, &hpos);
    int64_t hist_n = int64_t(hpos.size());
    for (uint16_t i = 0; i < n; ++i) {
        int64_t abs = int64_t(first_row) + i;
        std::vector<SelPos> rp(cols);
        if (abs < 0) {
            int64_t hi = hist_n + abs;
            if (hi >= 0 && hi < hist_n) { rp = hpos[size_t(hi)]; rp.resize(cols, rp.empty() ? SelPos{} : rp.back()); }
        } else if (abs < grid().rows()) {
            auto &r = grid().row(int(abs));
            uint32_t ci = 0;
            for (uint16_t x = 0; x < cols && x < r.cells.size(); ++x) {
                if (r.cells[x].flags & ITCORE_CELL_CONTINUATION) rp[x] = SelPos{r.id, ci ? ci - 1 : 0};
                else rp[x] = SelPos{r.id, ci++};
            }
        }
        out.push_back(std::move(rp));
    }
    return out;
}

bool Screen::sel_pos_at(int32_t first_row, uint16_t col, uint16_t row, SelPos *out) const {
    auto pos = view_positions(first_row + row, 1);
    if (pos.empty() || pos[0].empty()) return false;
    *out = pos[0][std::min<size_t>(col, pos[0].size() - 1)];
    return out->seg != 0;
}

// 词字符：字母数字、非 ASCII（含 CJK）与 word_chars_（默认同上游 "/-+\~_."，itcore_set_word_chars 可配）；空白一类；其余标点各自成词
int Screen::char_class(const Cell *c) const {
    if (!c || !c->grapheme_id) return 0;
    size_t n = 0;
    const char *t = pool_.get(c->grapheme_id, &n);
    if (!t || !n) return 0;
    unsigned char ch = static_cast<unsigned char>(t[0]);
    if (ch == ' ' || ch == '\t') return 0;
    if (ch >= 0x80 || std::isalnum(ch) || word_chars_.find(char(ch)) != std::string::npos) return 1;
    return 2;
}

void Screen::unit_range(const std::vector<Seg> &segs, SegPos p, int unit, SegPos *lo, SegPos *hi) const {
    *lo = *hi = p;
    if (unit == 0 || segs.empty()) return;
    size_t k0 = p.first, k1 = p.first;
    while (k0 > 0 && !segs[k0 - 1].hard) --k0;
    while (k1 + 1 < segs.size() && !segs[k1].hard) ++k1;
    if (unit == 2) {
        *lo = {k0, 0};
        *hi = {k1, segs[k1].chars.empty() ? 0u : uint32_t(segs[k1].chars.size() - 1)};
        return;
    }
    std::vector<SegPos> flat;
    std::vector<const Cell *> cells;
    size_t at = SIZE_MAX;
    for (size_t k = k0; k <= k1; ++k)
        for (uint32_t i = 0; i < segs[k].chars.size(); ++i) {
            if (k == p.first && i == p.second) at = flat.size();
            flat.push_back({k, i});
            cells.push_back(segs[k].chars[i]);
        }
    if (flat.empty()) return;
    if (at == SIZE_MAX) {  // 点在行尾之后：取同段最后一个字符
        for (size_t i = flat.size(); i-- > 0;) if (flat[i].first <= p.first) { at = i; break; }
        if (at == SIZE_MAX) at = 0;
    }
    int cls = char_class(cells[at]);
    size_t a = at, b = at;
    if (cls != 2) {
        while (a > 0 && char_class(cells[a - 1]) == cls) --a;
        while (b + 1 < cells.size() && char_class(cells[b + 1]) == cls) ++b;
    }
    *lo = flat[a]; *hi = flat[b];
}

// 端点 → (段序号, 字符序号)，按文档序排好。段已被淘汰（历史裁剪）时夹到最前；网格 resize 截掉的段夹到最后。
bool Screen::sel_range(const Selection &sel, const std::vector<Seg> &segs, SegPos *s, SegPos *e) const {
    if (!sel.active || segs.empty()) return false;
    auto locate = [&](SelPos p, bool *found) -> SegPos {
        for (size_t k = 0; k < segs.size(); ++k) if (segs[k].id == p.seg) { *found = true; return {k, p.off}; }
        *found = false;
        if (p.seg < segs.front().id) return {0, 0};
        return {segs.size() - 1, uint32_t(segs.back().chars.size())};
    };
    bool fa = false, fb = false;
    SegPos a = locate(sel.anchor, &fa), b = locate(sel.head, &fb);
    if (!fa && !fb) return false;
    if (sel.unit == 0) {
        if (b < a) std::swap(a, b);
        *s = a; *e = b;
        return true;
    }
    SegPos alo, ahi, hlo, hhi;
    if (fa) unit_range(segs, a, sel.unit, &alo, &ahi); else alo = ahi = a;
    if (fb) unit_range(segs, b, sel.unit, &hlo, &hhi); else hlo = hhi = b;
    if (b < alo) { *s = hlo; *e = ahi; }
    else { *s = alo; *e = std::max(ahi, hhi); }
    return true;
}

void Screen::sel_touch(const Selection &old) {
    ++sel_serial_;
    // 至少推进一次全局计数：历史行的 revision 取当前计数，因此可见的历史行都会被重画
    main_.next_rev();
    for (int pass = 0; pass < 2; ++pass) {
        const Selection &sl = pass ? sel_ : old;
        if (!sl.active) continue;
        Selection probe = sel_;
        Selection saved = sel_;
        probe = sl;
        // sel_segments 按 sel_.alt 选网格；临时切换
        sel_ = probe;
        auto segs = sel_segments();
        SegPos a, b;
        bool ok = sel_range(sl, segs, &a, &b);
        sel_ = saved;
        if (!ok) continue;
        size_t hist = sl.alt ? 0 : scrollback_.logical_count();
        Grid &g = sl.alt ? alt_ : main_;
        for (size_t k = std::max(a.first, hist); k <= b.first && k < segs.size(); ++k) {
            int y = int(k - hist);
            if (y >= 0 && y < g.rows()) g.row(y).revision = g.next_rev();
        }
    }
    bump();
}

bool Screen::sel_start(int32_t first_row, uint16_t col, uint16_t row, int unit) {
    SelPos p;
    if (!sel_pos_at(first_row, col, row, &p)) { sel_clear(); return false; }
    Selection old = sel_;
    sel_ = Selection{true, modes_.alt, unit < 0 || unit > 2 ? 0 : unit, p, p};
    sel_touch(old);
    return true;
}
bool Screen::sel_extend(int32_t first_row, uint16_t col, uint16_t row) {
    if (!sel_.active || sel_.alt != modes_.alt) return false;
    SelPos p;
    if (!sel_pos_at(first_row, col, row, &p)) return false;
    if (p.seg == sel_.head.seg && p.off == sel_.head.off) return true;
    Selection old = sel_;
    sel_.head = p;
    sel_touch(old);
    return true;
}
void Screen::sel_set(SelPos anchor, SelPos head) {
    Selection old = sel_;
    sel_ = Selection{true, modes_.alt, 0, anchor, head};
    sel_touch(old);
}
void Screen::sel_clear() {
    if (!sel_.active) return;
    Selection old = sel_;
    sel_ = Selection{};
    sel_touch(old);
}

// 复制文本（上游 iTerm2 默认行为）：空格补空白格，宽字符只出一次，软换行拼接，
// 硬换行出 '\n' 并去掉该行尾部空白；跨到行尾之后（点在内容右侧）不额外补换行。
std::string Screen::sel_text() const {
    auto segs = sel_segments();
    SegPos a, b;
    if (sel_.alt != modes_.alt || !sel_range(segs, &a, &b)) return {};
    std::string out, line;
    auto rtrim = [](std::string &t) { while (!t.empty() && t.back() == ' ') t.pop_back(); };
    for (size_t k = a.first; k <= b.first; ++k) {
        const Seg &sg = segs[k];
        size_t from = k == a.first ? a.second : 0;
        size_t to = k == b.first ? std::min<size_t>(size_t(b.second) + 1, sg.chars.size()) : sg.chars.size();
        for (size_t i = from; i < to; ++i) {
            const Cell *c = sg.chars[i];
            if (!c->grapheme_id) { line.push_back(' '); continue; }
            size_t n = 0;
            const char *t = pool_.get(c->grapheme_id, &n);
            line.append(t, n);
        }
        if (k == b.first) break;
        if (sg.hard) { rtrim(line); out += line; out += '\n'; line.clear(); }
    }
    if (b.second >= segs[b.first].chars.size() || segs[b.first].hard) rtrim(line);
    out += line;
    return out;
}

std::string Screen::export_text(size_t max_bytes) const {
    auto segs = segments(false);
    std::string out, line;
    auto rtrim = [](std::string &t) { while (!t.empty() && t.back() == ' ') t.pop_back(); };
    for (size_t k = 0; k < segs.size(); ++k) {
        for (const Cell *c : segs[k].chars) {
            if (!c->grapheme_id) { line.push_back(' '); continue; }
            size_t n = 0;
            const char *t = pool_.get(c->grapheme_id, &n);
            line.append(t, n);
        }
        if (segs[k].hard || k + 1 == segs.size()) { rtrim(line); out += line; out += '\n'; line.clear(); }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (out.size() <= max_bytes) return out;
    size_t from = out.size() - max_bytes;
    while (from < out.size() && (static_cast<unsigned char>(out[from]) & 0xC0) == 0x80) ++from;  // 不切断 UTF-8
    size_t nl = out.find('\n', from);
    if (from > 0 && out[from - 1] != '\n' && nl != std::string::npos && nl + 1 - from <= 4096) from = nl + 1;  // 尽量从行首开始
    return out.substr(from);
}

size_t Screen::mem_bytes() const {
    return pool_.bytes() + main_.mem_bytes() + alt_.mem_bytes() + scrollback_.bytes() + 256;
}

}  // namespace itcore
