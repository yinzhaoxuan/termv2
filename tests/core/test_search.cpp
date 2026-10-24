// SRCH-01：itcore_search 纯文本搜索（草案 /workspace/qa/itcore_search-草案-v0.2.md §5 的 SRCH-01～36）。
// v0.1 不做正则：SRCH-13～15 整条跳过、SRCH-10～12 的正则部分跳过（v0.2 引入 PCRE2 后补）。
#include "itcore.h"
#include "screen.h"
#include "search.h"
#include "vt_parser.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static void skip(const char *id, const char *why) { std::printf("SKIP %s：%s\n", id, why); }

static int g_inval = 0;  // 收到的 SEARCH_INVALIDATED 事件数

static itcore_core *make(uint16_t cols = 20, uint16_t rows = 3, uint32_t sb = 1000) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), cols, rows, sb, 64ull << 20};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static void drain(itcore_core *c) {
    while (itcore_event *e = itcore_next_event(c)) {
        if (itcore_event_type(e) == ITCORE_EV_SEARCH_INVALIDATED) ++g_inval;
        itcore_event_release(e);
    }
}
static void feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    drain(c);
}
static std::string sel(itcore_core *c) {
    itcore_buffer *b = nullptr;
    if (itcore_selection_text(c, &b) != 0 || !b) return "<none>";
    std::string s(reinterpret_cast<char *>(b->data), b->length);
    itcore_buffer_release(b);
    return s;
}
static itcore_search_options opts(uint8_t cm = ITCORE_SEARCH_CASE_SMART, uint32_t flags = 0, uint64_t seg = 0, uint32_t off = 0) {
    itcore_search_options o{};
    o.struct_size = sizeof(o);
    o.case_mode = cm; o.flags = flags; o.start_seg = seg; o.start_off = off;
    return o;
}
static itcore_search *begin(itcore_core *c, const std::string &n, uint8_t cm = ITCORE_SEARCH_CASE_SMART, uint32_t flags = 0,
                            uint64_t seg = 0, uint32_t off = 0) {
    auto o = opts(cm, flags, seg, off);
    itcore_search *h = nullptr;
    CHECK(itcore_search_begin(c, n.data(), n.size(), &o, &h) == 0 && h);
    return h;
}
static itcore_search_progress prog0() { itcore_search_progress p{}; p.struct_size = sizeof(p); return p; }
// 一直 step 到 DONE，收集所有交付的结果（按交付顺序）
static itcore_search_progress run(itcore_search *h, std::vector<itcore_search_match> *all = nullptr, uint32_t budget = 0) {
    itcore_search_progress p = prog0();
    for (int guard = 0; guard < 100000; ++guard) {
        itcore_search_match buf[64]; uint32_t n = 0;
        int st = itcore_search_step(h, budget, 0, &p, buf, 64, &n);
        if (all) all->insert(all->end(), buf, buf + n);
        if (st != ITCORE_SEARCH_RUNNING && n < 64) break;
    }
    return p;
}
static int64_t total(itcore_core *c, const std::string &n, uint8_t cm = ITCORE_SEARCH_CASE_SMART) {
    auto *h = begin(c, n, cm);
    auto p = run(h);
    itcore_search_end(h);
    return p.total;
}
static std::vector<itcore_search_range> ranges(itcore_search *h, int64_t first, uint32_t rows) {
    int n = itcore_search_ranges_in_view(h, first, rows, nullptr, 0);
    std::vector<itcore_search_range> v(n > 0 ? size_t(n) : 0);
    if (n > 0) CHECK(itcore_search_ranges_in_view(h, first, rows, v.data(), uint32_t(n)) == n);
    return v;
}
static int64_t grid_abs(itcore_core *c, int y) {
    return int64_t(itcore_scrollback_overflow(c)) + int64_t(itcore_history_rows(c)) + y;
}
// 结果的视口坐标（相对网格首行）选成选区，取文本
static std::string sel_by_extend(itcore_core *c, const itcore_search_match &m) {
    int32_t sr, er; uint16_t sc, ec;
    if (itcore_search_match_to_view(c, &m, &sr, &sc, &er, &ec) != 0) return "<noview>";
    int32_t first = std::min(sr, 0);
    itcore_selection_start(c, first, sc, uint16_t(sr - first));
    itcore_selection_extend(c, first, uint16_t(ec - 1), uint16_t(er - first));
    return sel(c);
}

