// SPDX-License-Identifier: GPL-3.0-or-later
// CORE-22 / CORE-23：GAP-14 SGR 子参数，GAP-16 DA / XTVERSION / DECRQSS / XTGETTCAP。
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
static std::string feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    std::string out;
    while (itcore_event *e = itcore_next_event(c)) {
        if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) {
            size_t n; auto *d = itcore_event_data(e, &n); out.append(reinterpret_cast<const char *>(d), n);
        }
        itcore_event_release(e);
    }
    return out;
}
static itcore_cell cell0(itcore_core *c) {
    auto *s = itcore_acquire_snapshot(c, 0, 1);
    itcore_wrap w; uint64_t r;
    itcore_cell cell = itcore_snapshot_row(s, 0, &w, &r)[0];
    itcore_snapshot_release(s);
    return cell;
}
static itcore_cell paint(itcore_core *c, const std::string &seq) {
    feed(c, "\x1b" "c");
    feed(c, seq + "X");
    return cell0(c);
}
static int ul_style(uint16_t f) {
    return (f & ITCORE_CELL_UL_STYLE_MASK) >> ITCORE_CELL_UL_STYLE_SHIFT;
}
static constexpr uint32_t kRed = 0x02FF0000u;
static constexpr uint32_t kGreen = 0x0200FF00u;
static constexpr uint32_t kIdx(uint8_t i) { return 0x01000000u | i; }

