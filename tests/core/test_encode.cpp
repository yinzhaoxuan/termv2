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

static std::string paste(itcore_core *c, const std::string &in) {
    itcore_buffer *b = nullptr;
    CHECK(itcore_encode_paste(c, in.data(), in.size(), &b) == 0);
    std::string r = buf_str(b);
    itcore_buffer_release(b);
    return r;
}

static void test_paste_sanitize() {
    itcore_core *c = make();
    const std::string O = "\x1b[200~", C = "\x1b[201~";
    // 换行规范化
    CHECK(paste(c, "a\r\nb\nc\rd") == "a\rb\rc\rd");
    CHECK(paste(c, "\n\r\n\r") == "\r\r\r");
    // 剥离标记（非 bracketed 也剥离）
    CHECK(paste(c, "a" + O + "b") == "ab");
    CHECK(paste(c, "a" + C + "b") == "ab");
    CHECK(paste(c, "a\xc2\x9b" "200~b\xc2\x9b" "201~c") == "abc");  // 8-bit CSI
    CHECK(paste(c, "x\x1b[20\x1b[201~1~y") == "xy");                 // 拼接标记
    CHECK(paste(c, "x\x1b[2\x1b[200~01~y") == "xy");                 // 剥离后再拼出的标记
    feed(c, "\x1b[?2004h");
    CHECK(paste(c, "a" + C + "\nb") == O + "a\rb" + C);
    // DECSTR 不复位 ?2004：仍然包裹
    feed(c, "\x1b[!p");
    CHECK(paste(c, "q") == O + "q" + C);
    // RIS 复位：不再包裹
    feed(c, "\x1b" "c");
    CHECK(paste(c, "q\n") == "q\r");
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
    test_paste_sanitize();
    test_key();
    test_mouse();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ok\n");
    return failures ? 1 : 0;
}
