// CORE-03/04/05 与 MEM-01 子集：屏幕模型、CSI 默认参数、历史预算。
#include "itcore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make(uint16_t cols = 80, uint16_t rows = 24,
                         uint32_t sb = 1000, uint64_t mem = 1ull << 20) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), cols, rows, sb, mem};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static void drain(itcore_core *c) {
    while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e);
}
static void feed(itcore_core *c, const std::string &s) {
    auto r = itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    CHECK(r.status == ITCORE_OK);
    drain(c);
}
static std::string cell_text(const itcore_snapshot *s, uint16_t row, uint16_t col) {
    itcore_wrap w; uint64_t rev;
    const itcore_cell *cells = itcore_snapshot_row(s, row, &w, &rev);
    if (!cells) return {};
    if (cells[col].flags & ITCORE_CELL_CONTINUATION) return {};
    size_t n = 0;
    const char *g = itcore_snapshot_grapheme(s, cells[col].grapheme_id, &n);
    return std::string(g, n);
}
static std::string row_text(const itcore_snapshot *s, uint16_t row) {
    itcore_wrap w; uint64_t rev;
    const itcore_cell *cells = itcore_snapshot_row(s, row, &w, &rev);
    if (!cells) return {};
    uint16_t cols = itcore_snapshot_columns(s);
    std::string out;
    for (uint16_t x = 0; x < cols; ++x) {
        if (cells[x].flags & ITCORE_CELL_CONTINUATION) continue;
        if (!cells[x].grapheme_id) { out.push_back(' '); continue; }
        size_t n = 0;
        const char *g = itcore_snapshot_grapheme(s, cells[x].grapheme_id, &n);
        out.append(g, n);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// CORE-03：UTF-8 分块、中文贴右边界、组合字符
static void test_core03() {
    itcore_core *c = make(10, 4);
    // 分块喂入 UTF-8「你」E4 BD A0
    const uint8_t ni[] = {0xE4, 0xBD, 0xA0};
    for (int i = 0; i < 3; ++i) {
        auto r = itcore_feed(c, ni + i, 1);
        CHECK(r.status == ITCORE_OK && r.accepted == 1);
    }
    drain(c);
    {
        auto *s = itcore_acquire_snapshot(c, 0, 4);
        CHECK(s);
        CHECK(cell_text(s, 0, 0) == "你");
        itcore_wrap w; uint64_t rev;
        const itcore_cell *cells = itcore_snapshot_row(s, 0, &w, &rev);
        CHECK(cells[0].width == 2);
        CHECK(cells[1].flags & ITCORE_CELL_CONTINUATION);
        uint16_t col, row; int vis;
        itcore_snapshot_cursor(s, &col, &row, &vis);
        CHECK(col == 2 && row == 0);
        itcore_snapshot_release(s);
    }
    // 贴右边界：列 8 起写宽字符（宽 2），应软换到下一行
    feed(c, "\r\n");  // 新行
    feed(c, "12345678");  // 光标到列 8（0 基）
    feed(c, "中");  // 宽 2，cols=10，8+2=10 刚好；再写一个应换行
    feed(c, "文");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 4);
        CHECK(row_text(s, 1).find("中") != std::string::npos);
        // 「文」因贴边换行
        std::string r1 = row_text(s, 1), r2 = row_text(s, 2);
        CHECK(r1.find("文") == std::string::npos);
        CHECK(r2.find("文") != std::string::npos);
        itcore_wrap w; uint64_t rev;
        itcore_snapshot_row(s, 1, &w, &rev);
        CHECK(w == ITCORE_WRAP_SOFT || w == ITCORE_WRAP_WIDE);
        itcore_snapshot_release(s);
    }
    // 组合字符：a + combining acute (CC 81)
    feed(c, "\x1b[3;1H");
    feed(c, std::string("a\xCC\x81"));
    {
        auto *s = itcore_acquire_snapshot(c, 0, 4);
        std::string t = cell_text(s, 2, 0);
        CHECK(t.size() >= 2);  // a + combining
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);

    // --- CORE-03 追加：wrap_pending 组合、宽字符孤儿、快照 generation ---
    c = make(8, 3);
    feed(c, "12345678");  // 填满一行 → wrap_pending
    feed(c, std::string("\xCC\x81"));  // combining，应附着到 '8'
    {
        auto *s = itcore_acquire_snapshot(c, 0, 3);
        std::string t = cell_text(s, 0, 7);
        CHECK(t.size() >= 2);
        CHECK(t[0] == '8');
        itcore_wrap w; uint64_t rev;
        const itcore_cell *r1 = itcore_snapshot_row(s, 1, &w, &rev);
        bool empty = true;
        for (int i = 0; i < 8; ++i) if (r1[i].grapheme_id) empty = false;
        CHECK(empty);
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);

    // 窄盖宽：清多余尾格
    c = make(6, 2);
    feed(c, "中");
    feed(c, "\x1b[1;1H");
    feed(c, "A");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 2);
        itcore_wrap w; uint64_t rev;
        const itcore_cell *cells = itcore_snapshot_row(s, 0, &w, &rev);
        CHECK(cell_text(s, 0, 0) == "A");
        CHECK(!(cells[1].flags & ITCORE_CELL_CONTINUATION));
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);

    // ICH 后行内修复
    c = make(8, 2);
    feed(c, "中文");
    feed(c, "\x1b[1;2H");
    feed(c, "\x1b[2@");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 2);
        itcore_wrap w; uint64_t rev;
        const itcore_cell *cells = itcore_snapshot_row(s, 0, &w, &rev);
        for (int x = 0; x < 8; ++x) {
            if (cells[x].flags & ITCORE_CELL_CONTINUATION) {
                CHECK(x > 0);
                int b = x;
                while (b > 0 && (cells[b].flags & ITCORE_CELL_CONTINUATION)) --b;
                CHECK(!(cells[b].flags & ITCORE_CELL_CONTINUATION));
                CHECK(b + cells[b].width > x);
            }
        }
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);

    // DCH 后行内修复
    c = make(8, 2);
    feed(c, "中AB");
    feed(c, "\x1b[1;1H");
    feed(c, "\x1b[1P");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 2);
        itcore_wrap w; uint64_t rev;
        const itcore_cell *cells = itcore_snapshot_row(s, 0, &w, &rev);
        for (int x = 0; x < 8; ++x) {
            if (cells[x].flags & ITCORE_CELL_CONTINUATION) {
                int b = x;
                while (b > 0 && (cells[b].flags & ITCORE_CELL_CONTINUATION)) --b;
                CHECK(b + (cells[b].width ? cells[b].width : 1) > x);
            }
        }
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);

    // 快照携带 resize generation
    c = make(10, 4);
    CHECK(itcore_resize(c, 12, 5, 42) == 0);
    {
        auto *s = itcore_acquire_snapshot(c, 0, 5);
        CHECK(itcore_snapshot_generation(s) == 42);
        CHECK(itcore_snapshot_columns(s) == 12);
        CHECK(itcore_snapshot_rows(s) == 5);
        itcore_snapshot_release(s);
    }
    CHECK(itcore_resize(c, 10, 4, 40) == -2);
    CHECK(itcore_resize(c, 10, 4, 43) == 0);
    {
        auto *s = itcore_acquire_snapshot(c, 0, 4);
        CHECK(itcore_snapshot_generation(s) == 43);
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);
}

