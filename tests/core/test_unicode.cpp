// CORE-06 宽度表 / CORE-07 OSC 8 / CORE-08 快照增量引用字素池。
#include "itcore.h"
#include "ucd_width.h"
#include "screen.h"
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

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "all" || w == "width") { test_width_table(); test_width_screen(); }
    if (w == "all" || w == "osc8") test_osc8();
    if (w == "all" || w == "pool") test_snapshot_pool();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
