// SPDX-License-Identifier: GPL-2.0-only
// iTerm2 Windows 派生核心：C ABI 实现（设计 14.1 / 14.2）。解析见 vt_parser；屏幕见 screen。
#include "itcore.h"
#include "vt_parser.h"
#include "screen.h"
#include "encode.h"
#include "ucd_width.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct itcore_event {
    itcore_event_kind kind;
    std::string data;
};

struct itcore_snapshot {
    itcore::Screen::SnapshotData data;
    // ABI 行缓存（与 data.rows_data 平行）
    std::vector<std::vector<itcore_cell>> rows;
    itcore_colors colors{};
};

namespace {
constexpr std::size_t kEventCapacity = 1024;
constexpr std::size_t kPerByteReserve = 4;
constexpr std::size_t kMaxTitleChars = 256;

std::string sanitize_title(const std::string &in) {
    std::string out; std::size_t chars = 0;
    for (std::size_t i = 0; i < in.size() && chars < kMaxTitleChars;) {
        unsigned char b = in[i];
        std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + n > in.size()) break;
        if (n == 1 && (b < 0x20 || b == 0x7f)) { ++i; continue; }
        if (n == 2 && b == 0xc2 && (unsigned char)in[i + 1] < 0xa0) { i += 2; continue; }
        out.append(in, i, n); i += n; ++chars;
    }
    return out;
}
}  // namespace

struct itcore_core final : itcore::TokenSink {
    itcore::Parser parser;
    std::unique_ptr<itcore::Screen> screen;
    std::deque<itcore_event *> events;
    bool finished = false, fatal = false, invalidated_pending = false;
    uint64_t incomplete = 0;
    uint64_t resize_gen = 0;

    uint16_t cell_w = 0, cell_h = 0;                 // CSI 14/16 t
    std::string win_title, icon_title;               // CSI 22/23 t 标题栈
    std::vector<std::string> win_stack, icon_stack;  // 各最多 kTitleStackMax 项（xterm 同为 10）
    itcore_colors cfg{}, cur{};                      // 配置色 / 生效色（OSC 覆盖）

    itcore_core(uint16_t cols, uint16_t rows, uint32_t sb, uint64_t mem)
        : screen(std::make_unique<itcore::Screen>(cols, rows, sb, mem)) {
        itcore_default_colors(&cfg);
        cur = cfg;
    }
    static constexpr size_t kTitleStackMax = 10;

    static int pnum(const itcore::Token &t, size_t i, int def) {
        if (i >= t.params.size() || t.params[i].empty() || !t.params[i][0]) return def;
        return *t.params[i][0];
    }
    // CSI Ps t（XTWINOPS 的只读与标题栈部分；改窗口大小/位置的操作一律忽略）
    void winops(const itcore::Token &t, std::string *reply) {
        const auto &g = screen->grid();
        // 像素尺寸未设置时按 8x16 估算应答（后端应在首次 feed 前 itcore_set_cell_pixel_size）
        const unsigned cell_w = this->cell_w ? this->cell_w : 8u, cell_h = this->cell_h ? this->cell_h : 16u;
        switch (pnum(t, 0, 0)) {
        case 18: *reply += "\x1b[8;" + std::to_string(g.rows()) + ";" + std::to_string(g.cols()) + "t"; break;
        case 14:
            *reply += "\x1b[4;" + std::to_string(unsigned(g.rows()) * cell_h) + ";" + std::to_string(unsigned(g.cols()) * cell_w) + "t";
            break;
        case 16:
            *reply += "\x1b[6;" + std::to_string(cell_h) + ";" + std::to_string(cell_w) + "t";
            break;
        case 22: {
            int w = pnum(t, 1, 0);
            auto pushs = [](std::vector<std::string> &st, const std::string &v) {
                if (st.size() >= kTitleStackMax) st.erase(st.begin());
                st.push_back(v);
            };
            if (w == 0 || w == 1) pushs(icon_stack, icon_title);
            if (w == 0 || w == 2) pushs(win_stack, win_title);
            break;
        }
        case 23: {
            int w = pnum(t, 1, 0);
            if ((w == 0 || w == 1) && !icon_stack.empty()) {
                icon_title = std::move(icon_stack.back()); icon_stack.pop_back();
                push(ITCORE_EV_ICON_TITLE, icon_title);
            }
            if ((w == 0 || w == 2) && !win_stack.empty()) {
                win_title = std::move(win_stack.back()); win_stack.pop_back();
                push(ITCORE_EV_TITLE, win_title);
            }
            break;
        }
        default: break;  // 21t/20t（回报标题）有注入风险，不实现；其余改窗口的操作忽略
        }
    }

