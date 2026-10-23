// CORE-06 宽度表 / CORE-07 OSC 8 / CORE-08 快照增量引用字素池。
#include "itcore.h"
#include "ucd_width.h"
#include "screen.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make(uint16_t cols = 20, uint16_t rows = 5) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), cols, rows, 100, 1ull << 22};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static void feed(itcore_core *c, const std::string &s) {
    auto r = itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    CHECK(r.status == ITCORE_OK);
    while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e);
}
static const itcore_cell *row0(const itcore_snapshot *s) {
    itcore_wrap w; uint64_t rev; return itcore_snapshot_row(s, 0, &w, &rev);
}
static std::string g(const itcore_snapshot *s, uint32_t id) {
    size_t n = 0; const char *p = itcore_snapshot_grapheme(s, id, &n); return std::string(p, n);
}
template <class P> static std::string gs(const P &p, uint32_t id) {
    size_t n = 0; const char *q = p.get(id, &n); return std::string(q, n);
}
static uint16_t cursor_col(itcore_core *c) {
    auto *s = itcore_acquire_snapshot(c, 0, 1);
    uint16_t col, row; int vis; itcore_snapshot_cursor(s, &col, &row, &vis);
    itcore_snapshot_release(s); return col;
}

static void test_width_table() {
    using namespace itcore::ucd;
    CHECK(std::strlen(version()) > 0);
    CHECK(width('A') == 1);
    CHECK(width(0x4E2D) == 2);   // 中
    CHECK(width(0xAC00) == 2);   // 가
    CHECK(width(0xFF21) == 2);   // 全角 Ａ
    CHECK(width(0x0301) == 0);   // 组合尖音
    CHECK(width(0x1160) == 0);   // Hangul 中声
    CHECK(width(0x200D) == 0);   // ZWJ
    CHECK(width(0xFE0F) == 0);
    CHECK(width(0x00AD) == 0 && is_ignorable(0x00AD));  // 软连字符：Default_Ignorable
    CHECK(width(0x00B7) == 1 && width(0x00B7, true) == 2);  // 歧义
    CHECK(width(0x1F600) == 2);  // 😀 Emoji_Presentation → W
    CHECK(width(0x2764) == 1 && is_vs16_base(0x2764));  // ❤ 文本呈现
    CHECK(width(0x1F1E8) == 2);  // 孤立区域指示符（fullWidthFlags=YES）
    CHECK(width(0x20000) == 2);  // CJK 扩展 B
    CHECK(width(0x1FAE8) == 2 || width(0x1FAE8) == 1);
    CHECK(is_ext_pict(0x1F469) && is_modifier_base(0x1F44D) && is_modifier(0x1F3FB));
}

// IME-02：itcore_text_clusters 的簇切分与列宽 = itcore_feed 写进网格的结果（歧义宽度、emoji、组合、ZWJ、旗帜）
// 在 c 的当前屏幕上：itcore_text_clusters 的宽度 = feed 之后快照第 0 行的宽度；返回 widths
static std::vector<uint8_t> clusters_vs_grid(const char *t, bool alt) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 40, 2, 0, 0};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0);
    if (alt) { const char *e = "\x1b[?1049h"; itcore_feed(c, reinterpret_cast<const uint8_t *>(e), strlen(e)); }
    CHECK((itcore_mode_query(c, 1049) == ITCORE_MODE_SET) == alt);
    size_t len = strlen(t);
    size_t n = itcore_text_clusters(c, t, len, nullptr, nullptr, 0);   // 组字时：按当前屏幕
    std::vector<uint8_t> w(n); std::vector<uint32_t> bl(n);
    CHECK(itcore_text_clusters(c, t, len, w.data(), bl.data(), n) == n);
    size_t sum = 0; for (auto b : bl) sum += b;
    CHECK(sum == len);
    itcore_feed(c, reinterpret_cast<const uint8_t *>(t), len);           // 上屏
    itcore_snapshot *sn = itcore_acquire_snapshot(c, 0, 2);
    itcore_wrap wr; uint64_t rr;
    const itcore_cell *row = itcore_snapshot_row(sn, 0, &wr, &rr);
    std::vector<uint8_t> grid;
    for (int x = 0; x < 40; ++x)
        if (row[x].grapheme_id && !(row[x].flags & ITCORE_CELL_CONTINUATION)) grid.push_back(row[x].width);
    if (grid != w) std::printf("text_clusters 不一致（%s）：%s n=%zu grid=%zu\n", alt ? "备用屏" : "主屏", t, n, grid.size());
    CHECK(grid == w);
    itcore_snapshot_release(sn);
    itcore_destroy(c);
    return w;
}