// 冒号形式的后续分号参数仍是独立 SGR：38:2:1 分量不足，不吞掉后面的 1。
// 完整的 38;2;R;G;B 之后的 0 是真正的 SGR 0，不是泄漏。
static void core22() {
    auto *c = make();
    auto a = paint(c, "\x1b[38;2;255;0;0m");
    CHECK(a.fg == kRed && a.flags == 0);
    a = paint(c, "\x1b[38:2:255:0:0m");
    CHECK(a.fg == kRed);
    a = paint(c, "\x1b[38:2::255:0:0m");
    CHECK(a.fg == kRed);
    a = paint(c, "\x1b[38:2:0:255:0:0m");
    CHECK(a.fg == kRed);
    a = paint(c, "\x1b[48:2::0:0:255m");
    CHECK(a.bg == 0x020000FFu);
    a = paint(c, "\x1b[38;5;196m");
    CHECK(a.fg == kIdx(196));
    a = paint(c, "\x1b[48:5:200m");
    CHECK(a.bg == kIdx(200));
    a = paint(c, "\x1b[48;5;7m");
    CHECK(a.bg == kIdx(7));
    a = paint(c, "\x1b[1;3;7;38;5;196;48:5:2m");
    CHECK((a.flags & ITCORE_CELL_BOLD) && (a.flags & ITCORE_CELL_ITALIC) && (a.flags & ITCORE_CELL_INVERSE));
    CHECK(a.fg == kIdx(196) && a.bg == kIdx(2));

    a = paint(c, "\x1b[4;58;2;0;255;0m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && ul_style(a.flags) == ITCORE_UL_SINGLE);
    CHECK(a.underline_color == kGreen);
    CHECK((a.flags & ITCORE_CELL_DIM) == 0);
    a = paint(c, "\x1b[4;58:2::0:255:0m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && a.underline_color == kGreen);
    a = paint(c, "\x1b[4;58;5;196m");
    CHECK(a.underline_color == kIdx(196) && (a.flags & ITCORE_CELL_UNDERLINE));
    a = paint(c, "\x1b[4;58;2;0;255;0;59m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && a.underline_color == 0);

    a = paint(c, "\x1b[4m\x1b[4:0m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) == 0 && ul_style(a.flags) == 0);
    a = paint(c, "\x1b[4:0m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) == 0);
    for (int st = 1; st <= 5; ++st) {
        a = paint(c, "\x1b[4:" + std::to_string(st) + "m");
        CHECK((a.flags & ITCORE_CELL_UNDERLINE) && ul_style(a.flags) == st);
    }
    a = paint(c, "\x1b[21m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && ul_style(a.flags) == ITCORE_UL_DOUBLE);
    a = paint(c, "\x1b[4:3;4:9m");
    CHECK(ul_style(a.flags) == ITCORE_UL_WAVY && (a.flags & ITCORE_CELL_UNDERLINE));
    a = paint(c, "\x1b[4:3m\x1b[24m");
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) == 0 && ul_style(a.flags) == 0);

    a = paint(c, "\x1b[53m");
    CHECK(a.flags & ITCORE_CELL_OVERLINE);
    a = paint(c, "\x1b[53m\x1b[55m");
    CHECK((a.flags & ITCORE_CELL_OVERLINE) == 0);
    a = paint(c, "\x1b[4;53m\x1b[24m");
    CHECK((a.flags & ITCORE_CELL_OVERLINE) && (a.flags & ITCORE_CELL_UNDERLINE) == 0);
    a = paint(c, "\x1b[1;4:3;53;38:2::255:0:0;58:2::0:255:0m\x1b[0m");
    CHECK(a.flags == 0 && a.fg == 0 && a.bg == 0 && a.underline_color == 0);

    a = paint(c, "\x1b[38;2;1m");
    CHECK(a.fg == 0 && a.flags == 0);
    a = paint(c, "\x1b[1;38;2;1m");
    CHECK(a.flags == ITCORE_CELL_BOLD && a.fg == 0);
    a = paint(c, "\x1b[38;2m");
    CHECK(a.fg == 0 && a.flags == 0);
    a = paint(c, "\x1b[38;5m");
    CHECK(a.fg == 0 && (a.flags & ITCORE_CELL_BLINK) == 0);
    a = paint(c, "\x1b[58;2;1;2m");
    CHECK(a.underline_color == 0 && a.flags == 0);
    a = paint(c, "\x1b[38:2:1;1m");
    CHECK(a.fg == 0 && a.flags == ITCORE_CELL_BOLD);
    a = paint(c, "\x1b[38;2;255;0;0;4m");
    CHECK(a.fg == kRed && (a.flags & ITCORE_CELL_UNDERLINE));
    a = paint(c, "\x1b[1;38;2;255;0;0m");
    CHECK(a.fg == kRed && (a.flags & ITCORE_CELL_BOLD));

    feed(c, "\x1b" "c");
    feed(c, "\x1b[4;58;2;0;255;0m\x1b" "7\x1b[0m\x1b" "8X");
    a = cell0(c);
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && a.underline_color == kGreen);
    feed(c, "\x1b" "c");
    feed(c, "\x1b[4;58:2::10:20:30m\x1b[?1049h\x1b[0m\x1b[?1049lX");
    a = cell0(c);
    CHECK((a.flags & ITCORE_CELL_UNDERLINE) && a.underline_color == 0x020A141Eu);
    itcore_destroy(c);
}

static void core23() {
    auto *c = make();
    CHECK(feed(c, "\x1b[c") == "\x1b[?62;22c");
    CHECK(feed(c, "\x1b[0c") == "\x1b[?62;22c");
    CHECK(feed(c, "\x1b[>c") == "\x1b[>1;0;0c");
    CHECK(feed(c, "\x1b[>0c") == "\x1b[>1;0;0c");
    CHECK(feed(c, "\x1b[>1c").empty());
    CHECK(feed(c, "\x1b[=c") == "\x1bP!|7465726d7632\x1b\\");
    CHECK(feed(c, "\x1b[=0c") == "\x1bP!|7465726d7632\x1b\\");
    CHECK(feed(c, "\x1b[=1c").empty());
    CHECK(feed(c, "\x1b[>q") == "\x1bP>|termv2 0.1\x1b\\");
    CHECK(feed(c, "\x1b[>0q") == "\x1bP>|termv2 0.1\x1b\\");
    CHECK(feed(c, "\x1b[>1q").empty());
    CHECK(feed(c, "\x1b[?69$p") == "\x1b[?69;0$y");

    CHECK(feed(c, "\x1bP$qm\x1b\\") == "\x1bP1$r0m\x1b\\");
    CHECK(feed(c, "\x1b[1;4:3;53;38:2:255:0:0;48:5:9;58:5:196m\x1bP$qm\x1b\\")
          == "\x1bP1$r0;1;4:3;53;38:2:255:0:0;101;58:5:196m\x1b\\");
    CHECK(feed(c, "\x1b[0;31m\x1bP$qm\x1b\\") == "\x1bP1$r0;31m\x1b\\");
    CHECK(feed(c, "\x1b[0;21m\x1bP$qm\x1b\\") == "\x1bP1$r0;21m\x1b\\");
    CHECK(feed(c, "\x1b[0;4:4m\x1bP$qm\x1b\\") == "\x1bP1$r0;4:4m\x1b\\");
    CHECK(feed(c, "\x1b[0;4:5m\x1bP$qm\x1b\\") == "\x1bP1$r0;4:5m\x1b\\");
    CHECK(feed(c, "\x1b[0;7;31m\x1bP$qm\x1b\\") == "\x1bP1$r0;7;31m\x1b\\");
    CHECK(feed(c, "\x1b" "c\x1bP$qr\x1b\\") == "\x1bP1$r1;5r\x1b\\");
    CHECK(feed(c, "\x1b[2;4r\x1bP$qr\x1b\\") == "\x1bP1$r2;4r\x1b\\");
    CHECK(feed(c, "\x1b[3 q\x1bP$q q\x1b\\") == "\x1bP1$r3 q\x1b\\");
    CHECK(feed(c, "\x1bP$q\"q\x1b\\") == "\x1bP1$r0\"q\x1b\\");
    CHECK(feed(c, "\x1bP$q\x1b\\") == "\x1bP0$r\x1b\\");
    CHECK(feed(c, "\x1bP$qzzzz\x1b\\") == "\x1bP0$r\x1b\\");

    CHECK(feed(c, "\x1bP+q544e\x1b\\") == "\x1bP1+r544e=7465726d7632\x1b\\");
    CHECK(feed(c, "\x1bP+q544E\x1b\\") == "\x1bP1+r544e=7465726d7632\x1b\\");
    CHECK(feed(c, "\x1bP+q6e616d65\x1b\\") == "\x1bP1+r6e616d65=7465726d7632\x1b\\");
    CHECK(feed(c, "\x1bP+q436f\x1b\\") == "\x1bP1+r436f=323536\x1b\\");
    CHECK(feed(c, "\x1bP+q636f6c6f7273\x1b\\") == "\x1bP1+r636f6c6f7273=323536\x1b\\");
    CHECK(feed(c, "\x1bP+q524742\x1b\\") == "\x1bP1+r524742=38\x1b\\");
    CHECK(feed(c, "\x1bP+q7878\x1b\\") == "\x1bP0+r\x1b\\");
    CHECK(feed(c, "\x1bP+q746e\x1b\\") == "\x1bP0+r\x1b\\");
    CHECK(feed(c, "\x1bP+q544\x1b\\") == "\x1bP0+r\x1b\\");
    CHECK(feed(c, "\x1bP+q544e;7878\x1b\\") == "\x1bP1+r544e=7465726d7632\x1b\\\x1bP0+r\x1b\\");
    CHECK(feed(c, "\x1bP+q\x1b\\") == "\x1bP0+r\x1b\\");
    CHECK(feed(c, "\x1bPq\x1b\\").empty());
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "core22") core22();
    if (w == "all" || w == "core23") core23();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("ok");
    return 0;
}