    // 颜色规格：rgb:h/h/h（每分量 1–4 位十六进制）、#rgb #rrggbb #rrrgggbbb #rrrrggggbbbb
    static bool parse_color(const std::string &s, uint32_t *out) {
        auto hexv = [](const std::string &h, uint32_t *v) {
            if (h.empty() || h.size() > 4) return false;
            uint32_t x = 0;
            for (char ch : h) {
                int d = std::isxdigit(static_cast<unsigned char>(ch)) ? (std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (std::tolower(ch) - 'a' + 10)) : -1;
                if (d < 0) return false;
                x = x * 16 + uint32_t(d);
            }
            uint32_t maxv = (1u << (4 * h.size())) - 1;
            *v = (x * 255 + maxv / 2) / maxv;
            return true;
        };
        uint32_t r, g, b;
        if (s.rfind("rgb:", 0) == 0) {
            auto a = s.find('/', 4), z = a == std::string::npos ? a : s.find('/', a + 1);
            if (z == std::string::npos) return false;
            if (!hexv(s.substr(4, a - 4), &r) || !hexv(s.substr(a + 1, z - a - 1), &g) || !hexv(s.substr(z + 1), &b)) return false;
        } else if (!s.empty() && s[0] == '#' && (s.size() - 1) % 3 == 0 && s.size() > 1 && s.size() <= 13) {
            size_t n = (s.size() - 1) / 3;
            if (!hexv(s.substr(1, n), &r) || !hexv(s.substr(1 + n, n), &g) || !hexv(s.substr(1 + 2 * n, n), &b)) return false;
        } else {
            return false;
        }
        *out = (r << 16) | (g << 8) | b;
        return true;
    }
    static std::string color_spec(uint32_t v) {
        char buf[32];
        unsigned r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
        std::snprintf(buf, sizeof buf, "rgb:%02x%02x/%02x%02x/%02x%02x", r, r, g, g, b, b);
        return buf;
    }
    void colors_changed() { screen->invalidate_all(); push(ITCORE_EV_RENDER_INVALIDATED); }