// IME-02：itcore_text_clusters 的簇切分与列宽 = itcore_feed 在同一屏幕写进网格的结果（主屏与备用屏）
static void test_text_clusters() {
    const char *samples[] = {"a\u03b1\u2460b", "\U0001F44D\U0001F3FD", "\u2764\uFE0F", "e\u0301x", "\u6d4b\u8bd5",
                             "\U0001F468\u200D\U0001F469\u200D\U0001F467", "\U0001F1E8\U0001F1F3\U0001F1FA", "\u00ad\u200bq", "\xff!"};
    for (bool alt : {false, true})
        for (const char *t : samples) clusters_vs_grid(t, alt);
    // ❤️（U+2764 FE0F）：主屏 VS16 扩为 2，备用屏不扩（screen.cpp 写网格同规则）
    auto wm = clusters_vs_grid("\u2764\uFE0F", false), wa = clusters_vs_grid("\u2764\uFE0F", true);
    CHECK(wm.size() == 1 && wm[0] == 2);
    CHECK(wa.size() == 1 && wa[0] == 1);
    // 肤色修饰两屏都扩
    auto tm = clusters_vs_grid("\u261D\U0001F3FD", false), ta = clusters_vs_grid("\u261D\U0001F3FD", true);
    CHECK(tm == ta && tm.size() == 1 && tm[0] == 2);
    // 无 core 返回 0；只含可忽略码点返回 0 个簇；只写前 cap 个
    CHECK(itcore_text_clusters(nullptr, "abc", 3, nullptr, nullptr, 0) == 0);
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 40, 2, 0, 0};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0);
    CHECK(itcore_text_clusters(c, nullptr, 3, nullptr, nullptr, 0) == 0);
    CHECK(itcore_text_clusters(c, "\u200b\u00ad", strlen("\u200b\u00ad"), nullptr, nullptr, 0) == 0);
    uint8_t one[1]; CHECK(itcore_text_clusters(c, "abc", 3, one, nullptr, 1) == 3 && one[0] == 1);
    itcore_destroy(c);
}

