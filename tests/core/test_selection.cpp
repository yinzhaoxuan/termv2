// CORE-11 模式查询 / DECRQM；CORE-12 选区；ENC-02 粘贴标记清洗
#include "itcore.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make(uint16_t cols = 10, uint16_t rows = 3, uint32_t sb = 100) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), cols, rows, sb, 1ull << 20};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static std::string feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    std::string reply;
    while (itcore_event *e = itcore_next_event(c)) {
        if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) {
            size_t n = 0; const uint8_t *d = itcore_event_data(e, &n);
            reply.append(reinterpret_cast<const char *>(d), n);
        }
        itcore_event_release(e);
    }
    return reply;
}
static std::string sel(itcore_core *c) {
    itcore_buffer *b = nullptr;
    if (itcore_selection_text(c, &b) != 0 || !b) return "<none>";
    std::string s(reinterpret_cast<char *>(b->data), b->length);
    itcore_buffer_release(b);
    return s;
}
static bool selected(itcore_core *c, int32_t first, uint16_t rows, uint16_t r, uint16_t col) {
    itcore_snapshot *s = itcore_acquire_snapshot(c, first, rows);
    itcore_wrap w; uint64_t rev;
    const itcore_cell *cells = itcore_snapshot_row(s, r, &w, &rev);
    bool on = cells && (cells[col].flags & ITCORE_CELL_SELECTED);
    itcore_snapshot_release(s);
    return on;
}

static void test_modes() {
    itcore_core *c = make();
    const int modes[] = {1, 25, 1000, 1002, 1003, 1006, 1049, 2004, 7, 9, 47, 1047};
    CHECK(itcore_mode_query(c, 25) == ITCORE_MODE_SET);
    CHECK(itcore_mode_query(c, 7) == ITCORE_MODE_SET);
    for (int m : {1, 1000, 1002, 1003, 1006, 1049, 2004, 9}) CHECK(itcore_mode_query(c, uint32_t(m)) == ITCORE_MODE_RESET);
    for (int m : modes) {
        std::string ms = std::to_string(m);
        if (m == 47 || m == 1047) continue;
        feed(c, "\x1b[?" + ms + "h");
        CHECK(itcore_mode_query(c, uint32_t(m)) == ITCORE_MODE_SET);
        CHECK(feed(c, "\x1b[?" + ms + "$p") == "\x1b[?" + ms + ";1$y");
        feed(c, "\x1b[?" + ms + "l");
        CHECK(itcore_mode_query(c, uint32_t(m)) == ITCORE_MODE_RESET);
        CHECK(feed(c, "\x1b[?" + ms + "$p") == "\x1b[?" + ms + ";2$y");
    }
    CHECK(itcore_mode_query(c, 4242) == ITCORE_MODE_NOT_RECOGNIZED);
    CHECK(feed(c, "\x1b[?4242$p") == "\x1b[?4242;0$y");
    CHECK(itcore_mode_query(nullptr, 1) == ITCORE_MODE_NOT_RECOGNIZED);
    itcore_destroy(c);
}

static void test_paste_markers() {
    itcore_core *c = make();
    itcore_buffer *b = nullptr;
    std::string evil = "a\x1b[201~rm -rf\x1b[2\x1b[200~01~b\xc2\x9b" "200~c";
    CHECK(itcore_encode_paste(c, evil.data(), evil.size(), &b) == 0);
    CHECK(std::string(reinterpret_cast<char *>(b->data), b->length) == "arm -rfbc");
    itcore_buffer_release(b);
    feed(c, "\x1b[?2004h");
    CHECK(itcore_encode_paste(c, "x\x1b[201~y", 8, &b) == 0);
    CHECK(std::string(reinterpret_cast<char *>(b->data), b->length) == "\x1b[200~xy\x1b[201~");
    itcore_buffer_release(b);
    itcore_destroy(c);
}