    // OSC 4 ; idx ; spec [; idx ; spec ...]
    void osc4(const std::string &arg, std::string *reply, const char *st) {
        size_t pos = 0;
        bool changed = false;
        while (pos <= arg.size()) {
            size_t a = arg.find(';', pos);
            if (a == std::string::npos) break;
            size_t b = arg.find(';', a + 1);
            std::string is = arg.substr(pos, a - pos), spec = arg.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            char *end = nullptr;
            long idx = std::strtol(is.c_str(), &end, 10);
            if (!is.empty() && *end == 0 && idx >= 0 && idx < 256) {
                if (spec == "?") *reply += "\x1b]4;" + std::to_string(idx) + ";" + color_spec(cur.palette[idx]) + st;
                else if (uint32_t v; parse_color(spec, &v)) { cur.palette[idx] = v; changed = true; }
            }
            if (b == std::string::npos) break;
            pos = b + 1;
        }
        if (changed) colors_changed();
    }
    // OSC 10/11/12 ; spec [; spec ...]（依次作用于 10、11、12）
    void osc_dyn(int first, const std::string &arg, std::string *reply, const char *st) {
        uint32_t *slot[3] = {&cur.foreground, &cur.background, &cur.cursor};
        bool changed = false;
        size_t pos = 0;
        for (int n = first; n <= 12; ++n) {
            size_t b = arg.find(';', pos);
            std::string spec = arg.substr(pos, b == std::string::npos ? std::string::npos : b - pos);
            if (spec == "?") *reply += "\x1b]" + std::to_string(n) + ";" + color_spec(*slot[n - 10]) + st;
            else if (uint32_t v; parse_color(spec, &v)) { *slot[n - 10] = v; changed = true; }
            if (b == std::string::npos) break;
            pos = b + 1;
        }
        if (changed) colors_changed();
    }
    // OSC 104 [; idx ...]：无参数恢复全部
    void osc104(const std::string &arg, bool has_arg) {
        if (!has_arg || arg.empty()) { std::copy(std::begin(cfg.palette), std::end(cfg.palette), cur.palette); colors_changed(); return; }
        bool changed = false;
        size_t pos = 0;
        while (true) {
            size_t b = arg.find(';', pos);
            std::string is = arg.substr(pos, b == std::string::npos ? std::string::npos : b - pos);
            char *end = nullptr;
            long idx = std::strtol(is.c_str(), &end, 10);
            if (!is.empty() && *end == 0 && idx >= 0 && idx < 256) { cur.palette[idx] = cfg.palette[idx]; changed = true; }
            if (b == std::string::npos) break;
            pos = b + 1;
        }
        if (changed) colors_changed();
    }
    // B4 OSC 52
    bool clip_write = false, clip_read = false;
    size_t clip_max = ITCORE_CLIPBOARD_DEFAULT_MAX;
    static bool b64_decode(std::string_view in, size_t cap, std::string *out) {
        out->clear();
        uint32_t acc = 0; int bits = 0; size_t pad = 0;
        for (char ch : in) {
            int v;
            if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
            else if (ch == '+') v = 62;
            else if (ch == '/') v = 63;
            else if (ch == '=') { ++pad; continue; }
            else return false;
            if (pad) return false;  // '=' 之后不得再有数据
            acc = (acc << 6) | uint32_t(v); bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (out->size() >= cap) return false;
                out->push_back(char((acc >> bits) & 0xFF));
            }
        }
        return pad <= 2 && (in.size() % 4 == 0 || pad == 0);
    }
    void osc52(const std::string &arg) {
        auto sc = arg.find(';');
        std::string sel = arg.substr(0, sc);
        if (sc == std::string::npos) { push(ITCORE_EV_CLIPBOARD_REQUEST, sel); return; }
        std::string_view data = std::string_view(arg).substr(sc + 1);
        if (data == "?") { push(clip_read ? ITCORE_EV_CLIPBOARD_READ : ITCORE_EV_CLIPBOARD_REQUEST, sel); return; }
        std::string dec;
        if (!clip_write || data.size() > (clip_max / 3 + 1) * 4 || !b64_decode(data, clip_max, &dec)) {
            push(ITCORE_EV_CLIPBOARD_REQUEST, sel);
            return;
        }
        push(ITCORE_EV_CLIPBOARD_WRITE, std::move(dec));
    }
    ~itcore_core() override { for (auto *e : events) delete e; }

    void push(itcore_event_kind k, std::string d = {}) {
        if (k == ITCORE_EV_RENDER_INVALIDATED) {
            if (invalidated_pending) return;
            invalidated_pending = true;
        }
        events.push_back(new itcore_event{k, std::move(d)});
    }
    void token(const itcore::Token &t) override {
        using itcore::TokenType;
        switch (t.type) {
        case TokenType::Text:
            screen->apply(t, nullptr);
            push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        case TokenType::C0:
            if (t.c == 0x07) push(ITCORE_EV_BELL);
            else {
                screen->apply(t, nullptr);
                push(ITCORE_EV_RENDER_INVALIDATED);
            }
            break;
        case TokenType::Csi: {
            std::string reply;
            if (t.final == 't' && !t.priv && t.inter.empty()) winops(t, &reply);
            else screen->apply(t, &reply);
            if (!reply.empty()) push(ITCORE_EV_REPLY_BYTES, std::move(reply));
            push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        }
        case TokenType::Esc:
            if (t.final == 'c' && t.inter.empty()) {  // RIS：颜色恢复配置，清标题栈
                win_stack.clear(); icon_stack.clear();
                if (!win_title.empty()) { win_title.clear(); push(ITCORE_EV_TITLE, std::string()); }
                if (!icon_title.empty()) { icon_title.clear(); push(ITCORE_EV_ICON_TITLE, std::string()); }
                cur = cfg;
                screen->invalidate_all();
            }
            screen->apply(t, nullptr);
            push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        case TokenType::Osc: {
            auto semi = t.text.find(';');
            std::string num = t.text.substr(0, semi);
            std::string arg = semi == std::string::npos ? std::string() : t.text.substr(semi + 1);
            std::string reply;
            if (num == "0" || num == "1" || num == "2") {
                std::string v = sanitize_title(arg);
                if (num != "1") { win_title = v; push(ITCORE_EV_TITLE, v); }
                if (num != "2") { icon_title = v; push(ITCORE_EV_ICON_TITLE, v); }
            }
            else if (num == "4") osc4(arg, &reply, t.bel_term ? "\x07" : "\x1b\\");
            else if (num == "10" || num == "11" || num == "12") osc_dyn(std::stoi(num), arg, &reply, t.bel_term ? "\x07" : "\x1b\\");
            else if (num == "104") osc104(arg, semi != std::string::npos);
            else if (num == "110") { cur.foreground = cfg.foreground; colors_changed(); }
            else if (num == "111") { cur.background = cfg.background; colors_changed(); }
            else if (num == "112") { cur.cursor = cfg.cursor; colors_changed(); }
            else if (num == "8") {
                auto sc = arg.find(';');
                if (sc != std::string::npos)
                    screen->set_hyperlink(std::string_view(arg).substr(0, sc), std::string_view(arg).substr(sc + 1));
            }
            else if (num == "52") osc52(arg);
            if (!reply.empty()) push(ITCORE_EV_REPLY_BYTES, std::move(reply));
            break;
        }
        case TokenType::Incomplete: ++incomplete; break;
        case TokenType::Error: fatal = true; break;
        default: break;
        }
    }
};