static void test_width_screen() {
    auto *c = make();
    // ZWJ 家庭序列 → 单簇 2 格
    std::string fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    feed(c, fam + "x");
    auto *s = itcore_acquire_snapshot(c, 0, 1);
    auto *r = row0(s);
    CHECK(g(s, r[0].grapheme_id) == fam);
    CHECK(r[0].width == 2 && (r[1].flags & ITCORE_CELL_CONTINUATION));
    CHECK(g(s, r[2].grapheme_id) == "x");
    itcore_snapshot_release(s);
    // VS16：❤️ 主屏扩宽为 2，光标前进 2
    feed(c, "\r\x1b[2K\xE2\x9D\xA4\xEF\xB8\x8F|");
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(r[0].width == 2 && g(s, r[2].grapheme_id) == "|");
    itcore_snapshot_release(s);
    // 备用屏不扩宽（vs16Supported=NO）
    feed(c, "\x1b[?1049h\xE2\x9D\xA4\xEF\xB8\x8F|");
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(r[0].width == 1 && g(s, r[1].grapheme_id) == "|");
    itcore_snapshot_release(s);
    feed(c, "\x1b[?1049l");
    // 国旗：两个 RI 一簇 2 格；第三个 RI 另起
    feed(c, "\r\x1b[2K\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3\xF0\x9F\x87\xAF");
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(g(s, r[0].grapheme_id) == "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3");
    CHECK(r[0].width == 2 && r[2].width == 2);
    itcore_snapshot_release(s);
    CHECK(cursor_col(c) == 4);
    // 肤色修饰：☝🏻（窄基字）→ 2 格
    feed(c, "\r\x1b[2K\xE2\x98\x9D\xF0\x9F\x8F\xBB");
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(r[0].width == 2);
    itcore_snapshot_release(s);
    // 可忽略：软连字符/ZWSP 不占格；组合字符并入
    feed(c, "\r\x1b[2Ka\xC2\xAD\xE2\x80\x8B" "e\xCC\x81");
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(g(s, r[0].grapheme_id) == "a" && g(s, r[1].grapheme_id) == "e\xCC\x81");
    itcore_snapshot_release(s);
    CHECK(cursor_col(c) == 2);
    // 组合字符上限：100 个尖音只保留到 32 码点
    std::string many = "o"; for (int i = 0; i < 100; ++i) many += "\xCC\x81";
    feed(c, "\r\x1b[2K" + many);
    s = itcore_acquire_snapshot(c, 0, 1); r = row0(s);
    CHECK(g(s, r[0].grapheme_id).size() == 1 + 31 * 2);
    itcore_snapshot_release(s);
    itcore_destroy(c);
}

static void test_osc8() {
    auto *c = make();
    feed(c, "\x1b]8;id=a1:foo=bar;https://example.com/x\x1b\\AB\x1b[0m\xE4\xB8\xAD\x1b]8;;\x07" "C");
    feed(c, "\x1b]8;;https://example.com/x\x07" "D\x1b]8;;\x07");
    auto *s = itcore_acquire_snapshot(c, 0, 1);
    auto *r = row0(s);
    CHECK(r[0].hyperlink_id && r[0].hyperlink_id == r[1].hyperlink_id);
    CHECK(r[2].hyperlink_id == r[0].hyperlink_id && r[3].hyperlink_id == r[0].hyperlink_id);  // SGR 0 不结束；宽字尾格带链接
    CHECK(r[4].hyperlink_id == 0);
    CHECK(r[5].hyperlink_id && r[5].hyperlink_id != r[0].hyperlink_id);  // 同 URL 不同 id → 不同链接
    const char *u, *i; size_t ul, il;
    CHECK(itcore_snapshot_hyperlink(s, r[0].hyperlink_id, &u, &ul, &i, &il) == 1);
    CHECK(std::string(u, ul) == "https://example.com/x" && std::string(i, il) == "a1");
    CHECK(itcore_snapshot_hyperlink(s, r[5].hyperlink_id, &u, &ul, &i, &il) == 1 && il == 0);
    CHECK(itcore_snapshot_hyperlink(s, 0, &u, &ul, &i, &il) == 0);
    CHECK(itcore_snapshot_hyperlink(s, r[0].grapheme_id, &u, &ul, &i, &il) == 0);  // 字素 id 不是链接
    itcore_snapshot_release(s);
    // 同 URL+id 复用；无分号的 OSC 8 忽略（上游 executeLink）
    feed(c, "\r\n\x1b]8;id=a1;https://example.com/x\x07Z\x1b]8\x07Y\x1b]8;;\x07");
    s = itcore_acquire_snapshot(c, 1, 1); r = row0(s);
    auto *s0 = itcore_acquire_snapshot(c, 0, 1);
    CHECK(r[0].hyperlink_id == row0(s0)[0].hyperlink_id);
    CHECK(r[1].hyperlink_id == r[0].hyperlink_id);
    itcore_snapshot_release(s0); itcore_snapshot_release(s);
    itcore_destroy(c);
}

