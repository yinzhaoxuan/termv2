// NEW-02：增量组计数与快照完整重排逐步对照。
#include "screen.h"
#include <cstdio>
#include <random>
#include <string>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
extern "C" uint64_t itcore_debug_history_recomputes(const itcore_core *c);

// 同一字节流交给 C API 与独立 Screen；参考值只走快照的完整重排路径。
struct Stream : itcore::TokenSink {
    itcore_core *core = nullptr;
    itcore::Screen reference;
    itcore::Parser parser;
    uint64_t resize_gen = 0;
    Stream(uint16_t cols, uint16_t rows, uint32_t history)
        : reference(cols, rows, history, 1ull << 20) {
        itcore_options o{ITCORE_ABI_VERSION, sizeof(o), cols, rows, history, 1ull << 20};
        CHECK(itcore_create(&o, &core) == 0 && core);
    }
    ~Stream() { itcore_destroy(core); }
    void token(const itcore::Token &t) override { reference.apply(t, nullptr); }
    void drain() { while (auto *e = itcore_next_event(core)) itcore_event_release(e); }
    void feed(const std::string &text) {
        const auto *p = reinterpret_cast<const uint8_t *>(text.data());
        auto r = itcore_feed(core, p, text.size());
        CHECK(r.status == ITCORE_OK && r.accepted == text.size());
        drain();
        parser.feed(p, text.size(), *this);
        parser.flush_text(*this);
    }
    void resize(uint16_t cols, uint16_t rows) {
        CHECK(itcore_resize(core, cols, rows, ++resize_gen) == 0);
        drain();
        reference.resize(cols, rows);
    }
    void check() {
        const size_t full = reference.on_alt() ? 0 : reference.scrollback().merged_display_rows_full_for_test(reference.grid().cols());
        CHECK(itcore_history_rows(core) == full);
    }
    uint64_t recomputes() const { return itcore_debug_history_recomputes(core); }
};

static void test_stream() {
    Stream s(12, 3, 17);
    const std::string lines[] = {"short", std::string(12, 'x'), std::string(36, 'y'),
                                  "12345678901中文中文中文中文", ""};
    s.check();
    for (int i = 0; i < 100; ++i) { s.feed(lines[i % 5] + "\r\n"); s.check(); }
    CHECK(s.reference.scrollback().logical_count() == 17);
    CHECK(s.reference.scrollback().evicted() > 0);
    const auto before = s.recomputes();
    for (int i = 0; i < 2000; ++i) {
        s.feed(lines[i % 5] + "\r\n");
        s.check();
        s.check();
        CHECK(s.recomputes() == before);
    }
}

