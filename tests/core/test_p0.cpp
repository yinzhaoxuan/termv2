// CORE-14..：核心未移植序列清单 P0 的测试向量（字符集、制表位、SGR 扩展、?5、DECALN、DECSTR、DECSCUSR、DEC DSR、模式）。
#include "itcore.h"
#include <cstdio>
#include <string>
#include <vector>

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
    // DECSTR 复位光标样式、?2026、?1004（1e239e1 评审）
    feed(c, "\x1b[5 q\x1b[?2026h\x1b[?1004h\x1b[!p");
    { auto *s = itcore_acquire_snapshot(c, 0, 1); int got = -1; itcore_snapshot_cursor_style(s, &got); CHECK(got == 0); itcore_snapshot_release(s); }
    CHECK(itcore_mode_query(c, 2026) == ITCORE_MODE_RESET);
    CHECK(itcore_mode_query(c, 1004) == ITCORE_MODE_RESET);
    // DECOM 下 DSR 6 相对滚动区上沿
    feed(c, "\x1b[2;3r\x1b[?6h\x1b[2;2H");
    CHECK(feed(c, "\x1b[6n") == "\x1b[2;2R");
    CHECK(feed(c, "\x1b[?6n") == "\x1b[?2;2R");
    itcore_destroy(c);
}

// CORE-19：itcore_export_text（重连旧 attempt 文本）：逻辑行、尾部截断不切断 UTF-8
static void export_text() {
    auto *c = make(10, 3);
    feed(c, "one  \r\n0123456789abc\r\n\xE4\xB8\xAD\xE6\x96\x87x\r\nlast");  // 软换行拼回逻辑行
    itcore_buffer *b = nullptr;
    CHECK(itcore_export_text(c, 1 << 20, &b) == 0 && b);
    std::string all((const char *)b->data, b->length); itcore_buffer_release(b);
    CHECK(all == "one\n0123456789abc\n\xE4\xB8\xAD\xE6\x96\x87x\nlast");
    for (size_t cap = 0; cap <= all.size(); ++cap) {
        CHECK(itcore_export_text(c, cap, &b) == 0 && b);
        std::string t((const char *)b->data, b->length); itcore_buffer_release(b);
        CHECK(t.size() <= cap);
        CHECK(all.compare(all.size() - t.size(), t.size(), t) == 0);  // 是尾部
        CHECK(t.empty() || (static_cast<unsigned char>(t[0]) & 0xC0) != 0x80);
    }
    feed(c, "\x1b[?1049hALT");  // 备用屏不影响：导出主屏
    CHECK(itcore_export_text(c, 1 << 20, &b) == 0 && b);
    CHECK(std::string((const char *)b->data, b->length) == all); itcore_buffer_release(b);
    CHECK(itcore_export_text(nullptr, 10, &b) == -1 && !b);
    itcore_destroy(c);
}

// 收集指定类型事件的负载
static std::vector<std::string> feed_ev(itcore_core *c, const std::string &s, int kind) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    std::vector<std::string> out;
    while (itcore_event *e = itcore_next_event(c)) {
        if (itcore_event_type(e) == kind) { size_t n; auto *d = itcore_event_data(e, &n); out.emplace_back((const char *)d, n); }
        itcore_event_release(e);
    }
    return out;
}

// CORE-20：DA2、CSI 18/14/16 t（像素尺寸由后端提供）、CSI 22/23 t 标题栈、OSC 1
static void winops() {
    auto *c = make(20, 5);
    CHECK(feed(c, "\x1b[>c") == "\x1b[>64;2500;0c");
    CHECK(feed(c, "\x1b[>0c") == "\x1b[>64;2500;0c");
    CHECK(feed(c, "\x1b[18t") == "\x1b[8;5;20t");
    CHECK(feed(c, "\x1b[14t") == "\x1b[4;80;160t");     // 尺寸未设：按 8x16 估算
    CHECK(feed(c, "\x1b[16t") == "\x1b[6;16;8t");
    itcore_set_cell_pixel_size(c, 9, 18);
    CHECK(feed(c, "\x1b[14t") == "\x1b[4;90;180t");
    CHECK(feed(c, "\x1b[16t") == "\x1b[6;18;9t");
    CHECK(feed(c, "\x1b[8;50;100t\x1b[21t\x1b[20t").empty());  // 改尺寸 / 回报标题：忽略
    CHECK((feed_ev(c, "\x1b]0;A\x07", ITCORE_EV_ICON_TITLE) == std::vector<std::string>{"A"}));
    CHECK((feed_ev(c, "\x1b]1;I\x07", ITCORE_EV_ICON_TITLE) == std::vector<std::string>{"I"}));
    CHECK(feed_ev(c, "\x1b]1;I2\x07", ITCORE_EV_TITLE).empty());   // OSC 1 不改窗口标题
    feed(c, "\x1b]2;W\x07\x1b]1;I\x07\x1b[22t\x1b]2;W2\x07\x1b]1;I2\x07\x1b[22;2t\x1b]2;W3\x07");
    CHECK((feed_ev(c, "\x1b[23;2t", ITCORE_EV_TITLE) == std::vector<std::string>{"W2"}));
    CHECK((feed_ev(c, "\x1b[23;2t", ITCORE_EV_TITLE) == std::vector<std::string>{"W"}));
    CHECK(feed_ev(c, "\x1b[23;2t", ITCORE_EV_TITLE).empty());   // 窗口标题栈已空
    feed(c, "\x1b]1;X\x07");
    CHECK((feed_ev(c, "\x1b[23t", ITCORE_EV_ICON_TITLE) == std::vector<std::string>{"I"}));  // 图标栈仍有一项
    CHECK(feed_ev(c, "\x1b[23t", ITCORE_EV_TITLE).empty());   // 空栈
    for (int i = 0; i < 15; ++i) feed(c, "\x1b]2;T" + std::to_string(i) + "\x07\x1b[22;2t");
    int pops = 0;
    while (!feed_ev(c, "\x1b[23;2t", ITCORE_EV_TITLE).empty()) ++pops;
    CHECK(pops == 10);                                        // 栈上限 10
    // RIS 清当前标题；之后 22t 压入空标题，23t 弹出为空
    feed(c, "\x1b]2;Before\x07\x1b]1;BeforeI\x07");
    CHECK((feed_ev(c, "\x1b" "c", ITCORE_EV_TITLE) == std::vector<std::string>{""}));
    CHECK((feed_ev(c, "\x1b[22t\x1b[23t", ITCORE_EV_TITLE) == std::vector<std::string>{""}));
    feed(c, "\x1b]1;Z\x07\x1b" "c");
    CHECK((feed_ev(c, "\x1b[22t\x1b[23t", ITCORE_EV_ICON_TITLE) == std::vector<std::string>{""}));
    itcore_destroy(c);
}