static void test_snapshot_pool() {
    // 1) 引用而非拷贝：快照后新增字素对旧快照不可见，旧 id 保持可读
    {
        itcore::GraphemePool p;
        uint32_t a = p.intern("a");
        auto ref = p.ref();
        uint32_t b = p.intern("b");
        size_t n; CHECK(gs(ref, a) == "a");
        CHECK(ref.get(b, &n)[0] == 0 && n == 0);  // 超出 limit
        CHECK(gs(p, b) == "b");
        uint64_t ep = p.epoch();
        p.clear();  // 换新存储
        CHECK(p.epoch() == ep + 1);
        CHECK(gs(ref, a) == "a");  // 旧快照仍持有旧存储
        CHECK(ref.store.use_count() == 1);
        // 跨分块
        for (uint32_t k = 0; k < itcore::PoolStore::kChunk * 2 + 5; ++k) p.intern(std::to_string(k));
        auto r2 = p.ref();
        CHECK(r2.limit == itcore::PoolStore::kChunk * 2 + 6);
        CHECK(gs(r2, r2.limit - 1) == std::to_string(itcore::PoolStore::kChunk * 2 + 4));
    }
    // 2) 快照在核心销毁后仍有效
    {
        auto *c = make();
        feed(c, "\xE4\xB8\xAD" "e\xCC\x81");
        auto *s = itcore_acquire_snapshot(c, 0, 1);
        itcore_destroy(c);
        auto *r = row0(s);
        CHECK(g(s, r[0].grapheme_id) == "\xE4\xB8\xAD" && g(s, r[2].grapheme_id) == "e\xCC\x81");
        itcore_snapshot_release(s);
    }
    // 3) 并发：写端持续 intern（跨多个分块），读端反复读已发布 ref
    {
        itcore::GraphemePool p;
        std::atomic<bool> stop{false};
        std::atomic<int> bad{0};
            std::atomic<itcore::PoolRef *> latest{nullptr};
        std::vector<std::unique_ptr<itcore::PoolRef>> keep;
        keep.reserve(4096);
        std::thread reader([&] {
            while (!stop.load()) {
                auto *r = latest.load(std::memory_order_acquire);
                if (!r) continue;
                for (uint32_t id = 1; id < r->limit; id += 97) {
                    size_t n; const char *q = r->get(id, &n);
                    if (std::string(q, n) != "g" + std::to_string(id)) bad++;
                }
            }
        });
        for (uint32_t k = 1; k < 20000; ++k) {
            CHECK(p.intern("g" + std::to_string(k)) == k);
            if (k % 50 == 0) {
                keep.push_back(std::make_unique<itcore::PoolRef>(p.ref()));
                latest.store(keep.back().get(), std::memory_order_release);
            }
        }
        stop = true; reader.join();
        CHECK(bad.load() == 0);
    }
    // 4) generation：resize 后新快照 generation 递增，旧快照不变
    {
        auto *c = make();
        feed(c, "abc");
        auto *s1 = itcore_acquire_snapshot(c, 0, 1);
        uint64_t g1 = itcore_snapshot_generation(s1);
        itcore_resize(c, 30, 5, 7);
        feed(c, "\xF0\x9F\x98\x80");
        auto *s2 = itcore_acquire_snapshot(c, 0, 1);
        CHECK(itcore_snapshot_generation(s2) == 7 && itcore_snapshot_generation(s1) == g1);
        CHECK(itcore_snapshot_columns(s1) == 20 && itcore_snapshot_columns(s2) == 30);
        CHECK(g(s1, row0(s1)[0].grapheme_id) == "a");
        itcore_snapshot_release(s1); itcore_snapshot_release(s2);
        itcore_destroy(c);
    }
}


