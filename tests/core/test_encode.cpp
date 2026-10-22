// ENC-01：encode_text / paste / key / mouse
#include "itcore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make() {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 100, 1ull << 20};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static void drain(itcore_core *c) {
    while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e);
}
static void feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    drain(c);
}
static std::string buf_str(itcore_buffer *b) {
    if (!b || !b->data) return {};
    return std::string(reinterpret_cast<char *>(b->data), b->length);
}

static void test_text_paste() {
    itcore_core *c = make();
    itcore_buffer *b = nullptr;
    CHECK(itcore_encode_text(c, "hi", 2, &b) == 0 && b && buf_str(b) == "hi");
    itcore_buffer_release(b);
    // 超限粘贴
    std::string big(32 * 1024 + 1, 'x');
    CHECK(itcore_encode_paste(c, big.data(), big.size(), &b) == -2 && b == nullptr);
    // 普通粘贴
    CHECK(itcore_encode_paste(c, "abc", 3, &b) == 0 && buf_str(b) == "abc");
    itcore_buffer_release(b);
    // bracketed
    feed(c, "\x1b[?2004h");
    CHECK(itcore_encode_paste(c, "xy", 2, &b) == 0);
    CHECK(buf_str(b) == "\x1b[200~xy\x1b[201~");
    itcore_buffer_release(b);
    {
        auto *s = itcore_acquire_snapshot(c, 0, 1);
        CHECK(itcore_snapshot_mouse_mode(s) == 0);
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);
}

static void test_key() {
    itcore_core *c = make();
    itcore_buffer *b = nullptr;
    itcore_key_event ev{};
    ev.struct_size = sizeof(ev);
    ev.vk = ITCORE_VK_UP;
    CHECK(itcore_encode_key(c, &ev, &b) == 0 && buf_str(b) == "\x1b[A");
    itcore_buffer_release(b);
    // DECCKM
    feed(c, "\x1b[?1h");
    CHECK(itcore_encode_key(c, &ev, &b) == 0 && buf_str(b) == "\x1bOA");
    itcore_buffer_release(b);
    // unicode
    ev = {}; ev.struct_size = sizeof(ev); ev.unicode = 'a';
    CHECK(itcore_encode_key(c, &ev, &b) == 0 && buf_str(b) == "a");
    itcore_buffer_release(b);
    ev.modifiers = ITCORE_MOD_CTRL; ev.unicode = 'c';
    CHECK(itcore_encode_key(c, &ev, &b) == 0 && buf_str(b).size() == 1 && buf_str(b)[0] == 3);
    itcore_buffer_release(b);
    itcore_destroy(c);
}

static void test_mouse() {
    itcore_core *c = make();
    itcore_buffer *b = nullptr;
    itcore_mouse_event ev{};
    ev.struct_size = sizeof(ev);
    ev.col = 2; ev.row = 3; ev.button = ITCORE_MBUTTON_LEFT; ev.action = ITCORE_MOUSE_PRESS;
    // 无模式 → 本地
    CHECK(itcore_encode_mouse(c, &ev, &b) == 1 && b == nullptr);
    feed(c, "\x1b[?1000h\x1b[?1006h");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 1);
        uint32_t m = itcore_snapshot_mouse_mode(s);
        CHECK(m & ITCORE_MOUSE_MODE_NORMAL);
        CHECK(m & ITCORE_MOUSE_MODE_SGR);
        itcore_snapshot_release(s);
    }
    CHECK(itcore_encode_mouse(c, &ev, &b) == 0);
    CHECK(buf_str(b) == "\x1b[<0;3;4M");  // 1-based
    itcore_buffer_release(b);
    ev.action = ITCORE_MOUSE_RELEASE;
    CHECK(itcore_encode_mouse(c, &ev, &b) == 0 && buf_str(b) == "\x1b[<0;3;4m");
    itcore_buffer_release(b);
    itcore_destroy(c);
}

int main() {
    test_text_paste();
    test_key();
    test_mouse();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ok\n");
    return failures ? 1 : 0;
}
