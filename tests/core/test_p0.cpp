// CORE-14..：核心未移植序列清单 P0 的测试向量（字符集、制表位、SGR 扩展、?5、DECALN、DECSTR、DECSCUSR、DEC DSR、模式）。
#include "itcore.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make(uint16_t cols = 20, uint16_t rows = 5) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), cols, rows, 100, 1u << 20};
    itcore_core *c = nullptr;
    itcore_create(&o, &c);
    return c;
}
// 喂入并返回所有 REPLY_BYTES 拼接
static std::string feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    std::string out;
    while (itcore_event *e = itcore_next_event(c)) {
        if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) { size_t n; auto *d = itcore_event_data(e, &n); out.append((const char *)d, n); }
        itcore_event_release(e);
    }
    return out;
}
static std::string text_at(itcore_core *c, int row, int col) {
    auto *s = itcore_acquire_snapshot(c, 0, 0xFFFF);
    itcore_wrap w; uint64_t r;
    const itcore_cell *cells = itcore_snapshot_row(s, uint16_t(row), &w, &r);
    size_t n = 0; const char *g = itcore_snapshot_grapheme(s, cells[col].grapheme_id, &n);
    std::string out(g, n);
    itcore_snapshot_release(s);
    return out;
}
static uint16_t flags_at(itcore_core *c, int row, int col) {
    auto *s = itcore_acquire_snapshot(c, 0, 5);
    itcore_wrap w; uint64_t r;
    uint16_t f = itcore_snapshot_row(s, uint16_t(row), &w, &r)[col].flags;
    itcore_snapshot_release(s);
    return f;
}
static int cursor_col(itcore_core *c) {
    auto *s = itcore_acquire_snapshot(c, 0, 5); uint16_t x, y; int v;
    itcore_snapshot_cursor(s, &x, &y, &v); itcore_snapshot_release(s); return x;
}

// CORE-14：SCS / SO / SI，DEC 特殊图形
static void charsets() {
    auto *c = make();
    feed(c, "\x1b(0lqk\x1b(Bq");
    CHECK(text_at(c, 0, 0) == "\xE2\x94\x8C");  // ┌
    CHECK(text_at(c, 0, 1) == "\xE2\x94\x80");  // ─
    CHECK(text_at(c, 0, 2) == "\xE2\x94\x90");  // ┐
    CHECK(text_at(c, 0, 3) == "q");
    feed(c, "\r\n\x1b)0a\x0ex\x0fx");            // G1=DEC，SO 切入，SI 切回
    CHECK(text_at(c, 1, 0) == "a");
    CHECK(text_at(c, 1, 1) == "\xE2\x94\x82");  // │
    CHECK(text_at(c, 1, 2) == "x");
    // DECSC/DECRC 保存字符集
    feed(c, "\r\n\x1b(0\x1b" "7\x1b(B\x1b" "8x");
    CHECK(text_at(c, 2, 0) == "\xE2\x94\x82");
    feed(c, "\x1b" "c" "x");                         // RIS 复位
    CHECK(text_at(c, 0, 0) == "x");
    itcore_destroy(c);
}

// CORE-15：HTS / TBC / CHT / CBT
static void tabs() {
    auto *c = make(40, 3);
    feed(c, "\t"); CHECK(cursor_col(c) == 8);
    feed(c, "\r\x1b[3g\x1b[5G\x1bH\x1b[12G\x1bH\r\t"); CHECK(cursor_col(c) == 4);
    feed(c, "\t"); CHECK(cursor_col(c) == 11);
    feed(c, "\t"); CHECK(cursor_col(c) == 39);     // 无更多制表位 → 末列
    feed(c, "\x1b[Z"); CHECK(cursor_col(c) == 11);
    feed(c, "\x1b[2Z"); CHECK(cursor_col(c) == 0);
    feed(c, "\x1b[2I"); CHECK(cursor_col(c) == 11);
    feed(c, "\x1b[g\r\t"); CHECK(cursor_col(c) == 4);  // TBC 0 清当前（11）
    feed(c, "\t"); CHECK(cursor_col(c) == 39);
    feed(c, "\x1b" "c\x1b[3I"); CHECK(cursor_col(c) == 24);  // RIS 恢复每 8 列
    itcore_destroy(c);
}

// CORE-16：SGR 2/5/8/9 与 22/25/28/29
static void sgr() {
    auto *c = make();
    feed(c, "\x1b[1;2mA\x1b[22mB\x1b[5mC\x1b[25mD\x1b[8mE\x1b[28mF\x1b[9mG\x1b[29mH\x1b[6mI\x1b[0mJ\x1b[2;5;8;9mK");
    CHECK(flags_at(c, 0, 0) == (ITCORE_CELL_BOLD | ITCORE_CELL_DIM));
    CHECK(flags_at(c, 0, 1) == 0);
    CHECK(flags_at(c, 0, 2) == ITCORE_CELL_BLINK);
    CHECK(flags_at(c, 0, 3) == 0);
    CHECK(flags_at(c, 0, 4) == ITCORE_CELL_HIDDEN);
    CHECK(flags_at(c, 0, 5) == 0);
    CHECK(flags_at(c, 0, 6) == ITCORE_CELL_STRIKE);
    CHECK(flags_at(c, 0, 7) == 0);
    CHECK(flags_at(c, 0, 8) == ITCORE_CELL_BLINK);
    CHECK(flags_at(c, 0, 9) == 0);
    CHECK(flags_at(c, 0, 10) == (ITCORE_CELL_DIM | ITCORE_CELL_BLINK | ITCORE_CELL_HIDDEN | ITCORE_CELL_STRIKE));
    // 位不与既有标志冲突
    unsigned ext = ITCORE_CELL_DIM | ITCORE_CELL_BLINK | ITCORE_CELL_HIDDEN | ITCORE_CELL_STRIKE;
    unsigned old = ITCORE_CELL_CONTINUATION | ITCORE_CELL_BOLD | ITCORE_CELL_ITALIC | ITCORE_CELL_UNDERLINE | ITCORE_CELL_INVERSE | ITCORE_CELL_SELECTED;
    CHECK((ext & old) == 0 && ext == 0x3C0u);
    itcore_destroy(c);
}