// SRCH-01～05：大小写
static void test_basic() {
    {   // SRCH-01（用 20 列：草案坐标按单行写，10 列时 11 个字符会自动换行）
        auto *c = make(20, 3);
        feed(c, "foo bar foo");
        auto *h = begin(c, "foo");
        std::vector<itcore_search_match> all;
        auto p = run(h, &all);
        CHECK(p.status == ITCORE_SEARCH_DONE && p.total == 2 && p.progress == 1.0f);
        CHECK(all.size() == 2);
        if (all.size() == 2) {
            CHECK(all[0].start_abs_row == 0 && all[0].start_col == 0 && all[0].end_abs_row == 0 && all[0].end_col == 3);
            CHECK(all[1].start_abs_row == 0 && all[1].start_col == 8 && all[1].end_abs_row == 0 && all[1].end_col == 11);
        }
        itcore_search_match m{}; int64_t idx = -1;
        CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_DONE && idx == 0 && m.start_col == 0);
        CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_DONE && idx == 1 && m.start_col == 8);
        CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_NOT_FOUND);  // 不回绕
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-02～04
        auto *c = make(20, 3);
        feed(c, "Foo foo FOO");
        auto *h = begin(c, "foo", ITCORE_SEARCH_CASE_SENSITIVE);
        std::vector<itcore_search_match> all;
        auto p = run(h, &all);
        CHECK(p.total == 1 && all.size() == 1 && all[0].start_col == 4 && all[0].end_col == 7);
        itcore_search_end(h);
        CHECK(total(c, "foo", ITCORE_SEARCH_CASE_INSENSITIVE) == 3);
        CHECK(total(c, "foo", ITCORE_SEARCH_CASE_SMART) == 3);
        CHECK(total(c, "Foo", ITCORE_SEARCH_CASE_SMART) == 1);
        CHECK(total(c, "FOO", ITCORE_SEARCH_CASE_INSENSITIVE) == 3);
        itcore_destroy(c);
    }
    {   // SRCH-05：简单折叠 Ω↔ω；ß 不展开成 ss（已知差异）
        auto *c = make(40, 3);
        feed(c, "Stra\xC3\x9F" "e STRASSE \xCE\xA9mega \xCF\x89MEGA");
        CHECK(total(c, "\xCF\x89mega", ITCORE_SEARCH_CASE_INSENSITIVE) == 2);
        CHECK(total(c, "\xCE\xA9MEGA", ITCORE_SEARCH_CASE_INSENSITIVE) == 2);
        CHECK(total(c, "strasse", ITCORE_SEARCH_CASE_INSENSITIVE) == 1);
        CHECK(total(c, "stra\xC3\x9F" "e", ITCORE_SEARCH_CASE_INSENSITIVE) == 1);
        CHECK(total(c, "\xCE\xA9mega", ITCORE_SEARCH_CASE_SMART) == 1);  // 含大写 → 区分
        itcore_destroy(c);
    }
}