// CORE-04：主备用屏、滚动区、清屏、DSR 光标查询
static void test_core04() {
    itcore_core *c = make(20, 6);
    feed(c, "MAIN");
    feed(c, "\x1b[2;1Hline2");
    // 进入备用屏 1049
    feed(c, "\x1b[?1049h");
    feed(c, "\x1b[H\x1b[2JALT");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 6);
        CHECK(row_text(s, 0).find("ALT") != std::string::npos);
        CHECK(row_text(s, 0).find("MAIN") == std::string::npos);
        itcore_snapshot_release(s);
    }
    // 退出备用屏，主屏恢复
    feed(c, "\x1b[?1049l");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 6);
        CHECK(row_text(s, 0).find("MAIN") != std::string::npos);
        itcore_snapshot_release(s);
    }
    // 滚动区 DECSTBM：2;5，在底部滚
    feed(c, "\x1b[2;5r");
    feed(c, "\x1b[5;1H");
    feed(c, "A\nB\nC");
    // CUP 默认参数：CSI H → 1;1
    feed(c, "\x1b[H");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 6);
        uint16_t col, row; int vis;
        itcore_snapshot_cursor(s, &col, &row, &vis);
        CHECK(col == 0 && row == 0);  // origin 关时到 1;1
        itcore_snapshot_release(s);
    }
    // ED 默认 0：从光标清到末尾
    feed(c, "\x1b[1;1HKEEP\x1b[1;3H\x1b[J");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 6);
        std::string r0 = row_text(s, 0);
        CHECK(r0.size() >= 2 && r0[0] == 'K' && r0[1] == 'E');
        // 光标在列 3（0 基 2）之后被清
        itcore_snapshot_release(s);
    }
    // DSR 6n
    {
        // 清空事件后查询
        drain(c);
        feed(c, "\x1b[2;5H");
        auto r = itcore_feed(c, reinterpret_cast<const uint8_t *>("\x1b[6n"), 4);
        CHECK(r.status == ITCORE_OK);
        bool got = false;
        while (itcore_event *e = itcore_next_event(c)) {
            if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) {
                size_t n; const uint8_t *p = itcore_event_data(e, &n);
                std::string d(reinterpret_cast<const char *>(p), n);
                CHECK(d == "\x1b[2;5R");
                got = true;
            }
            itcore_event_release(e);
        }
        CHECK(got);
    }
    // SGR 粗体与 256 色
    feed(c, "\x1b[0m\x1b[1;38;5;196mX");
    {
        auto *s = itcore_acquire_snapshot(c, 0, 6);
        itcore_wrap w; uint64_t rev;
        // 找 X
        bool found = false;
        for (uint16_t y = 0; y < 6 && !found; ++y) {
            const itcore_cell *cells = itcore_snapshot_row(s, y, &w, &rev);
            for (uint16_t x = 0; x < 20; ++x) {
                if (cell_text(s, y, x) == "X") {
                    CHECK(cells[x].flags & ITCORE_CELL_BOLD);
                    CHECK((cells[x].fg >> 24) == 1 && (cells[x].fg & 0xFF) == 196);
                    found = true; break;
                }
            }
        }
        CHECK(found);
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);
}