static void test_selection_basic() {
    itcore_core *c = make(10, 3);
    feed(c, "hello  \r\nworld");
    CHECK(!itcore_selection_active(c));
    CHECK(sel(c) == "<none>");
    CHECK(itcore_selection_start(c, 0, 1, 0) == 1);
    CHECK(itcore_selection_extend(c, 0, 2, 1) == 1);
    CHECK(itcore_selection_extend(c, 0, 2, 1) == 0);
    CHECK(sel(c) == "ello\nwor");  // 硬换行 + 行尾空白去除
    CHECK(selected(c, 0, 3, 0, 1) && selected(c, 0, 3, 1, 2) && !selected(c, 0, 3, 1, 3) && !selected(c, 0, 3, 0, 0));
    // 反向拖动
    itcore_selection_start(c, 0, 2, 1);
    itcore_selection_extend(c, 0, 1, 0);
    CHECK(sel(c) == "ello\nwor");
    // 拖到行尾之后
    itcore_selection_start(c, 0, 0, 0);
    itcore_selection_extend(c, 0, 9, 0);
    CHECK(sel(c) == "hello");
    itcore_selection_clear(c);
    CHECK(!itcore_selection_active(c) && !selected(c, 0, 3, 0, 0));
    itcore_destroy(c);
}

static void test_selection_wrap_wide() {
    itcore_core *c = make(4, 3);
    feed(c, "abcdef\r\n");           // 软换行
    itcore_selection_start(c, 0, 2, 0);
    itcore_selection_extend(c, 0, 1, 1);
    CHECK(sel(c) == "cdef");
    itcore_destroy(c);
    c = make(5, 3);
    feed(c, "a\xe4\xb8\xad" "b\xe6\x96\x87");  // a 中 b 文 → 文放不下，WIDE 换行
    itcore_selection_start(c, 0, 2, 0);          // 中 的尾格
    itcore_selection_extend(c, 0, 1, 1);         // 文 的尾格
    CHECK(sel(c) == "\xe4\xb8\xad" "b\xe6\x96\x87");
    CHECK(selected(c, 0, 3, 0, 1) && selected(c, 0, 3, 0, 2));
    itcore_destroy(c);
}

static void test_selection_follows_scroll_resize() {
    itcore_core *c = make(10, 3);
    feed(c, "one\r\ntwo\r\nthree");
    itcore_selection_start(c, 0, 0, 1);
    itcore_selection_extend(c, 0, 2, 1);
    CHECK(sel(c) == "two");
    feed(c, "\r\nfour\r\nfive");      // two 已进历史
    CHECK(sel(c) == "two");
    CHECK(selected(c, -2, 3, 1, 0) && !selected(c, -2, 3, 0, 0));  // 视口上滚 2 行：第 0 行 one，第 1 行 two
    itcore_resize(c, 6, 4, 1);
    CHECK(sel(c) == "two");
    itcore_resize(c, 2, 4, 2);       // 历史按新宽度重排：two → "tw" "o"
    CHECK(sel(c) == "two");
    // 选区跨历史与网格
    itcore_selection_start(c, -100, 0, 0);
    CHECK(!itcore_selection_active(c));
    itcore_destroy(c);

    // 历史裁剪：起点被淘汰时夹到最早一行
    c = make(10, 2, 2);
    feed(c, "l1\r\nl2\r\nl3");
    itcore_selection_start(c, -1, 0, 0);   // l1（历史）
    itcore_selection_extend(c, -1, 1, 2);  // l3
    CHECK(sel(c) == "l1\nl2\nl3");
    feed(c, "\r\nl4\r\nl5\r\nl6");       // 历史只留 2 行：l1 l2 被裁
    CHECK(sel(c) == "l3");
    itcore_destroy(c);
}

static void test_selection_alt_clear() {
    itcore_core *c = make(10, 3);
    feed(c, "abc");
    itcore_selection_start(c, 0, 0, 0);
    itcore_selection_extend(c, 0, 2, 0);
    feed(c, "\x1b[?1049h");
    CHECK(!itcore_selection_active(c));
    feed(c, "xyz");
    itcore_selection_start(c, 0, 0, 0);
    itcore_selection_extend(c, 0, 1, 0);
    CHECK(sel(c) == "xy");
    feed(c, "\x1b[?1049l");
    CHECK(!itcore_selection_active(c));
    itcore_selection_start(c, 0, 0, 0);
    feed(c, "\x1b" "c");
    CHECK(!itcore_selection_active(c));
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "modes") test_modes();
    if (w == "all" || w == "paste") test_paste_markers();
    if (w == "all" || w == "sel") {
        test_selection_basic();
        test_selection_wrap_wide();
        test_selection_follows_scroll_resize();
        test_selection_alt_clear();
    }
    if (failures) std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