extern "C" {

int itcore_create(const itcore_options *opt, itcore_core **out) {
    if (!out) return -1;
    *out = nullptr;
    if (opt && (opt->struct_size < sizeof(itcore_options) || opt->abi_version != ITCORE_ABI_VERSION))
        return -2;
    uint16_t cols = 80, rows = 24;
    uint32_t sb = 10000;
    uint64_t mem = 64ull << 20;
    if (opt) {
        if (opt->columns) cols = opt->columns;
        if (opt->rows) rows = opt->rows;
        if (opt->scrollback_lines) sb = opt->scrollback_lines;
        if (opt->memory_limit_bytes) mem = opt->memory_limit_bytes;
    }
    *out = new (std::nothrow) itcore_core(cols, rows, sb, mem);
    return *out ? 0 : -3;
}

itcore_feed_result itcore_feed(itcore_core *c, const uint8_t *bytes, size_t length) {
    itcore_feed_result r{sizeof(itcore_feed_result), 0, ITCORE_OK};
    if (!c || c->finished || c->fatal || (!bytes && length)) { r.status = ITCORE_FATAL; return r; }
    std::size_t i = 0;
    for (; i < length; ++i) {
        if (c->events.size() + kPerByteReserve > kEventCapacity) { r.status = ITCORE_NEED_DRAIN; break; }
        c->parser.feed(bytes + i, 1, *c);
        if (c->fatal) { ++i; r.status = ITCORE_FATAL; break; }
    }
    c->parser.flush_text(*c);
    r.accepted = i;
    return r;
}

int itcore_finish_input(itcore_core *c) {
    if (!c) return -1;
    if (c->finished) return 0;
    c->finished = true;
    if (c->fatal) return 0;
    bool utf8 = c->parser.utf8_pending();
    c->parser.finish(*c);
    if (utf8) {
        // 残缺 UTF-8 → 解析器已产出 U+FFFD Text 或需补写
        itcore::Token t;
        t.type = itcore::TokenType::Text;
        t.text = "\xEF\xBF\xBD";
        c->screen->apply(t, nullptr);
        c->push(ITCORE_EV_RENDER_INVALIDATED);
    }
    return 0;
}

uint64_t itcore_incomplete_count(const itcore_core *c) { return c ? c->incomplete : 0; }

itcore_event *itcore_next_event(itcore_core *c) {
    if (!c || c->events.empty()) return nullptr;
    itcore_event *e = c->events.front();
    c->events.pop_front();
    if (e->kind == ITCORE_EV_RENDER_INVALIDATED) c->invalidated_pending = false;
    return e;
}
itcore_event_kind itcore_event_type(const itcore_event *e) { return e->kind; }
const uint8_t *itcore_event_data(const itcore_event *e, size_t *len) {
    if (len) *len = e ? e->data.size() : 0;
    return e ? reinterpret_cast<const uint8_t *>(e->data.data()) : nullptr;
}
void itcore_event_release(itcore_event *e) { delete e; }
void itcore_destroy(itcore_core *c) { delete c; }

itcore_mode_state itcore_mode_query(const itcore_core *c, uint32_t mode) {
    if (!c || !c->screen || mode > 0x7fffffffu) return ITCORE_MODE_NOT_RECOGNIZED;
    return static_cast<itcore_mode_state>(c->screen->dec_mode_state(int(mode)));
}
static constexpr int kMaxClusterCps = 32;  // 与 screen.cpp Screen::join_cluster 一致
size_t itcore_text_clusters(const itcore_core *c, const char *utf8, size_t length, uint8_t *widths, uint32_t *byte_lens, size_t cap) {
    if (!c || !c->screen || !utf8) return 0;
    const bool alt = c->screen->on_alt();  // 备用屏 VS16 不扩宽（screen.cpp 写网格同规则）
    const uint8_t *p = reinterpret_cast<const uint8_t *>(utf8);
    size_t n = 0, i = 0;
    uint32_t last_cp = 0, base_cp = 0; int cps = 0; uint8_t w = 0; uint32_t bytes = 0;
    auto flush = [&] {
        if (!cps) return;
        if (n < cap) { if (widths) widths[n] = w; if (byte_lens) byte_lens[n] = bytes; }
        ++n; cps = 0; bytes = 0;
    };
    while (i < length) {
        uint32_t cp = 0; size_t adv = 0;
        itcore::utf8_next(p + i, length - i, &cp, &adv);
        if (!adv) adv = 1;
        i += adv;
        if (itcore::ucd::is_ignorable(cp)) { bytes += uint32_t(adv); continue; }  // 不占格，字节并入当前簇
        bool join = cps > 0 && (itcore::ucd::is_combining(cp) || (last_cp == 0x200D && itcore::ucd::is_ext_pict(cp)) ||
                                (itcore::ucd::is_regional_indicator(cp) && itcore::ucd::is_regional_indicator(base_cp) && cps == 1));
        if (join) {
            // 同 Screen::join_cluster：单簇最多 kMaxClusterCps 个码点，超出的码点丢弃（字节仍记入本簇，不另起簇）
            if (cps >= kMaxClusterCps) { bytes += uint32_t(adv); continue; }
            if (w == 1 && ((cp == 0xFE0F && !alt && itcore::ucd::is_vs16_base(base_cp)) ||
                           (itcore::ucd::is_modifier(cp) && itcore::ucd::is_modifier_base(base_cp)))) w = 2;
            ++cps; last_cp = cp; bytes += uint32_t(adv);
            continue;
        }
        flush();
        int cw = itcore::cell_width(cp);
        w = uint8_t(cp == 0xFFFD || cw <= 0 ? 1 : cw > 2 ? 2 : cw);
        base_cp = last_cp = cp; cps = 1; bytes += uint32_t(adv);
    }
    flush();
    return n;
}
uint64_t itcore_sync_end_count(const itcore_core *c) { return c && c->screen ? c->screen->sync_end_count() : 0; }
itcore_mode_state itcore_ansi_mode_query(const itcore_core *c, uint32_t mode) {
    if (!c || !c->screen || mode > 0x7fffffffu) return ITCORE_MODE_NOT_RECOGNIZED;
    return static_cast<itcore_mode_state>(c->screen->ansi_mode_state(int(mode)));
}
int itcore_selection_start(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row) {
    if (!c || !c->screen) return 0;
    uint64_t before = c->screen->sel_serial();
    c->screen->sel_start(first_row, col, row);
    if (c->screen->sel_serial() == before) return 0;
    c->push(ITCORE_EV_RENDER_INVALIDATED);
    return 1;
}
int itcore_selection_start_unit(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row, int unit) {
    if (!c || !c->screen) return 0;
    uint64_t before = c->screen->sel_serial();
    c->screen->sel_start(first_row, col, row, unit);
    if (c->screen->sel_serial() == before) return 0;
    c->push(ITCORE_EV_RENDER_INVALIDATED);
    return 1;
}
uint32_t itcore_history_rows(const itcore_core *c) {
    if (!c || !c->screen) return 0;
    return static_cast<uint32_t>(c->screen->history_display_rows());
}
// 测试钩子（不在公开头文件）：history_display_rows 实际重排次数（B2 缓存命中验证）
extern "C" uint64_t itcore_debug_history_recomputes(const itcore_core *c) {
    return (c && c->screen) ? c->screen->debug_hist_rows_recompute_count : 0;
}
int itcore_selection_extend(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row) {
    if (!c || !c->screen) return 0;
    uint64_t before = c->screen->sel_serial();
    c->screen->sel_extend(first_row, col, row);
    if (c->screen->sel_serial() == before) return 0;
    c->push(ITCORE_EV_RENDER_INVALIDATED);
    return 1;
}
void itcore_selection_clear(itcore_core *c) {
    if (!c || !c->screen || !c->screen->sel_active()) return;
    c->screen->sel_clear();
    c->push(ITCORE_EV_RENDER_INVALIDATED);
}
int itcore_selection_active(const itcore_core *c) { return c && c->screen && c->screen->sel_active() ? 1 : 0; }
int itcore_selection_text(itcore_core *c, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!c || !c->screen || !out) return -1;
    if (!c->screen->sel_active()) return 1;
    *out = itcore_encode::make_buf(c->screen->sel_text());
    return *out ? 0 : -1;
}