// SRCH-06～12：宽字符、换行、簇
static void test_cells() {
    {   // SRCH-06/07
        auto *c = make(10, 3);
        feed(c, "ab\xE4\xB8\xAD\xE6\x96\x87" "cd");
        auto *h = begin(c, "\xE6\x96\x87" "c");
        std::vector<itcore_search_match> all;
        run(h, &all);
        CHECK(all.size() == 1);
        if (!all.empty()) {
            auto &m = all[0];
            CHECK(m.start_abs_row == 0 && m.start_col == 4 && m.end_abs_row == 0 && m.end_col == 7);
            CHECK(m.start_off == 3 && m.end_off == 4 && m.start_seg == m.end_seg);
            CHECK(itcore_search_select(h, 0) == 0);
            CHECK(sel(c) == "\xE6\x96\x87" "c");
            CHECK(sel_by_extend(c, m) == "\xE6\x96\x87" "c");  // SRCH-28
        }
        itcore_search_end(h);
        h = begin(c, "\xE4\xB8\xAD");
        all.clear();
        run(h, &all);
        CHECK(all.size() == 1 && all[0].start_col == 2 && all[0].end_col == 4);
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-08：宽字符放不下时行尾补的空格不算内容
        auto *c = make(5, 3);
        feed(c, "abcd\xE4\xB8\xAD");
        auto *h = begin(c, "d\xE4\xB8\xAD");
        std::vector<itcore_search_match> all;
        run(h, &all);
        CHECK(all.size() == 1);
        if (!all.empty()) CHECK(all[0].start_abs_row == 0 && all[0].start_col == 3 && all[0].end_abs_row == 1 && all[0].end_col == 2);
        itcore_search_end(h);
        CHECK(total(c, "d \xE4\xB8\xAD") == 0);
        itcore_destroy(c);
    }
    {   // SRCH-09：软换行拼接
        auto *c = make(5, 4);
        feed(c, "hello world");
        auto *h = begin(c, "lo wo");
        std::vector<itcore_search_match> all;
        run(h, &all);
        CHECK(all.size() == 1);
        if (!all.empty()) {
            CHECK(all[0].start_abs_row == 0 && all[0].start_col == 3 && all[0].end_abs_row == 1 && all[0].end_col == 3);
            CHECK(itcore_search_select(h, 0) == 0 && sel(c) == "lo wo");
            CHECK(sel_by_extend(c, all[0]) == "lo wo");  // SRCH-28
        }
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-10：硬换行不拼接（正则部分 v0.1 跳过）
        auto *c = make(20, 3);
        feed(c, "foo\r\nbar");
        CHECK(total(c, "foobar") == 0);
        CHECK(total(c, "foo bar") == 0);
        CHECK(total(c, "bar") == 1);
        itcore_destroy(c);
        skip("SRCH-10 正则部分", "v0.1 不支持正则（o\\nb、o.b、^bar$），v0.2 引入 PCRE2 后补");
    }
    {   // SRCH-11：ZWJ / VS16（正则部分跳过）
        auto *c = make(30, 3);
        const std::string fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
        feed(c, fam + " x \xE2\x9D\xA4\xEF\xB8\x8F y \xE2\x9D\xA4 z");
        auto *h = begin(c, fam);
        std::vector<itcore_search_match> all;
        run(h, &all);
        CHECK(all.size() == 1);
        if (!all.empty()) {
            CHECK(all[0].end_col - all[0].start_col == 2);
            CHECK(itcore_search_select(h, 0) == 0 && sel(c) == fam);
            CHECK(sel_by_extend(c, all[0]) == fam);  // SRCH-28
        }
        itcore_search_end(h);
        CHECK(total(c, "\xF0\x9F\x91\xA9") == 0);  // 簇内不对齐
        CHECK(total(c, "\xE2\x9D\xA4", ITCORE_SEARCH_CASE_SENSITIVE) == 1);
        CHECK(total(c, "\xE2\x9D\xA4", ITCORE_SEARCH_CASE_INSENSITIVE) == 2);
        CHECK(total(c, "\xE2\x9D\xA4\xEF\xB8\x8F", ITCORE_SEARCH_CASE_INSENSITIVE) == 2);
        itcore_destroy(c);
        skip("SRCH-11 正则部分", "v0.1 不支持正则（正则 👩 扩展为整簇），v0.2 补");
    }
    {   // SRCH-12：组合字符（正则部分跳过）
        auto *c = make(20, 3);
        feed(c, "e\xCC\x81 caf\xC3\xA9");
        auto *h = begin(c, "e\xCC\x81");
        std::vector<itcore_search_match> all;
        run(h, &all);
        CHECK(all.size() == 1 && !all.empty() && all[0].end_col - all[0].start_col == 1);
        itcore_search_end(h);
        CHECK(total(c, "e") == 0);
        CHECK(total(c, "\xC3\xA9") == 1);  // 只命中预组合（不做规范化）
        itcore_destroy(c);
        skip("SRCH-12 正则部分", "v0.1 不支持正则（正则 e 扩展为整格），v0.2 补");
    }
    skip("SRCH-13", "v0.2：正则基本与零长度（v0.1 不支持正则）");
    skip("SRCH-14", "v0.2：灾难性正则（v0.1 不支持正则）");
    skip("SRCH-15", "v0.2：正则长匹配上限（v0.1 不支持正则）");
}