static uint64_t rowrev(const itcore_snapshot *s, uint16_t y) {
    itcore_wrap w; uint64_t rev = 0; itcore_snapshot_row(s, y, &w, &rev); return rev;
}
// CORE-09：行 revision 全局单调 + 快照 epoch
static void test_revision_epoch() {
    // 1) 滚动后被复用的行位置 revision 必须严格变大
    {
        auto *c = make(10, 3);
        feed(c, "a\r\nb\r\nc");
        auto *s1 = itcore_acquire_snapshot(c, 0, 3);
        uint64_t mx = 0; for (uint16_t y = 0; y < 3; ++y) mx = std::max(mx, rowrev(s1, y));
        feed(c, "\r\nd");  // 滚动一行
        auto *s2 = itcore_acquire_snapshot(c, 0, 3);
        CHECK(rowrev(s2, 2) > mx);
        CHECK(rowrev(s2, 2) != rowrev(s1, 2));
        // SU / IL / DL 新行同样取新值
        feed(c, "\x1b[S"); auto *s3 = itcore_acquire_snapshot(c, 0, 3);
        CHECK(rowrev(s3, 2) > rowrev(s2, 2));
        feed(c, "\x1b[1;1H\x1b[L"); auto *s4 = itcore_acquire_snapshot(c, 0, 3);
        CHECK(rowrev(s4, 0) > rowrev(s3, 2));
        feed(c, "\x1b[M"); auto *s5 = itcore_acquire_snapshot(c, 0, 3);
        CHECK(rowrev(s5, 2) > rowrev(s4, 0));
        // resize 保留原 revision
        uint64_t r0 = rowrev(s5, 0);
        itcore_resize(c, 12, 3, 2);
        auto *s6 = itcore_acquire_snapshot(c, 0, 3);
        CHECK(rowrev(s6, 0) == r0);
        for (auto *s : {s1, s2, s3, s4, s5, s6}) itcore_snapshot_release(s);
        itcore_destroy(c);
    }
    // 2) 清池后所有行（主/备用屏）revision 推进
    {
        itcore::Screen sc(10, 3, 100, 1 << 22);
        uint64_t before[3], abefore[3];
        for (int y = 0; y < 3; ++y) before[y] = sc.grid().row(y).revision;
        uint64_t mx = 0; for (int y = 0; y < 3; ++y) mx = std::max(mx, before[y]);
        sc.reset_pool_for_test();
        for (int y = 0; y < 3; ++y) CHECK(sc.grid().row(y).revision > mx);
        (void)abefore;
    }
    // 3) epoch 读出：RIS 后递增，旧快照不变
    {
        auto *c = make();
        feed(c, "x");
        auto *s1 = itcore_acquire_snapshot(c, 0, 1);
        uint64_t e1 = itcore_snapshot_epoch(s1);
        CHECK(e1 >= 1);
        feed(c, "\x1b" "c");
        auto *s2 = itcore_acquire_snapshot(c, 0, 1);
        CHECK(itcore_snapshot_epoch(s2) == e1 + 1);
        CHECK(itcore_snapshot_epoch(s1) == e1);
        CHECK(itcore_snapshot_epoch(nullptr) == 0);
        CHECK(row0(s2)[0].grapheme_id == 0);
        CHECK(g(s1, row0(s1)[0].grapheme_id) == "x");
        itcore_snapshot_release(s1); itcore_snapshot_release(s2);
        itcore_destroy(c);
    }
}