int itcore_set_clipboard_policy(itcore_core *c, const itcore_clipboard_policy *cfg) {
    if (!c) return -1;
    if (!cfg) { c->clip_write = c->clip_read = false; c->clip_max = ITCORE_CLIPBOARD_DEFAULT_MAX; return 0; }
    if (cfg->struct_size < sizeof(itcore_clipboard_policy)) return -1;
    c->clip_write = cfg->allow_write != 0;
    c->clip_read = cfg->allow_read != 0;
    c->clip_max = cfg->max_bytes ? cfg->max_bytes : ITCORE_CLIPBOARD_DEFAULT_MAX;
    return 0;
}
int itcore_encode_clipboard_reply(itcore_core *c, const char *utf8, size_t len, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!c || !out || (!utf8 && len)) return -1;
    if (!c->clip_read) return 1;
    if (len > c->clip_max) return -2;
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s = "\x1b]52;c;";
    s.reserve(10 + (len + 2) / 3 * 4 + 2);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(uint8_t(utf8[i])) << 16;
        if (i + 1 < len) v |= uint32_t(uint8_t(utf8[i + 1])) << 8;
        if (i + 2 < len) v |= uint8_t(utf8[i + 2]);
        s.push_back(T[(v >> 18) & 63]); s.push_back(T[(v >> 12) & 63]);
        s.push_back(i + 1 < len ? T[(v >> 6) & 63] : '=');
        s.push_back(i + 2 < len ? T[v & 63] : '=');
    }
    s += "\x1b\\";
    *out = itcore_encode::make_buf(std::move(s));
    return *out ? 0 : -1;
}
int itcore_set_word_chars(itcore_core *c, const char *chars, size_t len) {
    if (!c || !c->screen) return -1;
    if (!chars) { c->screen->set_word_chars(itcore::Screen::kDefaultWordChars); return 0; }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(chars[i]);
        if (ch < 0x21 || ch > 0x7E || std::isalnum(ch)) return -1;
    }
    c->screen->set_word_chars(std::string(chars, len));
    return 0;
}
void itcore_set_cell_pixel_size(itcore_core *c, uint16_t w, uint16_t h) {
    if (c) { c->cell_w = w; c->cell_h = h; }
}
void itcore_default_colors(itcore_colors *o) {
    if (!o) return;
    *o = {};
    o->struct_size = sizeof(itcore_colors);
    static const uint32_t base[16] = {0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
                                      0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
    for (int i = 0; i < 16; ++i) o->palette[i] = base[i];
    for (int i = 0; i < 216; ++i) {
        auto lv = [](int v) { return uint32_t(v ? 55 + v * 40 : 0); };
        o->palette[16 + i] = (lv(i / 36) << 16) | (lv(i / 6 % 6) << 8) | lv(i % 6);
    }
    for (int i = 0; i < 24; ++i) { uint32_t v = uint32_t(8 + i * 10); o->palette[232 + i] = (v << 16) | (v << 8) | v; }
    o->foreground = o->cursor = 0xC7C7C7;
    o->background = 0x000000;
}
int itcore_set_colors(itcore_core *c, const itcore_colors *cfg) {
    if (!c || !cfg || cfg->struct_size < sizeof(itcore_colors)) return -1;
    c->cfg = *cfg;
    c->cur = *cfg;  // 新配置覆盖 OSC 临时改动
    c->colors_changed();
    return 0;
}
void itcore_snapshot_colors(const itcore_snapshot *s, itcore_colors *out) {
    if (!out) return;
    if (s) *out = s->colors; else itcore_default_colors(out);
    out->struct_size = sizeof(itcore_colors);
}

int itcore_export_text(const itcore_core *c, size_t max_bytes, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!c || !c->screen || !out) return -1;
    *out = itcore_encode::make_buf(c->screen->export_text(max_bytes));
    return *out ? 0 : -1;
}

