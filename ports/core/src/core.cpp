// SPDX-License-Identifier: GPL-2.0-only
// iTerm2 Windows 派生核心：C ABI 实现（设计 14.1）。解析部分见 vt_parser.cpp。
#include "itcore.h"
#include "vt_parser.h"
#include <deque>
#include <memory>
#include <new>
#include <string>

struct itcore_event {
    itcore_event_kind kind;
    std::string data;
};

namespace {
constexpr std::size_t kEventCapacity = 1024;  // 有界事件队列
constexpr std::size_t kPerByteReserve = 4;    // 单字节最多产生的事件数（含失效通知）
constexpr std::size_t kMaxTitleChars = 256;

// 去除控制字符，按码点截断到 256（输入已由解析器保证为 UTF-8 字节，非法字节按单字节计）
std::string sanitize_title(const std::string &in) {
    std::string out; std::size_t chars = 0;
    for (std::size_t i = 0; i < in.size() && chars < kMaxTitleChars;) {
        unsigned char b = in[i];
        std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + n > in.size()) break;
        if (n == 1 && (b < 0x20 || b == 0x7f)) { ++i; continue; }
        if (n == 2 && b == 0xc2 && (unsigned char)in[i + 1] < 0xa0) { i += 2; continue; }  // C1
        out.append(in, i, n); i += n; ++chars;
    }
    return out;
}
}  // namespace

struct itcore_core final : itcore::TokenSink {
    itcore::Parser parser;
    std::deque<itcore_event *> events;
    bool finished = false, fatal = false, invalidated_pending = false;
    uint64_t incomplete = 0;

    ~itcore_core() override { for (auto *e : events) delete e; }

    void push(itcore_event_kind k, std::string d = {}) {
        if (k == ITCORE_EV_RENDER_INVALIDATED) {  // 合并：队列中最多一个
            if (invalidated_pending) return;
            invalidated_pending = true;
        }
        events.push_back(new itcore_event{k, std::move(d)});
    }
    void token(const itcore::Token &t) override {
        using itcore::TokenType;
        switch (t.type) {
        case TokenType::Text: push(ITCORE_EV_RENDER_INVALIDATED); break;
        case TokenType::C0:
            if (t.c == 0x07) push(ITCORE_EV_BELL); else push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        case TokenType::Csi:
            if (t.final == 'n' && t.priv == 0 && t.inter.empty() && t.params.size() == 1 &&
                t.params[0][0] == 5)
                push(ITCORE_EV_REPLY_BYTES, "\x1b[0n");  // DSR 5n；6n 待屏幕模型（T05）
            else push(ITCORE_EV_RENDER_INVALIDATED);
            break;
        case TokenType::Esc: push(ITCORE_EV_RENDER_INVALIDATED); break;
        case TokenType::Osc: {
            auto semi = t.text.find(';');
            std::string num = t.text.substr(0, semi);
            std::string arg = semi == std::string::npos ? std::string() : t.text.substr(semi + 1);
            if (num == "0" || num == "2") push(ITCORE_EV_TITLE, sanitize_title(arg));
            else if (num == "52") {  // 首版策略：核心消费并拒绝，不触达系统剪贴板
                push(ITCORE_EV_CLIPBOARD_REQUEST, arg.substr(0, arg.find(';')));
            }
            break;
        }
        case TokenType::Incomplete: ++incomplete; break;
        case TokenType::Error: fatal = true; break;
        default: break;  // DCS/SOS/PM/APC/INVALID：按状态机消费，无副作用
        }
    }
};

extern "C" {

int itcore_create(const itcore_options *opt, itcore_core **out) {
    if (!out) return -1;
    *out = nullptr;
    if (opt && (opt->struct_size < sizeof(itcore_options) || opt->abi_version != ITCORE_ABI_VERSION)) return -2;
    *out = new (std::nothrow) itcore_core();
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
    if (c->finished) return 0;  // 重复调用无副作用
    c->finished = true;
    if (c->fatal) return 0;
    bool utf8 = c->parser.utf8_pending();
    c->parser.finish(*c);
    if (utf8) c->push(ITCORE_EV_RENDER_INVALIDATED);  // 残缺 UTF-8 → 一个 U+FFFD 进入屏幕
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

// 以下为 T05 屏幕模型范围，当前返回失败。
int itcore_resize(itcore_core *, uint16_t, uint16_t, uint64_t) { return -100; }
int itcore_encode_text(itcore_core *, const char *, size_t, itcore_buffer **o) { if (o) *o = nullptr; return -100; }
int itcore_encode_paste(itcore_core *, const char *, size_t, itcore_buffer **o) { if (o) *o = nullptr; return -100; }
void itcore_buffer_release(itcore_buffer *) {}
itcore_snapshot *itcore_acquire_snapshot(itcore_core *, int32_t, uint16_t) { return nullptr; }
}