static std::string dump(itcore::Screen &sc) {
    std::string out;
    for (int y = 0; y < sc.grid().rows(); ++y) {
        for (auto &cell : sc.grid().row(y).cells) {
            size_t n; const char *p = sc.pool().get(cell.grapheme_id, &n);
            out.append(p, n); out += '|';
            if (cell.hyperlink_id) {
                std::string_view u, i;
                const char *q = sc.pool().get(cell.hyperlink_id, &n);
                if (itcore::GraphemePool::decode_hyperlink(q, n, &u, &i)) { out += '<'; out += u; out += '>'; }
            }
        }
        out += '\n';
    }
    return out;
}
struct VecSink : itcore::TokenSink {
    std::vector<itcore::Token> v;
    void token(const itcore::Token &t) override { v.push_back(t); }
};
static void feed_screen(itcore::Screen &sc, itcore::Parser &p, const std::string &s) {
    VecSink k;
    p.feed(reinterpret_cast<const uint8_t *>(s.data()), s.size(), k);
    p.flush_text(k);
    for (auto &t : k.v) sc.apply(t, nullptr);
}
// CORE-10：字素池压缩 / 池满 U+FFFD
static void test_compact() {
    // 1) 池满返回预留 U+FFFD id
    {
        itcore::GraphemePool p;
        p.set_capacity_for_test(100);
        uint32_t last = 0;
        for (uint32_t k = 1; k + 1 < 100; ++k) last = p.intern("q" + std::to_string(k));
        CHECK(last == 98);
        uint32_t f = p.intern("overflow-1");
        CHECK(f != 0 && f == p.replacement_id());
        CHECK(gs(p, f) == "\xEF\xBF\xBD");
        CHECK(p.intern("overflow-2") == f);
        CHECK(p.intern_hyperlink("http://x", "") == f);
    }
    // 2) 长会话（ABI 层）：每行一个唯一 OSC 8 链接；压缩前的旧快照仍有效
    {
        auto *c = make(20, 5);
        itcore_snapshot *early = nullptr;
        for (int k = 0; k < 30000; ++k) {
            feed(c, "\xE4\xB8\xAD\x1b]8;;http://h/" + std::to_string(k) + "\x1b\\L\x1b]8;;\x1b\\\r\n");
            if (k == 10) early = itcore_acquire_snapshot(c, -1, 5);
        }
        auto *late = itcore_acquire_snapshot(c, 0, 1);
        CHECK(itcore_snapshot_epoch(late) > itcore_snapshot_epoch(early));
        CHECK(early && g(early, row0(early)[0].grapheme_id) == "\xE4\xB8\xAD");
        char url[64]; size_t ul = 0;
        (void)url; (void)ul;
        itcore_snapshot_release(early); itcore_snapshot_release(late);
        itcore_destroy(c);
    }
    {
        itcore::Screen sc(20, 5, 50, 1 << 20);
        sc.set_pool_budget(64 << 10);
        itcore::Parser par;
        size_t peak = 0;
        for (int k = 0; k < 50000; ++k) {
            sc.set_hyperlink("", "http://h/" + std::to_string(k));
            feed_screen(sc, par, "\xF0\x9F\x91\x8D" + std::string(k % 2 ? "\xF0\x9F\x8F\xBB" : "\xF0\x9F\x8F\xBF") + "e\xCC\x81L");
            sc.set_hyperlink("", "");
            feed_screen(sc, par, "\r\n");
            peak = std::max(peak, sc.pool().bytes());
        }
        CHECK(sc.compactions() > 0);
        CHECK(peak <= sc.pool_budget());
        // 压缩后内容不变
        std::string before = dump(sc);
        auto old = sc.pool().ref();
        uint32_t gid0 = sc.grid().row(0).cells[0].grapheme_id;
        std::string g0 = gs(old, gid0);
        uint64_t mx = 0; for (int y = 0; y < 5; ++y) mx = std::max(mx, sc.grid().row(y).revision);
        uint64_t ep = sc.pool().epoch();
        sc.compact_pool();
        CHECK(dump(sc) == before);
        CHECK(sc.pool().epoch() == ep + 1);
        for (int y = 0; y < 5; ++y) CHECK(sc.grid().row(y).revision > mx);
        CHECK(gs(old, gid0) == g0);  // 旧引用仍有效
        // bytes_ 精确：等于存活条目之和
        size_t sum = 0;
        for (uint32_t id = 1; id < sc.pool().size(); ++id) { size_t n; sc.pool().get(id, &n); sum += itcore::GraphemePool::entry_bytes(n); }
        CHECK(sc.pool().bytes() == sum);
        // ED 3 触发压缩
        uint64_t n0 = sc.compactions();
        feed_screen(sc, par, "\x1b[3J");
        CHECK(sc.compactions() == n0 + 1);
    }
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "width") { test_width_table(); test_width_screen(); test_text_clusters(); }
    if (w == "all" || w == "osc8") test_osc8();
    if (w == "all" || w == "pool") test_snapshot_pool();
    if (w == "all" || w == "rev") test_revision_epoch();
    if (w == "all" || w == "compact") test_compact();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