// CORE-17：?5 DECSCNM（查询 + 快照字段 + 全行 revision 推进）、DECSCUSR 快照、?1004/?2026 模式
static void modes() {
    auto *c = make();
    feed(c, "abc");
    auto *s0 = itcore_acquire_snapshot(c, 0, 5);
    uint64_t r0[5]; itcore_wrap w;
    for (int y = 0; y < 5; ++y) itcore_snapshot_row(s0, uint16_t(y), &w, &r0[y]);
    CHECK(itcore_snapshot_reverse_screen(s0) == 0);
    CHECK(itcore_mode_query(c, 5) == ITCORE_MODE_RESET);
    feed(c, "\x1b[?5h");
    CHECK(itcore_mode_query(c, 5) == ITCORE_MODE_SET);
    auto *s1 = itcore_acquire_snapshot(c, 0, 5);
    CHECK(itcore_snapshot_reverse_screen(s1) == 1);
    for (int y = 0; y < 5; ++y) { uint64_t r; itcore_snapshot_row(s1, uint16_t(y), &w, &r); CHECK(r > r0[y]); }
    CHECK(feed(c, "\x1b[?5$p") == "\x1b[?5;1$y");
    feed(c, "\x1b[?5l");
    CHECK(itcore_mode_query(c, 5) == ITCORE_MODE_RESET);
    itcore_snapshot_release(s0); itcore_snapshot_release(s1);

    for (int ps = 0; ps <= 6; ++ps) {
        feed(c, "\x1b[" + std::to_string(ps) + " q");
        auto *s = itcore_acquire_snapshot(c, 0, 1); int got = -1;
        itcore_snapshot_cursor_style(s, &got); CHECK(got == ps); itcore_snapshot_release(s);
    }
    feed(c, "\x1b[9 q");  // 越界忽略
    { auto *s = itcore_acquire_snapshot(c, 0, 1); int got = -1; itcore_snapshot_cursor_style(s, &got); CHECK(got == 6); itcore_snapshot_release(s); }
    feed(c, "\x1b" "c");
    { auto *s = itcore_acquire_snapshot(c, 0, 1); int got = -1; itcore_snapshot_cursor_style(s, &got); CHECK(got == 0); itcore_snapshot_release(s); }

    for (int m : {1004, 2026}) {
        CHECK(itcore_mode_query(c, m) == ITCORE_MODE_RESET);
        feed(c, "\x1b[?" + std::to_string(m) + "h");
        CHECK(itcore_mode_query(c, m) == ITCORE_MODE_SET);
        CHECK(feed(c, "\x1b[?" + std::to_string(m) + "$p") == "\x1b[?" + std::to_string(m) + ";1$y");
        feed(c, "\x1b[?" + std::to_string(m) + "l");
        CHECK(itcore_mode_query(c, m) == ITCORE_MODE_RESET);
    }
    itcore_destroy(c);
}

// CORE-18：DECALN、DECSTR、DEC 私有 DSR（原 screen.cpp:524 解析后不执行）
static void misc() {
    auto *c = make(4, 3);
    feed(c, "\x1b[2;3r\x1b[?6h\x1b#8");
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) CHECK(text_at(c, y, x) == "E");
    CHECK(feed(c, "\x1b[6n") == "\x1b[1;1R");     // 原点模式已关、光标在原点
    feed(c, "\x1b[2;3r\x1b[3;3H\x1b[4h\x1b[?25l\x1b[1;7m\x1b(0\x1b[!p");  // DECSTBM 会归位，先设区域
    CHECK(itcore_ansi_mode_query(c, 4) == ITCORE_MODE_RESET);
    CHECK(itcore_mode_query(c, 25) == ITCORE_MODE_SET);
    CHECK(feed(c, "\x1b[6n") == "\x1b[3;3R");     // DECSTR 不动光标
    feed(c, "q");
    CHECK(text_at(c, 2, 2) == "q" && flags_at(c, 2, 2) == 0);
    feed(c, "\x1b[3;1H\n");                        // 滚动区已复位：在最后一行换行会滚屏
    CHECK(text_at(c, 1, 2) == "q");
    CHECK(feed(c, "\x1b[2;4H\x1b[?6n") == "\x1b[?2;4R");
    CHECK(feed(c, "\x1b[?15n") == "\x1b[?13n");
    CHECK(feed(c, "\x1b[?25n") == "\x1b[?20n");
    CHECK(feed(c, "\x1b[?26n") == "\x1b[?27;1;0;0n");
    CHECK(feed(c, "\x1b[?55n") == "\x1b[?53n");
    CHECK(feed(c, "\x1b[?99n").empty());
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "charset") charsets();
    if (w == "all" || w == "tabs") tabs();
    if (w == "all" || w == "sgr") sgr();
    if (w == "all" || w == "modes") modes();
    if (w == "all" || w == "misc") misc();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("ok");
    return 0;
}