// SRCH-16～18：方向、回绕、分批
static void test_direction() {
    auto *c = make(10, 12);
    for (int i = 0; i < 10; ++i) feed(c, "k" + std::to_string(i) + (i < 9 ? "\r\n" : ""));
    std::vector<itcore_search_match> fwd;
    auto *h = begin(c, "k");
    run(h, &fwd);
    itcore_search_end(h);
    CHECK(fwd.size() == 10);
    if (fwd.size() != 10) return;
    {   // SRCH-16
        h = begin(c, "k", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_BACKWARD | ITCORE_SEARCH_F_WRAP, fwd[5].start_seg, 0);
        std::vector<itcore_search_match> back;
        run(h, &back);
        std::set<uint64_t> a, b;
        for (auto &m : fwd) a.insert(m.start_seg);
        for (auto &m : back) b.insert(m.start_seg);
        CHECK(a == b);
        const int want[10] = {4, 3, 2, 1, 0, 9, 8, 7, 6, 5};
        for (int i = 0; i < 10; ++i) {
            itcore_search_match m{}; int64_t idx = -1;
            CHECK(itcore_search_prev(h, &m, &idx) == ITCORE_SEARCH_DONE);
            CHECK(idx == want[i] && m.start_seg == fwd[size_t(want[i])].start_seg);
            auto p = run(h);
            CHECK(((p.flags & ITCORE_SEARCH_PF_WRAPPED) != 0) == (i >= 5));
            CHECK(p.current == want[i]);
        }
        itcore_search_end(h);
        h = begin(c, "k", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_BACKWARD, fwd[5].start_seg, 0);
        run(h);
        for (int i = 4; i >= 0; --i) {
            int64_t idx = -1;
            CHECK(itcore_search_prev(h, nullptr, &idx) == ITCORE_SEARCH_DONE && idx == i);
        }
        CHECK(itcore_search_prev(h, nullptr, nullptr) == ITCORE_SEARCH_NOT_FOUND);
        itcore_search_end(h);
    }
    {   // SRCH-17：只有起点行有匹配
        h = begin(c, "k3", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP, fwd[3].start_seg, 0);
        auto p = run(h);
        CHECK(p.total == 1);
        itcore_search_match m1{}, m2{}; int64_t i1 = -1, i2 = -1;
        CHECK(itcore_search_next(h, &m1, &i1) == ITCORE_SEARCH_DONE && i1 == 0);
        CHECK(itcore_search_next(h, &m2, &i2) == ITCORE_SEARCH_DONE && i2 == 0 && m2.start_seg == m1.start_seg);
        CHECK(itcore_search_prev(h, &m2, &i2) == ITCORE_SEARCH_DONE && i2 == 0);
        itcore_search_end(h);
    }
    itcore_destroy(c);
    {   // SRCH-18：分批（用 BACKWARD：从末尾往上扫，prev 立即可用）
        auto *d = make(80, 24, 10000);
        std::string s;
        for (int i = 0; i < 5000; ++i) s += "line " + std::to_string(i) + " x\r\n";
        feed(d, s);
        h = begin(d, "x", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_BACKWARD | ITCORE_SEARCH_F_WRAP);
        itcore_search_progress p = prog0();
        itcore_search_match buf[256]; uint32_t n = 0;
        CHECK(itcore_search_step(h, 2000, 0, &p, buf, 256, &n) == ITCORE_SEARCH_RUNNING);
        CHECK(p.status == ITCORE_SEARCH_RUNNING && p.total == -1 && p.progress > 0 && p.progress < 1 && n > 0);
        int64_t idx = -1;
        itcore_search_match m{};
        int st = itcore_search_prev(h, &m, &idx);
        CHECK(st == ITCORE_SEARCH_RUNNING && idx >= 0);
        p = run(h, nullptr, 2000);
        CHECK(p.status == ITCORE_SEARCH_DONE && p.total == 5000 && p.progress == 1.0f);
        CHECK(p.current == 4999);
        itcore_search_end(h);
        itcore_destroy(d);
    }
}

static int count_sub(const std::string &s, const std::string &n) {
    int k = 0;
    for (size_t p = s.find(n); p != std::string::npos; p = s.find(n, p + 1)) ++k;
    return k;
}
static std::string export_all(itcore_core *c) {
    itcore_buffer *b = nullptr;
    if (itcore_export_text(c, 1u << 30, &b) != 0 || !b) return {};
    std::string s(reinterpret_cast<char *>(b->data), b->length);
    itcore_buffer_release(b);
    return s;
}