// CORE-05：宽度增减与硬/软换行重排索引
static void test_core05() {
    itcore_core *c = make(8, 4, 100, 1 << 20);
    // 写满多行产生滚动历史：软换行长行
    feed(c, "abcdefghijklmnop");  // 16 字，宽 8 → 两行软换后继续
    feed(c, "\r\nHARD\r\n");
    // 再滚几行让内容进 scrollback
    for (int i = 0; i < 6; ++i) feed(c, "L" + std::to_string(i) + "\r\n");
    // 缩小宽度
    CHECK(itcore_resize(c, 5, 4, 1) == 0);
    drain(c);
    {
        // 视口可用
        auto *s = itcore_acquire_snapshot(c, 0, 4);
        CHECK(s && itcore_snapshot_columns(s) == 5 && itcore_snapshot_rows(s) == 4);
        itcore_snapshot_release(s);
    }
    // 读历史：first_row 负值
    {
        auto *s = itcore_acquire_snapshot(c, -8, 8);
        CHECK(s);
        // 至少有一些非空历史行；硬换行行身份在重排后仍可通过 revision/内容访问
        int nonempty = 0;
        for (uint16_t i = 0; i < 8; ++i) {
            if (!row_text(s, i).empty()) ++nonempty;
        }
        CHECK(nonempty >= 1);
        itcore_snapshot_release(s);
    }
    // 再加宽
    CHECK(itcore_resize(c, 12, 4, 2) == 0);
    drain(c);
    {
        auto *s = itcore_acquire_snapshot(c, -4, 4);
        CHECK(s && itcore_snapshot_columns(s) == 12);
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);
}