int itcore_resize(itcore_core *c, uint16_t columns, uint16_t rows, uint64_t generation) {
    if (!c || !columns || !rows) return -1;
    if (generation && generation < c->resize_gen) return -2;  // 过期
    c->resize_gen = generation ? generation : c->resize_gen + 1;
    c->screen->resize(columns, rows);
    c->push(ITCORE_EV_RENDER_INVALIDATED);
    return 0;
}

int itcore_encode_text(itcore_core *c, const char *utf8, size_t len, itcore_buffer **o) {
    if (!c || !c->screen) { if (o) *o = nullptr; return -1; }
    return itcore_encode::encode_text(c->screen.get(), utf8, len, o);
}
int itcore_encode_paste(itcore_core *c, const char *utf8, size_t len, itcore_buffer **o) {
    if (!c || !c->screen) { if (o) *o = nullptr; return -1; }
    return itcore_encode::encode_paste(c->screen.get(), utf8, len, o);
}
int itcore_encode_key(itcore_core *c, const itcore_key_event *ev, itcore_buffer **o) {
    if (!c || !c->screen) { if (o) *o = nullptr; return -1; }
    return itcore_encode::encode_key(c->screen.get(), ev, o);
}
int itcore_encode_mouse(itcore_core *c, const itcore_mouse_event *ev, itcore_buffer **o) {
    if (!c || !c->screen) { if (o) *o = nullptr; return -1; }
    return itcore_encode::encode_mouse(c->screen.get(), ev, o);
}
void itcore_buffer_release(itcore_buffer *b) { itcore_encode::release_buf(b); }