// CORE-21：OSC 4/10/11/12/104/110–112 与配置色
static void colors() {
    auto *c = make();
    // 应答结尾符与请求一致
    CHECK(feed(c, "\x1b]10;?\x07") == "\x1b]10;rgb:c7c7/c7c7/c7c7\x07");
    CHECK(feed(c, "\x1b]10;?\x1b\\") == "\x1b]10;rgb:c7c7/c7c7/c7c7\x1b\\");
    CHECK(feed(c, "\x1b]4;1;?\x07") == "\x1b]4;1;rgb:cdcd/0000/0000\x07");
    itcore_colors d; itcore_default_colors(&d);
    CHECK(d.palette[1] == 0xCD0000 && d.palette[16] == 0 && d.palette[231] == 0xFFFFFF && d.palette[232] == 0x080808 && d.palette[255] == 0xEEEEEE);
    itcore_colors cfg = d; cfg.foreground = 0x112233; cfg.background = 0x445566; cfg.palette[2] = 0x010203;
    CHECK(itcore_set_colors(c, &cfg) == 0);
    CHECK(feed(c, "\x1b]10;?\x07") == "\x1b]10;rgb:1111/2222/3333\x07");
    CHECK(feed(c, "\x1b]11;?\x07") == "\x1b]11;rgb:4444/5555/6666\x07");
    CHECK(feed(c, "\x1b]10;?;?\x07") == "\x1b]10;rgb:1111/2222/3333\x07\x1b]11;rgb:4444/5555/6666\x07");
    CHECK(feed(c, "\x1b]4;2;?\x07") == "\x1b]4;2;rgb:0101/0202/0303\x07");
    auto s0 = itcore_acquire_snapshot(c, 0, 1); itcore_wrap w; uint64_t r0; itcore_snapshot_row(s0, 0, &w, &r0);
    feed(c, "\x1b]4;2;#ff8000;3;rgb:f/80/0000\x07\x1b]10;#abc\x07\x1b]11;rgb:ffff/0/0\x07\x1b]12;#010203\x07\x1b]4;300;#fff\x07\x1b]10;bogus\x07");
    auto s1 = itcore_acquire_snapshot(c, 0, 1); uint64_t r1; itcore_snapshot_row(s1, 0, &w, &r1);
    CHECK(r1 > r0);
    itcore_colors k; itcore_snapshot_colors(s1, &k);
    CHECK(k.palette[2] == 0xFF8000 && k.palette[3] == 0xFF8000);
    CHECK(k.foreground == 0xAABBCC && k.background == 0xFF0000 && k.cursor == 0x010203);
    itcore_colors k0; itcore_snapshot_colors(s0, &k0);
    CHECK(k0.foreground == 0x112233 && k0.palette[2] == 0x010203);  // 旧快照不变
    itcore_snapshot_release(s0); itcore_snapshot_release(s1);
    feed(c, "\x1b]104;2\x07\x1b]110\x07\x1b]112\x07");
    auto s2 = itcore_acquire_snapshot(c, 0, 1); itcore_snapshot_colors(s2, &k); itcore_snapshot_release(s2);
    CHECK(k.palette[2] == 0x010203 && k.palette[3] == 0xFF8000 && k.foreground == 0x112233 && k.background == 0xFF0000 && k.cursor == cfg.cursor);
    feed(c, "\x1b]104\x07\x1b]111\x07");
    auto s3 = itcore_acquire_snapshot(c, 0, 1); itcore_snapshot_colors(s3, &k); itcore_snapshot_release(s3);
    CHECK(k.palette[3] == d.palette[3] && k.background == 0x445566);
    feed(c, "\x1b]4;5;#123456\x07\x1b" "c");                       // RIS 恢复配置
    auto s4 = itcore_acquire_snapshot(c, 0, 1); itcore_snapshot_colors(s4, &k); itcore_snapshot_release(s4);
    CHECK(k.palette[5] == d.palette[5]);
    itcore_colors bad{}; CHECK(itcore_set_colors(c, &bad) == -1);
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "charset") charsets();
    if (w == "all" || w == "tabs") tabs();
    if (w == "all" || w == "sgr") sgr();
    if (w == "all" || w == "modes") modes();
    if (w == "all" || w == "misc") misc();
    if (w == "all" || w == "export") export_text();
    if (w == "all" || w == "winops") winops();
    if (w == "all" || w == "colors") colors();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("ok");
    return 0;
}