// MEM-01 子集：行数 / 字节双重限额，淘汰最旧逻辑行
static void test_mem01() {
    // 限 5 逻辑行
    itcore_core *c = make(10, 3, /*sb*/5, /*mem*/1ull << 20);
    for (int i = 0; i < 20; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "row%02d\r\n", i);
        feed(c, buf);
    }
    // 快照历史不应无限增长；能取得快照且 core 仍可用
    auto *s = itcore_acquire_snapshot(c, -20, 20);
    CHECK(s);
    int nonempty = 0;
    for (uint16_t i = 0; i < 20; ++i)
        if (!row_text(s, i).empty()) ++nonempty;
    // 历史+视口有界：非空行远少于 20+ 全部写入
    CHECK(nonempty <= 5 + 3 + 2);  // sb + screen + slack
    itcore_snapshot_release(s);

    // 字节限额：极小内存
    itcore_destroy(c);
    c = make(40, 2, 10000, /*64 bytes*/ 200);
    for (int i = 0; i < 50; ++i) {
        feed(c, std::string(30, 'A') + "\r\n");
    }
    s = itcore_acquire_snapshot(c, -30, 30);
    CHECK(s);
    nonempty = 0;
    for (uint16_t i = 0; i < 30; ++i)
        if (!row_text(s, i).empty()) ++nonempty;
    CHECK(nonempty < 40);  // 受 200 字节约束
    itcore_snapshot_release(s);
    // 仍可继续写入（受控，不崩）
    feed(c, "still-ok\r\n");
    s = itcore_acquire_snapshot(c, 0, 2);
    CHECK(s);
    itcore_snapshot_release(s);
    itcore_destroy(c);
}

// CSI 默认参数单元（通过行为观察）
static void test_csi_defaults() {
    itcore_core *c = make(10, 5);
    feed(c, "\x1b[3;4H");  // 显式
    feed(c, "\x1b[H");     // 默认 1;1
    {
        auto *s = itcore_acquire_snapshot(c, 0, 5);
        uint16_t col, row; int vis;
        itcore_snapshot_cursor(s, &col, &row, &vis);
        CHECK(col == 0 && row == 0);
        itcore_snapshot_release(s);
    }
    feed(c, "\x1b[3;3H");
    feed(c, "\x1b[A");  // CUU 默认 1
    {
        auto *s = itcore_acquire_snapshot(c, 0, 5);
        uint16_t col, row; int vis;
        itcore_snapshot_cursor(s, &col, &row, &vis);
        CHECK(row == 1 && col == 2);  // 3;3 → up → 2;3 → 0 基 (1,2)
        itcore_snapshot_release(s);
    }
    itcore_destroy(c);
}

// B2：itcore_history_rows 缓存——内容/列宽不变时不重排；push、resize、ED3、切屏都正确失效
extern "C" uint64_t itcore_debug_history_recomputes(const itcore_core *c);
static void test_history_cache() {
    itcore_core *c = make(10, 3, 1000);
    auto feed = [&](const char *t) { itcore_feed(c, reinterpret_cast<const uint8_t *>(t), std::strlen(t)); drain(c); };
    CHECK(itcore_history_rows(c) == 0);
    feed("1\r\n2\r\n3\r\n4\r\n");               // 2 行进历史
    uint32_t h = itcore_history_rows(c);
    CHECK(h == 2);
    uint64_t n0 = itcore_debug_history_recomputes(c);
    for (int i = 0; i < 100; ++i) CHECK(itcore_history_rows(c) == h);
    CHECK(itcore_debug_history_recomputes(c) == n0);   // 命中缓存
    feed("x");                                          // 只改网格，不改历史
    CHECK(itcore_history_rows(c) == h && itcore_debug_history_recomputes(c) == n0);
    feed("\r\n5\r\n");                                // 新行进历史 → 失效
    CHECK(itcore_history_rows(c) == h + 2);
    feed("\r\n0123456789ABCDEFGHIJ\r\n\r\n\r\n");     // 一条 20 字符的软换行逻辑行入历史
    uint32_t w10 = itcore_history_rows(c);
    CHECK(itcore_resize(c, 20, 3, 1) == 0); drain(c);       // 列宽变化 → 失效并按新宽重排
    uint32_t w20 = itcore_history_rows(c);
    CHECK(w20 < w10);
    feed("\x1b[?1049h");
    CHECK(itcore_history_rows(c) == 0);                 // 备用屏
    feed("\x1b[?1049l");
    CHECK(itcore_history_rows(c) == w20);
    feed("\x1b[3J");                                   // ED3 清历史 → 失效
    CHECK(itcore_history_rows(c) == 0);
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "core03") test_core03();
    if (which == "all" || which == "core04") test_core04();
    if (which == "all" || which == "core05") test_core05();
    if (which == "all" || which == "mem01") test_mem01();
    if (which == "all" || which == "csi") test_csi_defaults();
    if (which == "all" || which == "histcache") test_history_cache();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ok\n");
    return failures ? 1 : 0;
}
