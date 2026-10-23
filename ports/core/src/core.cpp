// SPDX-License-Identifier: GPL-2.0-only
// iTerm2 Windows 派生核心：C ABI 实现（设计 14.1 / 14.2）。解析见 vt_parser；屏幕见 screen。
#include "itcore.h"
#include "vt_parser.h"
#include "screen.h"
#include "encode.h"
#include <deque>
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

    itcore_core(uint16_t cols, uint16_t rows, uint32_t sb, uint64_t mem)
        : screen(std::make_unique<itcore::Screen>(cols, rows, sb, mem)) {}
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
            screen->apply(t, &reply);
            if (!reply.empty()) push(ITCORE_EV_REPLY_BYTES, std::move(reply));
            push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        }
        case TokenType::Esc:
            screen->apply(t, nullptr);
            push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        case TokenType::Osc: {
            auto semi = t.text.find(';');
            std::string num = t.text.substr(0, semi);
            std::string arg = semi == std::string::npos ? std::string() : t.text.substr(semi + 1);
            if (num == "0" || num == "2") push(ITCORE_EV_TITLE, sanitize_title(arg));
            else if (num == "8") {
                auto sc = arg.find(';');
                if (sc != std::string::npos)
                    screen->set_hyperlink(std::string_view(arg).substr(0, sc), std::string_view(arg).substr(sc + 1));
            }
            else if (num == "52")
                push(ITCORE_EV_CLIPBOARD_REQUEST, arg.substr(0, arg.find(';')));
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