// SRCH-19～22：淘汰、改写、新内容
static void test_mutation() {
    {   // SRCH-19
        auto *c = make(10, 5, 100);
        std::string s;
        for (int i = 0; i < 300; ++i) s += "xo\r\n";
        feed(c, s);
        uint64_t ov0 = itcore_scrollback_overflow(c);
        auto *h = begin(c, "xo", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP);
        itcore_search_progress p = prog0();
        uint32_t n = 0;
        itcore_search_match buf[8];
        itcore_search_step(h, 1000, 0, &p, buf, 8, &n);  // 先扫一大段，再淘汰
        s.clear();
        for (int i = 0; i < 50; ++i) s += "xn\r\n";
        feed(c, s);
        uint64_t ov1 = itcore_scrollback_overflow(c);
        CHECK(ov1 > ov0);
        uint32_t dropped = 0;
        for (int g = 0; g < 10000; ++g) {
            int st = itcore_search_step(h, 10, 0, &p, buf, 8, &n);
            dropped += p.dropped;
            for (uint32_t i = 0; i < n; ++i) CHECK(itcore_search_match_to_view(c, &buf[i], nullptr, nullptr, nullptr, nullptr) == 0);
            if (st == ITCORE_SEARCH_DONE && n < 8) break;
        }
        CHECK(p.status == ITCORE_SEARCH_DONE);
        CHECK(dropped > 0);
        CHECK(p.flags & ITCORE_SEARCH_PF_NEW_CONTENT);
        CHECK(p.total == count_sub(export_all(c), "xo"));
        CHECK(p.total <= 100 + 5);
        // 所有存活结果都能映射到视口
        for (int64_t i = 0; i < p.total; ++i) {
            itcore_search_match m{}; int64_t idx = -1;
            CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_DONE);
            CHECK(itcore_search_match_to_view(c, &m, nullptr, nullptr, nullptr, nullptr) == 0);
        }
        itcore_search_end(h);
        CHECK(itcore_scrollback_overflow(c) >= ov1);
        itcore_destroy(c);
    }
    {   // SRCH-20：当前结果被淘汰
        auto *c = make(10, 3, 10);
        std::string s;
        for (int i = 0; i < 20; ++i) s += "a" + std::to_string(i) + "\r\n";
        feed(c, s);
        auto *h = begin(c, "a", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP);
        auto p = run(h);
        const int64_t old_total = p.total;
        CHECK(old_total == 12);
        int64_t idx = -1;
        itcore_search_match first{};
        for (int g = 0; g < 20 && idx != 0; ++g) itcore_search_next(h, &first, &idx);
        CHECK(idx == 0);
        p = run(h);
        CHECK(p.current == 0);
        feed(c, "b\r\nb\r\nb\r\n");
        p = run(h);
        CHECK(p.current == -1);
        CHECK(p.total == old_total - 3);
        itcore_search_match m{};
        CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_DONE && idx == 0);
        CHECK(m.start_seg != first.start_seg && m.start_abs_row == int64_t(itcore_scrollback_overflow(c)));
        CHECK(itcore_search_select(h, old_total - 1) == -1);
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-21：网格行原地改写
        auto *c = make(10, 5);
        feed(c, "a\r\nb\r\nfoo");
        auto *h = begin(c, "foo");
        auto p = run(h);
        CHECK(p.total == 1);
        feed(c, "\x1b[3;1Hbar");
        p = run(h);
        CHECK(p.dropped == 1 && p.total == 0);
        feed(c, "\x1b[3;1Hfoo");
        p = run(h);
        CHECK(p.changed == 1 && p.total == 1);
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-22：新输出不在范围内
        auto *c = make(10, 5);
        feed(c, "foo");
        auto *h = begin(c, "foo");
        auto p = run(h);
        CHECK(p.total == 1 && !(p.flags & ITCORE_SEARCH_PF_NEW_CONTENT));
        feed(c, "\r\nfoo");
        p = run(h);
        CHECK(p.total == 1 && (p.flags & ITCORE_SEARCH_PF_NEW_CONTENT));
        itcore_search_end(h);
        CHECK(total(c, "foo") == 2);
        itcore_destroy(c);
    }
}