static itcore::LogicalLine line(size_t cells, bool hard, uint64_t id = 1) {
    itcore::LogicalLine l;
    l.hard = hard; l.id = id;
    l.cells.assign(cells, itcore::Cell::blank());
    for (auto &c : l.cells) c.grapheme_id = 1;
    return l;
}
static void check(itcore::Screen &s) {
    CHECK(s.history_display_rows() == s.scrollback().merged_display_rows_full_for_test(s.grid().cols()));
}
static void test_lazy_groups() {
    itcore::Screen s(12, 2, 3000, 64ull << 20);
    auto &sb = s.scrollback();
    check(s);
    auto before = sb.debug_hist_rows_group_recompute_count;
    const auto full = s.debug_hist_rows_recompute_count;
    for (int i = 0; i < 3000; ++i) sb.push(line(12, false));
    CHECK(sb.debug_hist_rows_group_recompute_count == before);
    check(s); check(s);
    CHECK(s.history_display_rows() == 3000 && sb.evicted() == 0);
    CHECK(sb.debug_hist_rows_group_recompute_count == before + 1);
    CHECK(s.debug_hist_rows_recompute_count == full);

    // 首尾同一脏范围：长首组连续淘汰，尾部跨多个 hard 边界，查询前都不重排。
    before = sb.debug_hist_rows_group_recompute_count;
    for (int i = 0; i < 1000; ++i) sb.push(line(1, true));
    CHECK(sb.debug_hist_rows_group_recompute_count == before);
    check(s); check(s);
    CHECK(s.history_display_rows() == 3000 && sb.evicted() == 1000);
    CHECK(sb.debug_hist_rows_group_recompute_count == before + 1);

    // 分离的脏首/尾：只各重排一次，不扫描中间干净组。
    before = sb.debug_hist_rows_group_recompute_count;
    for (int i = 0; i < 500; ++i) sb.push(line(1, i % 3 == 0));
    CHECK(sb.debug_hist_rows_group_recompute_count == before);
    check(s); check(s);
    CHECK(sb.debug_hist_rows_group_recompute_count == before + 2);
    CHECK(s.debug_hist_rows_recompute_count == full);

    // 未查询的脏分区直接清空；下次追加仍可惰性计数。
    sb.push(line(12, false));
    before = sb.debug_hist_rows_group_recompute_count;
    sb.clear(); check(s);
    CHECK(s.history_display_rows() == 0);
    CHECK(sb.debug_hist_rows_group_recompute_count == before);
    sb.push(line(1, false)); check(s);
    CHECK(sb.debug_hist_rows_group_recompute_count == before + 1);
    CHECK(s.debug_hist_rows_recompute_count == full);
}
static void test_eviction() {
    itcore::Screen s(5, 2, 4, 1ull << 20);
    auto &sb = s.scrollback();
    check(s);
    auto before = s.debug_hist_rows_recompute_count;
    sb.push(line(2, false)); check(s);
    sb.push(line(3, false)); check(s);
    sb.push(line(2, true)); check(s);
    sb.push(line(1, true)); check(s);
    CHECK(s.history_display_rows() == 3);
    sb.push(line(1, true)); check(s);  // 首组去掉 2 格，剩下 5 格恰好一行。
    CHECK(s.history_display_rows() == 3 && sb.evicted() == 1);
    sb.push(line(1, true)); check(s);
    sb.push(line(1, true)); check(s);  // 首组最后一段被淘汰，整组移除。
    CHECK(s.history_display_rows() == 4);
    CHECK(s.debug_hist_rows_recompute_count == before);

    // 唯一开放组同时是首/尾组；空 soft 段（含默认 id=0）仍占一行。
    itcore::Screen open(5, 2, 3, 1ull << 20);
    check(open);
    before = open.debug_hist_rows_recompute_count;
    for (int i = 0; i < 30; ++i) {
        open.scrollback().push(line(i % 4, false, 0)); check(open);
    }
    CHECK(open.debug_hist_rows_recompute_count == before);
    open.scrollback().clear(); check(open);
    open.scrollback().push(line(0, false, 0)); check(open);
    CHECK(open.history_display_rows() == 1);

    // 字节限额可一次淘汰多段，甚至淘汰刚推入的唯一一段。
    itcore::Screen bytes(5, 2, 100, line(2, true).mem_bytes() * 3);
    check(bytes);
    before = bytes.debug_hist_rows_recompute_count;
    for (int i = 0; i < 20; ++i) {
        bytes.scrollback().push(line(i % 7, i % 3 == 0)); check(bytes);
    }
    bytes.scrollback().push(line(100, false)); check(bytes);
    CHECK(bytes.scrollback().empty() && bytes.history_display_rows() == 0);
    CHECK(bytes.debug_hist_rows_recompute_count == before);
}