itcore_snapshot *itcore_acquire_snapshot(itcore_core *c, int32_t first_row, uint16_t row_count) {
    if (!c || !c->screen || !row_count) return nullptr;
    auto *s = new (std::nothrow) itcore_snapshot;
    if (!s) return nullptr;
    s->data = c->screen->take_snapshot(first_row, row_count);
    s->colors = c->cur;
    s->data.generation = c->resize_gen;
    s->rows.resize(s->data.rows_data.size());
    for (size_t i = 0; i < s->data.rows_data.size(); ++i) {
        auto &src = s->data.rows_data[i];
        s->rows[i].resize(src.cells.size());
        for (size_t x = 0; x < src.cells.size(); ++x) s->rows[i][x] = src.cells[x].to_abi();
    }
    return s;
}
uint64_t itcore_snapshot_revision(const itcore_snapshot *s) { return s ? s->data.revision : 0; }
uint64_t itcore_snapshot_epoch(const itcore_snapshot *s) { return s ? s->data.epoch : 0; }
uint16_t itcore_snapshot_columns(const itcore_snapshot *s) { return s ? s->data.cols : 0; }
uint16_t itcore_snapshot_rows(const itcore_snapshot *s) { return s ? s->data.rows : 0; }
const itcore_cell *itcore_snapshot_row(const itcore_snapshot *s, uint16_t row, itcore_wrap *wrap, uint64_t *row_revision) {
    if (!s || row >= s->rows.size()) return nullptr;
    if (wrap) *wrap = s->data.rows_data[row].wrap;
    if (row_revision) *row_revision = s->data.rows_data[row].revision;
    return s->rows[row].data();
}
const char *itcore_snapshot_grapheme(const itcore_snapshot *s, uint32_t grapheme_id, size_t *len) {
    if (!s) { if (len) *len = 0; return ""; }
    return s->data.pool.get(grapheme_id, len);
}
void itcore_snapshot_cursor(const itcore_snapshot *s, uint16_t *col, uint16_t *row, int *visible) {
    if (!s) return;
    if (col) *col = static_cast<uint16_t>(s->data.cursor_col);
    if (row) *row = static_cast<uint16_t>(s->data.cursor_row);
    if (visible) *visible = s->data.cursor_visible ? 1 : 0;
}
uint64_t itcore_snapshot_generation(const itcore_snapshot *s) { return s ? s->data.generation : 0; }
uint32_t itcore_snapshot_mouse_mode(const itcore_snapshot *s) { return s ? s->data.mouse_mode : 0; }
void itcore_snapshot_release(itcore_snapshot *s) { delete s; }
void itcore_snapshot_cursor_style(const itcore_snapshot *s, int *ps) { if (ps) *ps = s ? s->data.cursor_style : 0; }
int itcore_snapshot_reverse_screen(const itcore_snapshot *s) { return s && s->data.reverse_screen ? 1 : 0; }
int itcore_snapshot_hyperlink(const itcore_snapshot *s, uint32_t hyperlink_id,
                              const char **url, size_t *url_len, const char **id, size_t *id_len) {
    if (!s || !hyperlink_id) return 0;
    size_t n = 0;
    const char *p = s->data.pool.get(hyperlink_id, &n);
    std::string_view u, i;
    if (!itcore::GraphemePool::decode_hyperlink(p, n, &u, &i)) return 0;
    if (url) *url = u.data();
    if (url_len) *url_len = u.size();
    if (id) *id = i.data();
    if (id_len) *id_len = i.size();
    return 1;
}

}  // extern "C"