// SRCH-23～26、36：失效
static void test_invalidation() {
    {   // SRCH-23：resize
        auto *c = make(10, 5);
        feed(c, "0123456789abcfoo");
        auto *h = begin(c, "foo", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP);
        run(h);
        itcore_search_match m{}; int64_t idx = -1;
        CHECK(itcore_search_next(h, &m, &idx) == ITCORE_SEARCH_DONE);
        CHECK(m.start_abs_row == 1 && m.start_col == 3);
        CHECK(itcore_resize(c, 40, 24, 0) == 0);
        drain(c);
        itcore_search_progress p = prog0();
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED);
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTART);
        CHECK(itcore_search_next(h, nullptr, nullptr) == ITCORE_SEARCH_RESTART);
        CHECK(itcore_search_prev(h, nullptr, nullptr) == ITCORE_SEARCH_RESTART);
        itcore_search_end(h);
        h = begin(c, "foo", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP, m.start_seg, m.start_off);
        run(h);
        itcore_search_match m2{};
        CHECK(itcore_search_next(h, &m2, &idx) == ITCORE_SEARCH_DONE);
        CHECK(m2.start_seg == m.start_seg && m2.start_off == m.start_off);
        // 核心 resize 不重排网格（软换行行按新宽度补空），坐标按新屏幕重算：与视口映射、选区一致
        int32_t sr = -1; uint16_t sc = 0;
        CHECK(itcore_search_match_to_view(c, &m2, &sr, &sc, nullptr, nullptr) == 0);
        CHECK(m2.start_abs_row == grid_abs(c, sr) && m2.start_col == sc);
        CHECK(sel_by_extend(c, m2) == "foo");
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-24：alt 屏
        auto *c = make(10, 3);
        feed(c, "mainfoo\r\n\r\n\r\n\r\n");
        auto *h = begin(c, "foo");
        CHECK(run(h).total == 1);
        feed(c, "\x1b[?1049h");
        itcore_search_progress p = prog0();
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED);
        itcore_search_end(h);
        feed(c, "altfoo");
        h = begin(c, "foo");
        std::vector<itcore_search_match> all;
        CHECK(run(h, &all).total == 1);
        CHECK(all.size() == 1 && !all.empty() && all[0].start_col == 3);
        feed(c, "\x1b[?1049l");
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED);
        itcore_search_end(h);
        CHECK(total(c, "foo") == 1);
        CHECK(total(c, "mainfoo") == 1);
        itcore_destroy(c);
    }
    {   // SRCH-25：ED 3
        auto *c = make(10, 3);
        feed(c, "foo1\r\nfoo2\r\nx\r\nx\r\nfoo3");
        auto *h = begin(c, "foo");
        CHECK(run(h).total == 3);
        uint64_t ov = itcore_scrollback_overflow(c);
        feed(c, "\x1b[3J");  // 本核心的 ED 3 同时清屏（ED 2）
        itcore_search_progress p = prog0();
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED);
        itcore_search_end(h);
        CHECK(total(c, "foo") == 0);
        feed(c, "\x1b[Hfoo4");
        CHECK(total(c, "foo") == 1);  // 只剩屏幕上的
        CHECK(itcore_scrollback_overflow(c) >= ov);
        itcore_destroy(c);
    }
    {   // SRCH-26：RIS
        auto *c = make(10, 3);
        feed(c, "foo");
        auto *h = begin(c, "foo");
        CHECK(run(h).total == 1);
        feed(c, "\x1b" "c");
        itcore_search_progress p = prog0();
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED);
        itcore_search_end(h);
        CHECK(total(c, "foo") == 0);
        itcore_destroy(c);
    }
    {   // SRCH-36：四种失效各发一次事件；step 先 RESTARTED 一次，之后 RESTART
        const char *trig[] = {"resize", "\x1b[?1049h", "\x1b[3J", "\x1b" "c"};
        for (const char *t : trig) {
            auto *c = make(10, 3);
            feed(c, "foo\r\nfoo\r\nfoo\r\nfoo");
            auto *h = begin(c, "foo");
            run(h);
            g_inval = 0;
            feed(c, "more");  // 普通输出不失效
            CHECK(g_inval == 0);
            if (std::string(t) == "resize") { itcore_resize(c, 12, 3, 0); drain(c); }
            else feed(c, t);
            CHECK(g_inval == 1);
            if (std::string(t) == "resize") { itcore_resize(c, 14, 3, 0); drain(c); }
            else feed(c, "\x1b" "c");
            CHECK(g_inval == 1);  // 同一句柄只发一次
            itcore_search_progress p = prog0();
            CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTARTED && p.status == ITCORE_SEARCH_RESTARTED);
            CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTART);
            CHECK(itcore_search_next(h, nullptr, nullptr) == ITCORE_SEARCH_RESTART);
            CHECK(itcore_search_ranges_in_view(h, 0, 3, nullptr, 0) == ITCORE_SEARCH_RESTART);
            CHECK(itcore_search_select(h, 0) == ITCORE_SEARCH_RESTART);
            itcore_search_end(h);
            g_inval = 0;  // 没有活动句柄：不发
            itcore_resize(c, 20, 3, 0); drain(c);
            feed(c, "\x1b" "c");
            CHECK(g_inval == 0);
            itcore_destroy(c);
        }
    }
}