static void test_invalidation() {
    Stream s(12, 3, 20);
    for (int i = 0; i < 30; ++i) s.feed("中文1234567890abcdef\r\n");
    s.check();
    auto before = s.recomputes();
    s.resize(7, 3);
    CHECK(s.recomputes() == before);
    s.check(); s.check();
    CHECK(s.recomputes() == before + 1);
    before = s.recomputes();
    s.resize(12, 3); s.resize(7, 3);  // 查询前来回 resize 仍须重建一次。
    s.check(); s.check();
    CHECK(s.recomputes() == before + 1);
    before = s.recomputes();
    s.resize(7, 4); s.check();
    CHECK(s.recomputes() == before);
    s.feed("\x1b[?1049h"); s.check();
    s.feed("\x1b[?1049l"); s.check();
    CHECK(s.recomputes() == before);
    for (const auto *clear : {"\x1b[3J", "\x1b" "c"}) {
        s.feed(clear);
        CHECK(s.recomputes() == before);
        s.check(); s.check();
        CHECK(itcore_history_rows(s.core) == 0 && s.recomputes() == before);
        s.feed("a\r\nb\r\nc\r\nd\r\ne\r\n"); s.check();
        before = s.recomputes();
    }

    // 池扫描只读；压缩只重映射 ID，宽度/CONTINUATION/边界均不变。
    itcore::Screen compact(5, 2, 10, 1ull << 20);
    auto l = line(7, false);
    l.cells[0].grapheme_id = compact.pool().intern("中");
    l.cells[0].width = 2;
    compact.scrollback().push(std::move(l));
    check(compact);
    before = compact.debug_hist_rows_recompute_count;
    const auto generation = compact.scrollback().generation();
    compact.compact_pool(); check(compact);
    CHECK(compact.scrollback().generation() > generation);
    CHECK(compact.debug_hist_rows_recompute_count == before);
    compact.scrollback().clear(); check(compact); check(compact);
    CHECK(compact.history_display_rows() == 0);
    CHECK(compact.debug_hist_rows_recompute_count == before);
}

static void test_random() {
    std::mt19937 rng(0x4e455732);
    for (int trial = 0; trial < 32; ++trial) {
        uint16_t cols = uint16_t(2 + rng() % 15);
        const uint32_t max_lines = 1 + rng() % 12;
        const uint64_t max_bytes = 128 + rng() % 2048;
        itcore::Screen s(cols, 2, max_lines, max_bytes);
        check(s);
        for (int op = 0; op < 250; ++op) {
            const auto before = s.debug_hist_rows_recompute_count;
            bool invalid = false;
            if (rng() % 13 == 0) {
                const auto next = uint16_t(2 + rng() % 15);
                invalid = next != cols;
                cols = next;
                s.resize(cols, 2);
            } else if (rng() % 71 == 0) {
                s.scrollback().clear();
            } else {
                const auto groups = s.scrollback().debug_hist_rows_group_recompute_count;
                const int pushes = 1 + rng() % 8;
                for (int p = 0; p < pushes; ++p) {
                    auto l = line(0, rng() % 3 == 0, uint64_t(op + 1));
                    const size_t n = rng() % 40;
                    for (size_t i = 0; i < n; ++i) {
                        auto c = itcore::Cell::blank();
                        c.grapheme_id = 1;
                        c.width = uint8_t(rng() % 3);  // 0 也按 reflow_line 的窄格规则计。
                        if (rng() % 4 == 0) c.flags = ITCORE_CELL_BOLD;
                        l.cells.push_back(c);
                        if (c.width == 2 && rng() % 2 == 0) {
                            c.width = 0; c.flags = ITCORE_CELL_CONTINUATION;
                            l.cells.push_back(c);
                        }
                    }
                    s.scrollback().push(std::move(l));
                }
                CHECK(s.scrollback().debug_hist_rows_group_recompute_count == groups);
            }
            CHECK(s.debug_hist_rows_recompute_count == before);
            const auto groups = s.scrollback().debug_hist_rows_group_recompute_count;
            check(s); check(s);
            CHECK(s.scrollback().debug_hist_rows_group_recompute_count <= groups + 2);
            CHECK(s.debug_hist_rows_recompute_count == before + (invalid ? 1 : 0));
            CHECK(s.scrollback().logical_count() <= max_lines);
            CHECK(s.scrollback().bytes() <= max_bytes);
        }
    }
}

int main(int argc, char **argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "stream") { test_stream(); test_lazy_groups(); }
    if (which == "all" || which == "eviction") test_eviction();
    if (which == "all" || which == "invalidation") test_invalidation();
    if (which == "all" || which == "random") test_random();
    return failures ? 1 : 0;
}