// SRCH-27：字素池压缩不使句柄失效（Screen 层直接触发 compact_pool）
namespace {
struct VecSink : itcore::TokenSink {
    std::vector<itcore::Token> v;
    void token(const itcore::Token &t) override { v.push_back(t); }
};
void feed_screen(itcore::Screen &sc, itcore::Parser &p, const std::string &s) {
    VecSink k;
    p.feed(reinterpret_cast<const uint8_t *>(s.data()), s.size(), k);
    p.flush_text(k);
    for (auto &t : k.v) sc.apply(t, nullptr);
}
}  // namespace
static void test_compact() {
    itcore::Screen sc(20, 5, 100, 1 << 20);
    itcore::Parser par;
    for (int k = 0; k < 300; ++k)
        feed_screen(sc, par, "e\xCC\x81" + std::string(k % 3 ? "\xF0\x9F\x91\x8D" : "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB") +
                                 "foo\r\n");
    auto o = opts();
    itcore::Searcher *h = nullptr;
    CHECK(itcore::Searcher::create(sc, "\xF0\x9F\x91\x8D" "foo", 7, &o, &h) == 0 && h);
    if (!h) return;
    itcore_search_progress p = prog0();
    std::vector<itcore_search_match> a(512);
    uint32_t n = 0;
    CHECK(h->step(0, 0, &p, a.data(), 512, &n) == ITCORE_SEARCH_DONE);
    a.resize(n);
    const int64_t t0 = p.total;
    CHECK(t0 == 70);  // 最后 104 行（k=196..299）里不带肤色的（k%3!=0）
    const uint64_t ep = sc.pool().epoch();
    sc.compact_pool();
    CHECK(sc.pool().epoch() > ep);
    CHECK(!h->dead());
    CHECK(h->step(0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_DONE);
    CHECK(p.total == t0 && p.dropped == 0);
    std::vector<itcore_search_range> r(256);
    int k = h->ranges(0, 1000, r.data(), 256);
    CHECK(k == t0);
    itcore_search_match m{};
    CHECK(h->next(&m, nullptr) == ITCORE_SEARCH_DONE);
    bool known = false;
    for (auto &x : a) if (x.start_seg == m.start_seg && x.start_off == m.start_off) known = true;
    CHECK(known);
    delete h;
}

// SRCH-28 已并入 SRCH-06/09/11；SRCH-29～30：生命周期与参数
static void test_lifecycle() {
    {
        auto *c = make();
        feed(c, "foo foo");
        auto *h1 = begin(c, "foo");
        run(h1);
        auto *h2 = begin(c, "foo");  // 使 h1 失效
        itcore_search_progress p = prog0();
        CHECK(itcore_search_step(h1, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_RESTART);
        CHECK(itcore_search_next(h1, nullptr, nullptr) == ITCORE_SEARCH_RESTART);
        CHECK(itcore_search_ranges_in_view(h1, 0, 3, nullptr, 0) == ITCORE_SEARCH_RESTART);
        CHECK(run(h2).total == 2);
        itcore_search_end(h1);
        CHECK(run(h2).total == 2);
        (void)begin(c, "x");  // 不 end，交给 destroy
        itcore_destroy(c);    // h2 与第三个句柄一起释放（ASan 检查）
    }
    {   // SRCH-30
        auto *c = make();
        itcore_search *h = reinterpret_cast<itcore_search *>(1);
        auto o = opts();
        CHECK(itcore_search_begin(c, "a", 0, &o, &h) == ITCORE_SEARCH_EINVAL && h == nullptr);
        h = reinterpret_cast<itcore_search *>(1);
        CHECK(itcore_search_begin(c, "a", 1, nullptr, &h) == ITCORE_SEARCH_EINVAL && h == nullptr);
        auto small = o; small.struct_size = 4;
        CHECK(itcore_search_begin(c, "a", 1, &small, &h) == ITCORE_SEARCH_EINVAL && h == nullptr);
        auto bad = o; bad.flags = 1u << 9;
        CHECK(itcore_search_begin(c, "a", 1, &bad, &h) == ITCORE_SEARCH_EINVAL && h == nullptr);
        bad = o; bad.case_mode = 3;
        CHECK(itcore_search_begin(c, "a", 1, &bad, &h) == ITCORE_SEARCH_EINVAL && h == nullptr);
        auto rx = o; rx.flags = ITCORE_SEARCH_F_REGEX;
        CHECK(itcore_search_begin(c, "a", 1, &rx, &h) == ITCORE_SEARCH_EUNSUPPORTED && h == nullptr);
        CHECK(itcore_search_begin(nullptr, "a", 1, &o, &h) == ITCORE_SEARCH_EINVAL);
        CHECK(itcore_search_step(nullptr, 0, 0, nullptr, nullptr, 0, nullptr) == ITCORE_SEARCH_EINVAL);
        h = begin(c, "a");
        itcore_search_progress p{};  // struct_size = 0
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 0, nullptr) == ITCORE_SEARCH_EINVAL);
        p = prog0();
        CHECK(itcore_search_step(h, 0, 0, &p, nullptr, 4, nullptr) == ITCORE_SEARCH_EINVAL);
        itcore_search_end(h);
        itcore_search_end(nullptr);
        itcore_destroy(c);
    }
}

// SRCH-31～35：ranges_in_view
static void test_ranges() {
    {   // SRCH-31
        auto *c = make(20, 24, 1000);
        std::string s;
        for (int i = 0; i < 200; ++i) s += (i % 10 == 0 ? "foo " : "bar ") + std::to_string(i) + (i < 199 ? "\r\n" : "");
        feed(c, s);
        CHECK(itcore_history_rows(c) == 176);
        auto *h = begin(c, "foo");
        CHECK(run(h).total == 20);
        auto v = ranges(h, 100, 24);
        CHECK(v.size() == 3);
        for (size_t i = 0; i < v.size(); ++i)
            CHECK(v[i].row == int64_t(100 + 10 * i) && v[i].start_col == 0 && v[i].end_col == 3 && v[i].is_current == 0);
        CHECK(ranges(h, 176, 24).size() == 2);  // 网格部分 176..199：180、190
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-32：软换行拆成两条；硬换行不拼接
        auto *c = make(10, 3);
        feed(c, "xxxxxxxfoobar");
        auto *h = begin(c, "foob");
        CHECK(run(h).total == 1);
        auto v = ranges(h, 0, 3);
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].row == 0 && v[0].start_col == 7 && v[0].end_col == 10);
            CHECK(v[1].row == 1 && v[1].start_col == 0 && v[1].end_col == 1);
        }
        // 只看第二行：仍给出跨过来的那一段
        auto w = ranges(h, 1, 1);
        CHECK(w.size() == 1 && !w.empty() && w[0].row == 1 && w[0].end_col == 1);
        itcore_search_end(h);
        feed(c, "\r\nfoo\r\nbar");
        CHECK(total(c, "foobar") == 1);  // 只有第一行那条（软换行）
        itcore_destroy(c);
    }
    {   // SRCH-33：宽字符与 is_current
        auto *c = make(20, 3);
        feed(c, "\xE4\xB8\xAD\xE6\x96\x87" "foo\xE4\xB8\xAD\xE6\x96\x87" "foo");
        auto *h = begin(c, "\xE4\xB8\xAD\xE6\x96\x87", ITCORE_SEARCH_CASE_SMART, ITCORE_SEARCH_F_WRAP);
        run(h);
        CHECK(itcore_search_next(h, nullptr, nullptr) == ITCORE_SEARCH_DONE);
        auto v = ranges(h, 0, 3);
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].start_col == 0 && v[0].end_col == 4 && v[0].is_current == 1);  // 两个宽字符 = 4 格
            CHECK(v[1].start_col == 7 && v[1].end_col == 11 && v[1].is_current == 0);
        }
        CHECK(itcore_search_prev(h, nullptr, nullptr) == ITCORE_SEARCH_DONE);  // 回绕到第二条
        v = ranges(h, 0, 3);
        CHECK(v.size() == 2 && v[0].is_current == 0 && v[1].is_current == 1);
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-34：cap 溢出
        auto *c = make(80, 10);
        std::string s;
        for (int i = 0; i < 10; ++i) s += std::string("ab ab ab ab ab") + (i < 9 ? "\r\n" : "");
        feed(c, s);
        auto *h = begin(c, "ab");
        CHECK(run(h).total == 50);
        itcore_search_range a[8], b[50];
        std::memset(a, 0, sizeof(a));
        CHECK(itcore_search_ranges_in_view(h, grid_abs(c, 0), 10, a, 8) == 50);
        CHECK(itcore_search_ranges_in_view(h, grid_abs(c, 0), 10, b, 50) == 50);
        CHECK(std::memcmp(a, b, sizeof(a)) == 0);
        CHECK(b[49].row == grid_abs(c, 9) && b[49].start_col == 12);
        itcore_search_end(h);
        itcore_destroy(c);
    }
    {   // SRCH-35：视口指向已淘汰的行
        auto *c = make(10, 3, 10);
        std::string s;
        for (int i = 0; i < 10; ++i) s += "foo\r\n";
        feed(c, s);
        auto *h = begin(c, "foo");
        CHECK(run(h).total == 10);
        int64_t old_first = int64_t(itcore_scrollback_overflow(c));
        s.clear();
        for (int i = 0; i < 20; ++i) s += "zz\r\n";
        feed(c, s);
        run(h);
        itcore_search_range r[4];
        std::memset(r, 0x5a, sizeof(r));
        CHECK(itcore_search_ranges_in_view(h, old_first, 3, r, 4) == 0);
        CHECK(reinterpret_cast<unsigned char *>(r)[0] == 0x5a);
        itcore_search_end(h);
        itcore_destroy(c);
    }
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "basic") test_basic();
    if (w == "all" || w == "cells") test_cells();
    if (w == "all" || w == "direction") test_direction();
    if (w == "all" || w == "mutation") test_mutation();
    if (w == "all" || w == "inval") test_invalidation();
    if (w == "all" || w == "compact") test_compact();
    if (w == "all" || w == "lifecycle") test_lifecycle();
    if (w == "all" || w == "ranges") test_ranges();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ok\n");
    return 0;
}
