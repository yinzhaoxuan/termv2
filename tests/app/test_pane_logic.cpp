// T08 PaneBackend 平台无关逻辑单测（PANE-01）
#include "pane_logic.h"
#include "frame_pump.h"
#include "pane_search.h"
#include "grid_metrics.h"
#include <deque>
#include <optional>
#include "frame_policy.h"
#include "itcore.h"
#include "iswap_chain_host.h"
#include <atomic>
#include <climits>
#include <limits>
#include <chrono>
#include <memory>
#include <cstdio>
#include <thread>
#include <string>
#include <cstring>
#include <functional>
#include <vector>
using namespace it2app;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static void limits() {
    RateLimiter bell(1000);
    CHECK(bell.allow(0)); CHECK(!bell.allow(500)); CHECK(!bell.allow(999)); CHECK(bell.allow(1000)); CHECK(!bell.allow(1999));
    int n = 0; RateLimiter b2(1000);
    for (uint64_t t = 0; t < 10000; t += 7) n += b2.allow(t);
    CHECK(n == 10);
    TitleThrottle tt(100);
    CHECK(tt.offer("a", 0) == std::optional<std::string>("a"));
    CHECK(!tt.offer("b", 10)); CHECK(!tt.offer("c", 20)); CHECK(tt.pending());
    CHECK(!tt.flush(50));
    CHECK(tt.flush(100) == std::optional<std::string>("c")); CHECK(!tt.pending());
}
static void grid() {
    CHECK(clampGrid(3, 1).cols == 20 && clampGrid(3, 1).rows == 5);
    CHECK(clampGrid(-5, -5).cols == 20);
    CHECK(clampGrid(120, 40).cols == 120 && clampGrid(120, 40).rows == 40);
    ResizeTracker r({80, 24});
    CHECK(r.generation() == 1);
    CHECK(!r.update(80, 24));
    CHECK(r.update(100, 30) == std::optional<uint64_t>(2));
    CHECK(r.update(1, 1) == std::optional<uint64_t>(3));
    CHECK(!r.update(10, 2));  // 都钳到 20×5，不增 generation
    CHECK(r.dims().cols == 20 && r.dims().rows == 5);
}
static void queue() {
    std::atomic<int> wakes{0};
    UiDispatchQueue q([&] { ++wakes; });
    int ran = 0;
    q.post([&] { ++ran; }); q.post([&] { ++ran; });
    CHECK(wakes == 1);  // 只在空→非空时唤醒
    CHECK(q.drain() == 2 && ran == 2);
    // 回调里再 post：留到下一次 drain
    q.post([&] { q.post([&] { ++ran; }); });
    CHECK(q.drain() == 1 && ran == 2 && q.size() == 1);
    CHECK(q.drain() == 1 && ran == 3);
    // 多线程 post，UI 线程顺序执行
    std::vector<std::thread> ts; std::atomic<int> sum{0}; int uiSum = 0;
    for (int i = 0; i < 4; ++i) ts.emplace_back([&] { for (int k = 0; k < 1000; ++k) q.post([&] { ++uiSum; }); ++sum; });
    for (auto &t : ts) t.join();
    q.drain();
    CHECK(uiSum == 4000);
    q.close();
    CHECK(!q.post([&] { ++ran; })); CHECK(q.drain() == 0);
    // 先 post 后设 wake：setWake 补唤醒
    UiDispatchQueue q2; int w2 = 0; q2.post([] {}); q2.setWake([&] { ++w2; }); CHECK(w2 == 1);
}
// R1：requestClose 不阻塞；结果只经 UI 队列送达；多会话并行；销毁后不回调
using Clock = std::chrono::steady_clock;
static long msSince(Clock::time_point t) {
    return long(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}
// 模拟一个窗格：与 Win32PaneBackend 同构——共享状态 + 关闭线程 + 析构先断回调再 close 队列
struct FakeSession {
    std::shared_ptr<SessionShared> sh = std::make_shared<SessionShared>();
    SessionCloser closer;
    std::atomic<int> *jobsDone;
    std::thread::id uiThread = std::this_thread::get_id();
    std::function<void(const CloseOutcome &)> onStatus;  // 代表 PaneEvents
    bool statusDelivered = false, statusOnUi = false, timedOut = false;
    explicit FakeSession(std::atomic<int> *d) : jobsDone(d) {
        sh->transport = std::make_shared<int>(7);  // 代表传输层对象
        onStatus = [this](const CloseOutcome &r) {
            statusDelivered = true;
            statusOnUi = std::this_thread::get_id() == uiThread;
            timedOut = r.timedOut;
        };
    }
    // 传输回调线程：经 weak_ptr 投递不捕获 this 的任务
    void transportEvent() {
        std::weak_ptr<SessionShared> w = sh;
        if (auto s = w.lock()) postTo<FakeSession>(*s->queue, [](FakeSession &p) { p.events++; });
    }
    int events = 0;
    void requestClose(int sleepMs, bool timeout = false) {
        closer.start(sh, [sleepMs, timeout, d = jobsDone](SessionShared &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            d->fetch_add(1);
            CloseOutcome r; r.ok = !timeout; r.timedOut = timeout;
            return r;
        }, [](SessionShared &s) { s.transport.reset(); });  // 传输层在关闭线程上析构
    }
    void drain() {
        sh->queue->drain(this);
        if (auto r = sh->takeCloseResult()) if (onStatus) onStatus(*r);
    }
    ~FakeSession() {
        onStatus = nullptr;          // 先断开回调
        sh->queue->close();          // 之后投递的事件一律丢弃
        if (!closer.started()) requestClose(1);
    }
};
static bool pumpUntil(std::vector<FakeSession *> ss, long budgetMs) {
    auto t0 = Clock::now();
    for (;;) {
        bool all = true;
        for (auto *s : ss) { s->drain(); all = all && s->statusDelivered; }
        if (all) return true;
        if (msSince(t0) > budgetMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static void closer() {
    std::atomic<int> done{0};
    int expect = 0;
    {
        FakeSession s(&done);
        auto t0 = Clock::now();
        s.requestClose(150); ++expect;
        CHECK(msSince(t0) < 50);               // 立即返回
        s.requestClose(150);                   // 重复请求无效
        CHECK(!s.statusDelivered);             // 不在调用线程同步回调
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!s.statusDelivered);             // 未 drain 前不回调（只经 UI 队列）
        CHECK(pumpUntil({&s}, 2000));
        CHECK(s.statusOnUi && !s.timedOut);
        s.transportEvent(); s.drain();
        CHECK(s.events == 1);
    }
    {   // 超时强杀路径
        FakeSession s(&done);
        s.requestClose(10, true); ++expect;
        CHECK(pumpUntil({&s}, 2000) && s.timedOut);
    }
    {   // 8 个会话并行关闭：总耗时 ≈ 单个，而非 8 倍
        std::vector<std::unique_ptr<FakeSession>> v;
        std::vector<FakeSession *> raw;
        for (int i = 0; i < 8; ++i) { v.push_back(std::make_unique<FakeSession>(&done)); raw.push_back(v.back().get()); }
        auto t0 = Clock::now();
        for (auto *s : raw) { s->requestClose(200); ++expect; }
        CHECK(msSince(t0) < 50);
        CHECK(pumpUntil(raw, 3000));
        long total = msSince(t0);
        CHECK(total < 800);
        for (auto *s : raw) CHECK(s->statusOnUi);
    }
    {   // 关闭中销毁：析构立即返回，结果与之后的事件都被丢弃
        int before = done.load();
        auto t0 = Clock::now();
        std::shared_ptr<SessionShared> sh;
        {
            FakeSession s(&done);
            sh = s.sh;
            s.requestClose(150); ++expect;
        }
        CHECK(msSince(t0) < 50);
        CHECK(!postTo<FakeSession>(*sh->queue, [](FakeSession &p) { p.events++; }));
        auto t1 = Clock::now();
        while (done.load() == before && msSince(t1) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(done.load() == before + 1);      // 关闭仍在后台跑完
        CHECK(SessionCloser::shutdownAll(std::chrono::milliseconds(3000)));
        CHECK(sh->queue->drain() == 0);        // 结果被丢弃
        CHECK(sh.use_count() == 1 && !sh->transport);
    }
    CHECK(SessionCloser::shutdownAll(std::chrono::milliseconds(3000)));
    CHECK(done.load() == expect);
    CHECK(SessionCloser::liveThreads() == 0);
}
// 创建后立即销毁 8 个窗格，循环 100 次：不崩溃、不残留线程（ASan/TSan 下同样跑）
static void churn() {
    std::atomic<int> done{0};
    for (int round = 0; round < 100; ++round) {
        std::vector<std::unique_ptr<FakeSession>> v;
        for (int i = 0; i < 8; ++i) v.push_back(std::make_unique<FakeSession>(&done));
        // 一半先 requestClose 再销毁，一半直接销毁（析构里启动关闭）；期间有传输线程在投递
        for (int i = 0; i < 8; i += 2) v[size_t(i)]->requestClose(0);
        std::vector<std::weak_ptr<SessionShared>> ws;
        for (auto &s : v) ws.push_back(s->sh);
        std::thread producer([&ws] {
            for (int k = 0; k < 50; ++k)
                for (auto &w : ws) if (auto s = w.lock()) postTo<FakeSession>(*s->queue, [](FakeSession &p) { p.events++; });
        });
        v.clear();
        producer.join();
        for (auto &w : ws) if (auto s = w.lock()) CHECK(s->queue->closed());
    }
    CHECK(SessionCloser::shutdownAll(std::chrono::milliseconds(3000)));
    CHECK(SessionCloser::liveThreads() == 0);
    CHECK(done.load() == 800);
    // 超时路径：卡住的关闭线程 → shutdownAll 返回 false 而不是挂死
    {
        auto sh = std::make_shared<SessionShared>();
        SessionCloser c;
        std::atomic<bool> release{false};
        c.start(sh, [&release](SessionShared &) {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return CloseOutcome{};
        });
        auto t0 = Clock::now();
        CHECK(!SessionCloser::shutdownAll(std::chrono::milliseconds(100)));
        CHECK(msSince(t0) < 1000);
        CHECK(SessionCloser::liveThreads() == 1);
        release = true;
        CHECK(SessionCloser::shutdownAll(std::chrono::milliseconds(3000)));
    }
}
static void viewport() {
    Viewport vp;
    CHECK(!vp.scroll(-5));                 // 无历史
    vp.updateHistory(10);
    CHECK(vp.offset() == 0);               // 在底部时跟随输出
    CHECK(vp.scroll(-3) && vp.offset() == 3 && vp.firstRow() == -3);
    CHECK(vp.scroll(-100) && vp.offset() == 10);  // 截断
    CHECK(!vp.scroll(-1));
    CHECK(vp.scroll(4) && vp.offset() == 6);
    CHECK(vp.updateHistory(12) && vp.offset() == 8);  // 上翻时输出不挪动视图
    CHECK(vp.updateHistory(2) && vp.offset() == 2);   // 历史被裁剪
    CHECK(vp.toBottom() && vp.offset() == 0);
    CHECK(!vp.toBottom());
    CHECK(vp.scroll(INT_MIN) && vp.offset() == 2);   // 一次滚到顶：截断到历史行数
    CHECK(!vp.scroll(INT_MIN) && vp.offset() == 2);
    CHECK(vp.scroll(INT_MAX) && vp.offset() == 0);   // 一次滚到底
    CHECK(!vp.scroll(INT_MAX));
    vp.updateHistory(INT_MAX);                       // 历史极大时也不溢出
    CHECK(vp.scroll(INT_MIN) && vp.offset() == INT_MAX);
    CHECK(!vp.updateHistory(INT_MAX) && vp.offset() == INT_MAX);
    CHECK(vp.scroll(INT_MAX) && vp.offset() == 0);
    vp.updateHistory(2);
    CHECK(routeWheel(true, false) == WheelRoute::Forward);
    CHECK(routeWheel(true, true) == WheelRoute::Forward);
    CHECK(routeWheel(false, true) == WheelRoute::Arrows);
    CHECK(routeWheel(false, false) == WheelRoute::Scroll);
    CHECK(applyNewlineMode("\r", true) == "\r\n");
    CHECK(applyNewlineMode("\x1b\r", true) == "\x1b\r\n");
    CHECK(applyNewlineMode("\r", false) == "\r");
    CHECK(applyNewlineMode("a\rb", true) == "a\rb");
    CHECK(applyNewlineMode("\x1b[A", true) == "\x1b[A");
    ClickCounter cc;
    CHECK(cc.press(1, 1, 0, 500) == 1);
    CHECK(cc.press(1, 1, 100, 500) == 2);
    CHECK(cc.press(1, 1, 200, 500) == 3);
    CHECK(cc.press(1, 1, 300, 500) == 1);
    CHECK(cc.press(1, 1, 1000, 500) == 1);  // 超时
    CHECK(cc.press(2, 1, 1100, 500) == 1);  // 换格
}
static void drag() {
    DragSelect d;
    using A = DragSelect::Act;
    CHECK(d.move({1, 1}) == A::None);          // 未按下
    d.down({2, 0});
    CHECK(d.move({2, 0}) == A::None);          // 同格抖动不建选区
    CHECK(d.move({3, 0}) == A::Start);
    CHECK(d.pressCell() == (DragSelect::Cell{2, 0}));
    CHECK(d.move({5, 1}) == A::Extend);
    CHECK(d.up({5, 1}) == A::Finish);
    CHECK(!d.pressed());
    d.down({4, 4});
    CHECK(d.up({4, 4}) == A::Click);           // 单击：清选区 / 链接
    d.down({4, 4});
    CHECK(d.up({6, 4}) == A::Finish);          // 无 move 直接异格松开也算选择
    d.down({1, 1}); d.cancel();
    CHECK(d.up({1, 1}) == A::None);
}
// PANE-04：sendKey 决策（VK_PROCESSKEY 忽略、AltGr 文本、Alt/Ctrl 字符附 base、修饰键位映射）
static void keys() {
    using it2app::planKey; using it2app::KeyPlan;
    CHECK(planKey(0xE5, 0, 'a', 'a').kind == KeyPlan::Ignore);
    auto g = planKey('Q', 2 | 4, '@', 'q');                 // 德语 AltGr+Q
    CHECK(g.kind == KeyPlan::Text && g.text == "@");
    auto e = planKey('E', 2 | 4, 0x20AC, 'e');              // AltGr+E → €
    CHECK(e.kind == KeyPlan::Text && e.text == "\xE2\x82\xAC");
    auto ca = planKey('C', 2 | 4, 0, 'c');                  // 真 Ctrl+Alt+C：无翻译 → 按键
    CHECK(ca.kind == KeyPlan::Key && ca.coreMods == (2u | 4u) && ca.unicode == 'c');
    auto a = planKey('X', 2, 'x', 'x');
    CHECK(a.kind == KeyPlan::Key && a.coreMods == 4u && a.unicode == 'x');
    auto c = planKey('A', 4, 1, 'a');
    CHECK(c.kind == KeyPlan::Key && c.coreMods == 2u && c.unicode == 'a');
    auto up = planKey(0x26, 1 | 4, 0, 0);
    CHECK(up.kind == KeyPlan::Key && up.vk == 0x26 && up.coreMods == (1u | 2u) && up.unicode == 0);
    auto f = planKey(0x70, 0, 0, 0);
    CHECK(f.kind == KeyPlan::Key && f.coreMods == 0);
    CHECK(planKey('A', 0, 'a', 'a').unicode == 0);          // 无修饰的字符键由 WM_CHAR 走 sendInput
    CHECK(it2app::utf8Encode(0x1F600) == "\xF0\x9F\x98\x80" && it2app::utf8Encode(0xD800).empty());
    CHECK(it2app::kPrevAttemptTextBytes == 8u * 1024u * 1024u);
}

static void syncfocus() {
    it2app::SyncGate g(ITREN_SYNC_TIMEOUT_MS);
    CHECK(ITREN_SYNC_TIMEOUT_MS == 150);
    CHECK(g.allow(false, 0));
    CHECK(!g.allow(true, 1000));          // 打开：暂停
    CHECK(!g.allow(true, 1149));
    CHECK(g.allow(true, 1150));           // 150ms 兜底帧
    CHECK(!g.allow(true, 1200));          // 重新计时
    CHECK(g.allow(false, 1201));          // 关闭（含软复位）立即出帧
    CHECK(!g.holding());
    {   // 同一批“关→内容→开”：轮询时 ?2026 仍是开，但同步结束计数变化 → 出一帧并重新计时
        itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 20, 5, 100, 1u << 20};
        itcore_core *c = nullptr;
        CHECK(itcore_create(&o, &c) == 0 && c);
        auto feed = [&](const char *t) { itcore_feed(c, reinterpret_cast<const uint8_t *>(t), std::strlen(t));
                                         while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e); };
        it2app::SyncGate sg(ITREN_SYNC_TIMEOUT_MS);
        auto on = [&] { return itcore_mode_query(c, 2026) == ITCORE_MODE_SET; };
        feed("\x1b[?2026h");
        CHECK(!sg.allow(on(), 0, itcore_sync_end_count(c)));
        feed("\x1b[?2026lframe1\x1b[?2026h");
        CHECK(on() && itcore_sync_end_count(c) == 1);
        CHECK(sg.allow(on(), 10, itcore_sync_end_count(c)));    // 出一帧
        CHECK(!sg.allow(on(), 20, itcore_sync_end_count(c)));   // 计数未变：继续暂停
        CHECK(!sg.allow(on(), 159, itcore_sync_end_count(c)));  // 计时从 10 重新开始
        CHECK(sg.allow(on(), 160, itcore_sync_end_count(c)));   // 超时只是出一帧
        CHECK(on());                                            // 不清同步状态
        feed("\x1b[!p\x1b[?2026h");                          // DECSTR 也计一次
        CHECK(itcore_sync_end_count(c) == 2);
        feed("\x1b" "c");                                      // RIS 关掉同步也计
        CHECK(itcore_sync_end_count(c) == 3 && !on());
        // SYNC-02：tmux/neovim 式每帧“关再开”，16ms 一批持续 1 秒 → 出帧数≈输入帧数（不是 1000/150≈6）
        feed("\x1b[?2026h");
        int frames = 0, batches = 0;
        for (uint64_t t = 1000; t < 2000; t += 16, ++batches) {
            feed("\x1b[?2026lxx\x1b[?2026h");
            if (sg.allow(on(), t, itcore_sync_end_count(c))) ++frames;
        }
        CHECK(frames == batches && batches >= 60);
        itcore_destroy(c);
    }
    {
        // SYNC-02 端到端：与 Win32PaneBackend::pump()/present() 同一段 pump_core + frame_due，计数 present
        itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 100, 1u << 20};
        itcore_core *c = nullptr;
        CHECK(itcore_create(&o, &c) == 0 && c);
        struct Chunk { std::string bytes; };
        std::deque<Chunk> q;
        auto pop = [&]() -> std::optional<Chunk> { if (q.empty()) return std::nullopt; Chunk k = std::move(q.front()); q.pop_front(); return k; };
        it2app::SyncGate gate(150);
        int presents = 0, events = 0;
        auto pumpOnce = [&](uint64_t now) {
            auto r = it2app::pump_core(c, pop, [&] { ++events; });
            if ((r == it2app::PumpResult::Fed || r == it2app::PumpResult::FedMore) && it2app::frame_due(gate, c, now, false)) ++presents;
        };
        q.push_back({"\x1b[?2026h"}); pumpOnce(0);
        int batches = 0; presents = 0;
        for (uint64_t t = 16; t <= 1000; t += 16, ++batches) {
            q.push_back({"\x1b[?2026lframe\r\n\x1b[?2026h"});
            pumpOnce(t);
        }
        CHECK(presents == batches && batches >= 60 && events > 0);
        // 对照：一直开着同步、无结束 → 只有 150ms 兜底帧
        presents = 0;
        for (uint64_t t = 2000; t < 3000; t += 16) { q.push_back({"x"}); pumpOnce(t); }
        CHECK(presents >= 5 && presents <= 7);
        // 无输出不出帧；改尺寸 force 总放行
        CHECK(it2app::pump_core(c, pop, [] {}) == it2app::PumpResult::Idle);
        CHECK(it2app::frame_due(gate, c, 3001, true));
        itcore_destroy(c);
    }
    it2app::FocusReporter f;
    CHECK(f.update(false, true).empty());
    CHECK(f.update(true, true).empty());  // 状态未变
    CHECK(f.update(true, false) == "\x1b[O");
    CHECK(f.update(true, true) == "\x1b[I");
    CHECK(f.update(false, false).empty());
}

// FONT-01：setFont 参数归一与格尺寸随字号变化
static void font() {
    using namespace it2app;
    FontSpec cur{L"Consolas", 12.f}, n;
    CHECK(!resolveFontChange(cur, "", 12.f, &n) && n.family == L"Consolas");       // 无变化
    CHECK(resolveFontChange(cur, "", 14.f, &n) && n.family == L"Consolas" && n.sizePt == 14.f);  // 空 family 保持
    CHECK(resolveFontChange(cur, "Cascadia Mono", 12.f, &n) && n.family == L"Cascadia Mono");
    CHECK(resolveFontChange(cur, "\xE7\xAD\x89\xE7\xBA\xBF", 12.f, &n) && n.family == L"\u7b49\u7ebf");  // UTF-8 → wide
    CHECK(resolveFontChange(cur, "", 2.f, &n) && n.sizePt == 6.f);
    CHECK(resolveFontChange(cur, "", 200.f, &n) && n.sizePt == 72.f);
    CHECK(!resolveFontChange(cur, "", std::numeric_limits<float>::quiet_NaN(), &n) && n.sizePt == 12.f);
    // 12 pt 估算与改动前固定值相同：1x 格 7×13 px
    auto m12 = itren::GridMetrics::from_font(itren::estimate_font_metrics(12.f), 1.f);
    CHECK(m12.cell_w_px() == 7 && m12.cell_h_px() == 13);
    auto m24 = itren::GridMetrics::from_font(itren::estimate_font_metrics(24.f), 1.f);
    CHECK(m24.cell_w_px() == 14 && m24.cell_h_px() == 26);
    uint16_t c1 = 0, r1 = 0, c2 = 0, r2 = 0;
    m12.grid_size(800, 600, c1, r1); m24.grid_size(800, 600, c2, r2);
    CHECK(c2 < c1 && r2 < r1 && c2 >= 20 && r2 >= 5);
    auto m15 = itren::GridMetrics::from_font(itren::estimate_font_metrics(12.f), 1.5f);
    CHECK(m15.cell_w_px() == 11 && m15.cell_h_px() == 20);
}

// B3：配置分词字符经 applyWordChars 下发核心
static void wordchars() {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 20, 3, 100, 1u << 20};
    itcore_core *c = nullptr; CHECK(itcore_create(&o, &c) == 0 && c);
    const char *t = "ab cd/e;f";
    itcore_feed(c, reinterpret_cast<const uint8_t *>(t), std::strlen(t));
    while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e);
    auto word = [&](uint16_t col) {
        itcore_selection_start_unit(c, 0, col, 0, 1);
        itcore_buffer *b = nullptr; std::string r;
        if (itcore_selection_text(c, &b) == 0 && b) { r.assign(reinterpret_cast<char *>(b->data), b->length); itcore_buffer_release(b); }
        return r;
    };
    CHECK(applyWordChars(c, std::nullopt) == 0 && word(3) == "cd/e");
    CHECK(applyWordChars(c, std::string(";")) == 0 && word(3) == "cd" && word(7) == "e;f");
    CHECK(applyWordChars(c, std::string("x")) == -1 && word(3) == "cd");   // 非法不改
    CHECK(applyWordChars(c, std::nullopt) == 0 && word(3) == "cd/e");
    CHECK(applyWordChars(nullptr, std::nullopt) == -1);
    // B4：OSC 52 配置下发（默认关，读写分开）
    auto osc = [&](const char *seq) {
        itcore_feed(c, reinterpret_cast<const uint8_t *>(seq), std::strlen(seq));
        int k = 0;
        while (itcore_event *e = itcore_next_event(c)) { if (itcore_event_type(e) != ITCORE_EV_RENDER_INVALIDATED) k = itcore_event_type(e); itcore_event_release(e); }
        return k;
    };
    CHECK(applyOsc52(c, Osc52Config{}) == 0);
    CHECK(osc("\x1b]52;c;aGk=\x07") == ITCORE_EV_CLIPBOARD_REQUEST);
    CHECK(applyOsc52(c, Osc52Config{true, false, 0}) == 0);
    CHECK(osc("\x1b]52;c;aGk=\x07") == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(osc("\x1b]52;c;?\x07") == ITCORE_EV_CLIPBOARD_REQUEST);
    CHECK(applyOsc52(c, Osc52Config{true, true, 1}) == 0);
    CHECK(osc("\x1b]52;c;?\x07") == ITCORE_EV_CLIPBOARD_READ);
    CHECK(osc("\x1b]52;c;aGk=\x07") == ITCORE_EV_CLIPBOARD_REQUEST);   // 2 字节 > 上限 1
    CHECK(applyOsc52(nullptr, Osc52Config{}) == -1);
    itcore_destroy(c);
}

// PUMP-01/02/PERF-02：pump_core 预算；续跑一律 Timer（含 Fed+missed）；收尾用 pump_core_final 按片 + yieldUi
static void pump_budget() {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 100, 1u << 20};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    struct Chunk { std::string bytes; };
    std::deque<Chunk> q;
    int pops = 0;
    auto pop = [&]() -> std::optional<Chunk> { if (q.empty()) return std::nullopt; ++pops; Chunk k = std::move(q.front()); q.pop_front(); return k; };
    using it2app::PumpResult;
    // 按字节：16 KiB 块 × 10，64 KiB 预算 → 4 块一停
    for (int i = 0; i < 10; ++i) q.push_back({std::string(16 * 1024, 'x')});
    it2app::PumpBudget bytes; bytes.maxBytes = 64 * 1024;
    CHECK(it2app::pump_core(c, pop, [] {}, bytes) == PumpResult::FedMore && pops == 4 && q.size() == 6);
    CHECK(it2app::nextAfterPump(PumpResult::FedMore) == it2app::PumpNext::Timer);   // 续跑走定时器，不投递
    CHECK(it2app::pump_core(c, pop, [] {}, bytes) == PumpResult::FedMore && q.size() == 2);
    CHECK(it2app::pump_core(c, pop, [] {}, bytes) == PumpResult::Fed && q.empty());   // 取空 → 不续跑
    CHECK(it2app::nextAfterPump(PumpResult::Fed) == it2app::PumpNext::None);
    CHECK(it2app::nextAfterPump(PumpResult::Idle) == it2app::PumpNext::None);
    CHECK(it2app::pump_core(c, pop, [] {}, bytes) == PumpResult::Idle);
    // 单块超预算：至少喂一块（保证前进），然后停
    q.push_back({std::string(100 * 1024, 'y')}); q.push_back({"z"});
    pops = 0;
    CHECK(it2app::pump_core(c, pop, [] {}, bytes) == PumpResult::FedMore && pops == 1 && q.size() == 1);
    q.clear();
    // 按时间：假时钟每次 pop 后前进 3ms，8ms 预算 → 3 块一停（字节远未到）
    struct Clock { uint64_t t = 1000; } clk;
    it2app::PumpBudget timeB; timeB.maxBytes = size_t(1) << 30; timeB.maxMs = 8;
    timeB.clockCtx = &clk; timeB.nowMs = [](void *p) { return static_cast<Clock *>(p)->t; };
    auto slowPop = [&]() -> std::optional<Chunk> { auto k = pop(); if (k) clk.t += 3; return k; };
    for (int i = 0; i < 10; ++i) q.push_back({"ab"});
    pops = 0;
    CHECK(it2app::pump_core(c, slowPop, [] {}, timeB) == PumpResult::FedMore && pops == 3 && q.size() == 7);
    // 默认预算下小输出照旧一次取空
    q.clear(); q.push_back({"a"}); q.push_back({"b"});
    CHECK(it2app::pump_core(c, pop, [] {}) == PumpResult::Fed && q.empty());
    // 带 carry：一块可在中间停下，剩余下次接着喂（单次不超过预算 + 一片）
    {
        it2app::PumpCarry carry;
        q.clear(); q.push_back({std::string(100 * 1024, 'y')}); q.push_back({"z"});
        pops = 0;
        CHECK(it2app::pump_core(c, pop, [] {}, bytes, &carry) == PumpResult::FedMore && pops == 1);
        CHECK(carry.bytes.size() - carry.off == 36 * 1024 && q.size() == 1);
        CHECK(it2app::pump_core(c, pop, [] {}, bytes, &carry) == PumpResult::Fed && carry.empty() && q.empty());
        // 按时间也在片之间检查：每次 feed 后时钟前进 5ms → 两片（32 KiB）一停
        struct Clock2 { uint64_t t = 0; } k2;
        it2app::PumpBudget tb; tb.maxBytes = size_t(1) << 30; tb.maxMs = 8;
        tb.clockCtx = &k2; tb.nowMs = [](void *p) { return static_cast<Clock2 *>(p)->t; };
        q.push_back({std::string(64 * 1024, 'w')});
        CHECK(it2app::pump_core(c, pop, [&] { k2.t += 5; }, tb, &carry) == PumpResult::FedMore);
        CHECK(carry.bytes.size() - carry.off == 32 * 1024);
        carry.clear(); q.clear();
    }
    // 事件队列已满（NEED_DRAIN 且 accepted==0）：排空后重试，这一片不丢
    {
        std::string bells(2000, '\a');
        auto fr = itcore_feed(c, reinterpret_cast<const uint8_t *>(bells.data()), bells.size());
        CHECK(fr.status == ITCORE_NEED_DRAIN && fr.accepted > 0 && fr.accepted < bells.size());
        auto drainAll = [&] { while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e); };
        q.push_back({"\x1b[2J\x1b[HNEEDDRAIN_OK"});
        CHECK(it2app::pump_core(c, pop, drainAll) == PumpResult::Fed);
        itcore_buffer *b = nullptr;
        CHECK(itcore_export_text(c, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("NEEDDRAIN_OK") != std::string::npos); itcore_buffer_release(b); }
    }
    // PUMP-04：afterFeed 不排空（核心一直 NEED_DRAIN 拒收）——非收尾整片留在 carry；收尾重试 N 次后跳过并记日志、继续喂
    {
        itcore_core *f = nullptr;
        CHECK(itcore_create(&o, &f) == 0 && f);
        std::string bells(4000, '\a');
        auto fr = itcore_feed(f, reinterpret_cast<const uint8_t *>(bells.data()), bells.size());
        CHECK(fr.status == ITCORE_NEED_DRAIN && fr.accepted < bells.size());
        struct LogSink { int n = 0; std::string last; } sink;
        auto logFn = [](void *ctx, const char *m) { auto *k = static_cast<LogSink *>(ctx); ++k->n; k->last = m; };
        int feeds = 0;
        auto noDrain = [&] { ++feeds; };
        std::deque<Chunk> fq;
        auto fpop = [&]() -> std::optional<Chunk> { if (fq.empty()) return std::nullopt; Chunk k = std::move(fq.front()); fq.pop_front(); return k; };
        it2app::PumpCarry carry;
        fq.push_back({std::string(10, '\a')});
        fq.push_back({"TAIL_OK"});
        it2app::PumpBudget nb; nb.log = logFn; nb.logCtx = &sink;
        CHECK(it2app::pump_core(f, fpop, noDrain, nb, &carry) == PumpResult::FedMore);   // 非收尾：不丢
        CHECK(carry.bytes == std::string(10, '\a') && carry.off == 0 && fq.size() == 1);
        CHECK(feeds == 2 && sink.n == 0);                                                 // 首喂 + 重试一次
        it2app::PumpBudget fb = it2app::PumpBudget::unlimited(); fb.log = logFn; fb.logCtx = &sink;
        CHECK(fb.final && fb.finalRetries == 8);
        feeds = 0;
        // 收尾：不返回 FedMore；这一片（首喂 + 8 次重试，afterFeed 都不排空）跳过并记日志，剩余积压照喂
        // （之后 afterFeed 恢复排空，"TAIL_OK" 那片能收下，证明跳片后继续）
        auto drainLater = [&] { if (++feeds > 9) while (itcore_event *e = itcore_next_event(f)) itcore_event_release(e); };
        CHECK(it2app::pump_core(f, fpop, drainLater, fb, &carry) == PumpResult::Fed);
        CHECK(carry.empty() && fq.empty() && sink.n == 1);
        CHECK(sink.last.find("refused 10 bytes after 8") != std::string::npos && sink.last.find("final") != std::string::npos);
        CHECK(feeds >= 1 + 8 + 1);
        itcore_buffer *b = nullptr;
        CHECK(itcore_export_text(f, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("TAIL_OK") != std::string::npos); itcore_buffer_release(b); }
        // 收尾时重试中途排空成功：不跳片、不记日志
        auto drainF = [&] { while (itcore_event *e = itcore_next_event(f)) itcore_event_release(e); };
        drainF();
        fr = itcore_feed(f, reinterpret_cast<const uint8_t *>(bells.data()), bells.size());
        CHECK(fr.status == ITCORE_NEED_DRAIN);
        fq.push_back({"\a\aRETRY_OK"});
        int calls = 0;
        auto lateDrain = [&] { if (++calls == 3) drainF(); };   // 第 3 次 afterFeed 才排空
        sink.n = 0;
        CHECK(it2app::pump_core(f, fpop, lateDrain, fb, &carry) == PumpResult::Fed);
        CHECK(sink.n == 0 && carry.empty());
        drainF();
        CHECK(itcore_export_text(f, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("RETRY_OK") != std::string::npos); itcore_buffer_release(b); }
        // 无日志回调：静默跳过（不崩）
        fr = itcore_feed(f, reinterpret_cast<const uint8_t *>(bells.data()), bells.size());
        fq.push_back({std::string(5, '\a')});
        CHECK(it2app::pump_core(f, fpop, [] {}, it2app::PumpBudget::unlimited(), &carry) == PumpResult::Fed && carry.empty());
        itcore_destroy(f);
    }
    // PUMP-04：finishSession 收尾顺序 pump(true) → KillTimer → sched.reset → finish_input → inputFinished
    {
        itcore_core *f = nullptr;
        CHECK(itcore_create(&o, &f) == 0 && f);
        it2app::PumpScheduler sc;
        CHECK(sc.readerShouldPost() && sc.postedShouldRun());
        sc.begin();
        CHECK(sc.end(true) == it2app::PumpScheduler::Next::Timer && sc.owned());   // 续跑定时器待发
        std::deque<Chunk> fq;
        for (int i = 0; i < 40; ++i) fq.push_back({std::string(64 * 1024 - 2, 'k') + "\r\n"});
        fq.push_back({"ORDER_END"});
        auto fpop = [&]() -> std::optional<Chunk> { if (fq.empty()) return std::nullopt; Chunk k = std::move(fq.front()); fq.pop_front(); return k; };
        auto drainF = [&] { while (itcore_event *e = itcore_next_event(f)) itcore_event_release(e); };
        bool inputFinished = false;
        std::vector<std::string> steps;
        it2app::PumpCarry carry;
        int finalYields = 0;
        it2app::finish_session_input(f, inputFinished, sc,
            [&] {
                CHECK(!inputFinished && sc.owned());
                // PERF-02：收尾按片 + yield（与 Win32 pump(true) 一致），不是一次 unlimited
                it2app::PumpBudget slice = it2app::PumpBudget::finalSlice();
                slice.yieldUi = [](void *ctx) -> bool { ++*static_cast<int *>(ctx); return false; };
                slice.yieldCtx = &finalYields;
                CHECK(it2app::pump_core_final(f, fpop, drainF, &carry, slice) == PumpResult::Fed);
                CHECK(finalYields >= 1);           // 多片积压 → 片间至少 yield 一次
                steps.push_back("pump");
            },
            [&] {
                CHECK(!inputFinished && sc.owned());   // 撤定时器在 reset 之前
                const char k = 'K';                    // finish_input 尚未调用：仍可喂
                CHECK(itcore_feed(f, reinterpret_cast<const uint8_t *>(&k), 1).accepted == 1);
                steps.push_back("kill");
            });
        CHECK((steps == std::vector<std::string>{"pump", "kill"}));
        CHECK(inputFinished && !sc.owned() && fq.empty() && carry.empty());
        CHECK(sc.readerShouldPost());                  // reset 之后读线程恢复投递资格
        itcore_buffer *b = nullptr;
        CHECK(itcore_export_text(f, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("ORDER_ENDK") != std::string::npos); itcore_buffer_release(b); }
        int pumps = 0;
        it2app::finish_session_input(f, inputFinished, sc, [&] { ++pumps; }, [] {});   // 二次调用：不重复 finish_input
        CHECK(pumps == 1 && inputFinished);
        itcore_destroy(f);
    }
    // PUMP-04：?2026 同步段被预算（FedMore）切开时不出半帧；段结束那次 pump 后立即放行
    {
        itcore_core *f = nullptr;
        CHECK(itcore_create(&o, &f) == 0 && f);
        auto drainF = [&] { while (itcore_event *e = itcore_next_event(f)) itcore_event_release(e); };
        std::string frame = "\x1b[?2026h\x1b[2J\x1b[H";
        for (int i = 0; i < 2000; ++i) frame += "row" + std::to_string(i) + std::string(60, '.') + "\r\n";
        frame += "SYNC_DONE\x1b[?2026l";
        CHECK(frame.size() > 2 * 64 * 1024);
        std::deque<Chunk> fq;
        fq.push_back({frame});
        auto fpop = [&]() -> std::optional<Chunk> { if (fq.empty()) return std::nullopt; Chunk k = std::move(fq.front()); fq.pop_front(); return k; };
        it2app::SyncGate gate(ITREN_SYNC_TIMEOUT_MS);
        CHECK(it2app::frame_due(gate, f, 0, false));    // 同步前：正常出帧
        it2app::PumpCarry carry;
        it2app::PumpBudget bb; bb.maxBytes = 64 * 1024;
        uint64_t now = 1;
        int partial = 0;
        PumpResult r;
        while ((r = it2app::pump_core(f, fpop, drainF, bb, &carry)) == PumpResult::FedMore) {
            ++partial;
            CHECK(itcore_mode_query(f, 2026) == ITCORE_MODE_SET);
            CHECK(!it2app::frame_due(gate, f, now, false));   // 段内（未超时）：不取快照，不出半帧
            now += 1;
        }
        CHECK(r == PumpResult::Fed && partial >= 2 && now < ITREN_SYNC_TIMEOUT_MS);
        CHECK(itcore_mode_query(f, 2026) != ITCORE_MODE_SET);
        CHECK(it2app::frame_due(gate, f, now, false));        // 段结束：立即出完整一帧
        itcore_buffer *b = nullptr;
        CHECK(itcore_export_text(f, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("SYNC_DONE") != std::string::npos); itcore_buffer_release(b); }
        itcore_destroy(f);
    }
    // PUMP-EXIT：积压远超一份预算时收尾（不限预算）一次喂完，结束标记可见，finish 之后不再 feed
    {
        itcore_core *f = nullptr;
        CHECK(itcore_create(&o, &f) == 0 && f);
        std::deque<Chunk> big;
        for (int i = 0; i < 80; ++i) big.push_back({std::string(64 * 1024 - 2, 'a' + i % 26) + "\r\n"});  // ~5 MiB
        big.push_back({"__END_MARK__"});
        auto bpop = [&]() -> std::optional<Chunk> { if (big.empty()) return std::nullopt; Chunk k = std::move(big.front()); big.pop_front(); return k; };
        auto drainF = [&] { while (itcore_event *e = itcore_next_event(f)) itcore_event_release(e); };
        it2app::PumpCarry carry;
        CHECK(it2app::pump_core(f, bpop, drainF, it2app::PumpBudget{}, &carry) == PumpResult::FedMore);  // 正常一份预算：没喂完
        CHECK(!big.empty());
        CHECK(it2app::pump_core(f, bpop, drainF, it2app::PumpBudget::unlimited(), &carry) == PumpResult::Fed);
        CHECK(big.empty() && carry.empty());
        itcore_finish_input(f);
        itcore_buffer *b = nullptr;
        CHECK(itcore_export_text(f, 1u << 20, &b) == 0 && b);
        if (b) { CHECK(std::string(reinterpret_cast<char *>(b->data), b->length).find("__END_MARK__") != std::string::npos); itcore_buffer_release(b); }
        itcore_destroy(f);
    }
    // PERF-02 调度：读线程合并投递；UI 线程 pump / 续跑定时器待发期间不投递；
    // Fed+missed / FedMore 一律 Timer（禁止 Post 续跑）；SetTimer 失败 → None（靠 kTickTimer）
    {
        using Next = it2app::PumpScheduler::Next;
        it2app::PumpScheduler sc;
        int posts = 0;
        for (int i = 0; i < 100; ++i) posts += sc.readerShouldPost();
        CHECK(posts == 1);
        CHECK(sc.postedShouldRun());               // 投递的 pump 开始执行
        sc.begin();                                // pump 期间
        posts = 0;
        for (int i = 0; i < 100; ++i) posts += sc.readerShouldPost();
        CHECK(posts == 0);                         // pump 期间读线程不投递
        CHECK(sc.end(true) == Next::Timer && sc.owned());   // 预算用尽 → 继续持有到 WM_TIMER
        for (int i = 0; i < 100; ++i) posts += sc.readerShouldPost();
        CHECK(posts == 0);                         // 定时器待发：不投递
        sc.begin();                                // WM_TIMER → pump，持有不间断
        for (int i = 0; i < 100; ++i) posts += sc.readerShouldPost();
        CHECK(posts == 0);
        // 取空但期间来过数据（missed）：也走 Timer，禁止 Post（刷屏 Fed+missed 的 Post 会饿死关闭点击）
        CHECK(sc.end(false) == Next::Timer && sc.owned());
        posts = 0;
        for (int i = 0; i < 100; ++i) posts += sc.readerShouldPost();
        CHECK(posts == 0);                         // 仍持有：读线程不投递
        sc.begin();                                // 续跑定时器触发
        CHECK(sc.end(false) == Next::None);        // 期间没新数据：释放
        CHECK(!sc.owned());
        CHECK(sc.readerShouldPost());              // 之后的新数据照常冷启动投递
        // SetTimer 失败：释放持有，不 Post（靠周期性 tick）
        sc.begin();
        CHECK(sc.end(true) == Next::Timer);
        CHECK(!sc.postedShouldRun());              // owned 时投递的任务跳过
        CHECK(sc.timerFailed() == Next::None);
        CHECK(!sc.owned());
        CHECK(sc.readerShouldPost());              // 释放后读线程可冷启动
        // release() 同样不 Post
        sc.begin(); CHECK(sc.end(true) == Next::Timer);
        CHECK(sc.release() == Next::None && !sc.owned());
        sc.reset();
        CHECK(sc.readerShouldPost());
    }
    // PERF-02：yieldUi 返回 true → 提前 FedMore（片间让出）；收尾 pump_core_final 片间调用 yield
    {
        itcore_core *f = nullptr;
        CHECK(itcore_create(&o, &f) == 0 && f);
        struct Chunk { std::string bytes; };
        std::deque<Chunk> fq;
        for (int i = 0; i < 20; ++i) fq.push_back({std::string(16 * 1024, 'y')});
        auto fpop = [&]() -> std::optional<Chunk> {
            if (fq.empty()) return std::nullopt;
            Chunk k = std::move(fq.front()); fq.pop_front(); return k;
        };
        int yields = 0;
        auto yieldAlways = [](void *ctx) -> bool { ++*static_cast<int *>(ctx); return true; };
        it2app::PumpBudget bb; bb.maxBytes = 256 * 1024;  // 足够多，靠 yield 提前停
        bb.yieldUi = yieldAlways; bb.yieldCtx = &yields;
        it2app::PumpCarry carry;
        CHECK(it2app::pump_core(f, fpop, [] {}, bb, &carry) == PumpResult::FedMore);
        CHECK(yields == 1);                        // 喂完第一片后 yield → FedMore
        CHECK(!fq.empty());                        // 还有积压
        // pump_core_final：按片喂完，片间每次 FedMore 都再调一次 yield
        yields = 0;
        auto yieldCount = [](void *ctx) -> bool { ++*static_cast<int *>(ctx); return false; };  // 不提前停
        it2app::PumpBudget slice = it2app::PumpBudget::finalSlice();
        slice.maxBytes = 32 * 1024;
        slice.yieldUi = yieldCount; slice.yieldCtx = &yields;
        CHECK(it2app::pump_core_final(f, fpop, [] {}, &carry, slice) == PumpResult::Fed);
        CHECK(fq.empty() && carry.empty());
        CHECK(yields >= 2);                        // 至少两片 → 片间 yield ≥1，加上片内检查
        itcore_destroy(f);
    }
    // PERF-03：pump 重入守卫——已在 pump 中时第二次 tryEnter 失败；析构后可再进；
    // 模拟「pump 中再调 pump」（MessageBox 嵌套）第二次被忽略，只记一次 feed
    {
        bool pumping = false;
        {
            it2app::PumpReentrancyGuard g1(pumping);
            CHECK(g1.entered() && pumping);
            it2app::PumpReentrancyGuard g2(pumping);
            CHECK(!g2.entered() && pumping);       // 第二次被忽略
        }
        CHECK(!pumping);
        {
            it2app::PumpReentrancyGuard g3(pumping);
            CHECK(g3.entered() && pumping);
        }
        CHECK(!pumping);
        // 嵌套：外层已持有时内层直接跳过，只记一次 feed
        int feeds = 0;
        {
            it2app::PumpReentrancyGuard outer(pumping);
            CHECK(outer);
            ++feeds;
            {
                it2app::PumpReentrancyGuard inner(pumping);
                if (inner) ++feeds;                // 不应进入
                CHECK(!inner.entered());
            }
        }
        CHECK(feeds == 1 && !pumping);
    }
    // M17：会话启动网格门闩——仍在 20×5 下限时推迟；严格大于后再启
    {
        CHECK(it2app::gridAtSessionFloor({it2app::kMinCols, it2app::kMinRows}));
        CHECK(!it2app::gridReadyForSessionStart({it2app::kMinCols, it2app::kMinRows}));
        CHECK(!it2app::gridAtSessionFloor({it2app::kMinCols, uint16_t(it2app::kMinRows + 1)}));
        CHECK(it2app::gridReadyForSessionStart({it2app::kMinCols, uint16_t(it2app::kMinRows + 1)}));
        CHECK(it2app::gridReadyForSessionStart({uint16_t(it2app::kMinCols + 1), it2app::kMinRows}));
        CHECK(it2app::gridReadyForSessionStart({80, 24}));
        auto floor = it2app::clampGrid(1, 1);
        CHECK(floor.cols == it2app::kMinCols && floor.rows == it2app::kMinRows);
        CHECK(it2app::gridAtSessionFloor(floor));
    }
    itcore_destroy(c);
}

// BUG-03：拖动中失去捕获（模态对话框）后，后续鼠标移动不得改变选区；已拖出的选区保留
static void capture_loss() {
    using C = DragSelect::Cell;
    using A = DragSelect::Act;
    DragSelect d;
    d.down(C{0, 2});
    CHECK(d.move(C{5, 4}) == A::Start);
    CHECK(d.move(C{6, 9}) == A::Extend);
    d.captureLost();                       // WM_CAPTURECHANGED
    CHECK(!d.pressed() && !d.dragging());  // 自动滚动 tick 以 pressed() 为门：停止
    CHECK(d.move(C{20, 12}) == A::None);   // QA：之后的 WM_MOUSEMOVE 不扩展选区
    CHECK(d.move(C{0, 0}) == A::None);
    CHECK(d.up(C{20, 12}) == A::None);     // 迟到的松开也不当成单击（不会清掉保留的选区）
    // WM_CANCELMODE 同样处理；之后新的按下照常工作
    d.down(C{1, 1}); d.captureLost();
    CHECK(d.move(C{3, 3}) == A::None);
    d.down(C{1, 1}); CHECK(d.move(C{3, 3}) == A::Start);

    // 激活点击：窗口未激活时的那一下点击不开始选区，配对的移动/松开也吞掉
    ActivationClickGate g;
    g.onMouseActivate(true);
    CHECK(!g.allowDown()); CHECK(!g.allowMove()); CHECK(!g.allowUp());
    CHECK(g.allowDown() && g.allowMove() && g.allowUp());  // 下一次点击正常
    // 被吞的按下在窗格外松开（没收到 UP）：下一次正常按下/松开不受影响
    g.onMouseActivate(true);
    CHECK(!g.allowDown());                                  // 激活点击，UP 落在窗格外丢失
    g.onMouseActivate(false);
    CHECK(g.allowDown() && g.allowMove() && g.allowUp());
    g.onMouseActivate(false);                               // 已激活窗口内的点击照常
    CHECK(g.allowDown() && g.allowUp());
}

// BUG-02：子窗口 HWND 宿主必须选 blt 模型 Discard + 1 缓冲；默认仍为 Flip + 2
static void swap_config() {
    CHECK(itren::kChildHwndSwapConfig.effect == itren::SwapEffect::Discard);
    CHECK(itren::kChildHwndSwapConfig.buffer_count == 1);
    struct H : itren::ISwapChainHost {
#ifdef _WIN32
        IDXGISwapChain1 *create_swap_chain(IDXGIFactory2 *, ID3D11Device *, const DXGI_SWAP_CHAIN_DESC1 &) override { return nullptr; }
#endif
        itren::SizePx client_size_px() const override { return {1, 1}; }
        float dpi_scale() const override { return 1.f; }
    } h;
    CHECK(h.swap_config().effect == itren::SwapEffect::FlipSequential && h.swap_config().buffer_count == 2);
}

// --command：按 shell 拼参数；cmd 用 /s /k "..." 原样保留命令里的引号与 &、|
static void local_command() {
    using it2app::planLocalShellCommand;
    auto e = planLocalShellCommand(L"C:\\Windows\\System32\\cmd.exe", L"");
    CHECK(e && e->args.empty() && e->rawTail.empty());
    auto c = planLocalShellCommand(L"C:\\Windows\\System32\\CMD.EXE", L"dir /s");
    CHECK(c && c->args == (std::vector<std::wstring>{L"/s", L"/k"}) && c->rawTail == L"\"dir /s\"");
    auto q = planLocalShellCommand(L"cmd.exe", L"\"C:\\a b\\t.cmd\" x & echo \"y\"");
    CHECK(q && q->rawTail == L"\"\"C:\\a b\\t.cmd\" x & echo \"y\"\"");
    auto p = planLocalShellCommand(L"C:/Program Files/PowerShell/7/pwsh.exe", L"Get-ChildItem -Recurse");
    CHECK(p && p->args == (std::vector<std::wstring>{L"-NoExit", L"-Command", L"Get-ChildItem -Recurse"}) && p->rawTail.empty());
    CHECK(planLocalShellCommand(L"powershell.exe", L"x")->args.size() == 3);
    auto o = planLocalShellCommand(L"D:\\msys64\\usr\\bin\\bash.exe", L"-lc 'ls -R /'");
    CHECK(o && o->args.empty() && o->rawTail == L"-lc 'ls -R /'");
    CHECK(!planLocalShellCommand(L"cmd.exe", L"dir\r\nformat c:"));
    CHECK(!planLocalShellCommand(L"cmd.exe", L"a\nb"));
    CHECK(!planLocalShellCommand(L"cmd.exe", std::wstring(L"a\0b", 3)));
}


// PANE-10：搜索驱动（findInScrollback / clearSearch 的平台无关部分）
static itcore_core *search_core(uint16_t cols, uint16_t rows, uint32_t sb) {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(o), cols, rows, sb, 64ull << 20};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
static void search_feed(itcore_core *c, const std::string &s) {
    itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    while (itcore_event *e = itcore_next_event(c)) itcore_event_release(e);
}
static void search_driver() {
    {   // 基本：分批计数（total=-1 → 终值）、next / prev、视口坐标、ranges
        itcore_core *c = search_core(40, 10, 100000);
        std::string s;
        for (int i = 0; i < 20000; ++i) s += "line " + std::to_string(i) + (i % 100 == 0 ? " needle\r\n" : "\r\n");
        search_feed(c, s);
        PaneSearch ps;
        PaneSearch::Options o;
        auto r = ps.find(c, "needle", o, true);  // 向上（prev）：最近的一条
        CHECK(ps.active());
        CHECK(r.found || r.pending);
        int guard = 0;
        while (ps.busy() && guard++ < 100000) if (auto u = ps.tick(64 * 1024, 0)) r = *u;
        CHECK(!ps.busy());
        CHECK(r.found && r.total == 200 && r.index == 199);
        CHECK(r.hasView && r.viewRow < 0 && r.viewCol == 11);  // "line 19900 needle"
        auto v = ps.ranges(r.viewRow, 1);
        CHECK(v.size() == 1 && v[0].start_col == 11 && v[0].end_col == 17 && v[0].is_current == 1);
        r = ps.find(c, "needle", o, true);
        CHECK(r.found && r.index == 198 && r.viewRow < 0);
        r = ps.find(c, "needle", o, false);
        CHECK(r.found && r.index == 199);
        CHECK(!ps.tick(1024, 0));  // 无变化不通知
        // 大小写：SENSITIVE 搜 Needle → 0；换选项即新一轮
        o.caseMode = ITCORE_SEARCH_CASE_SENSITIVE;
        r = ps.find(c, "Needle", o, true);
        while (ps.busy()) if (auto u = ps.tick(1u << 20, 0)) r = *u;
        CHECK(!r.found && r.total == 0);
        // regex：v0.1 不支持 → found=false、不退回子串；空 needle 同样
        o.regex = true;
        r = ps.find(c, "needle", o, true);
        CHECK(!r.found && !ps.active());
        o.regex = false;
        CHECK(!ps.find(c, "", o, true).found && !ps.active());
        ps.end();
        itcore_destroy(c);
    }
    {   // 失效：resize → restarted（只报一次），句柄已释放；UI 用同一 needle 重搜
        itcore_core *c = search_core(20, 5, 100);
        search_feed(c, "foo\r\nbar foo\r\n");
        PaneSearch ps;
        auto r = ps.find(c, "foo", {}, false);
        CHECK(r.found && r.total == 2 && r.index == 0);
        itcore_resize(c, 30, 6, 2);
        auto u = ps.tick(1024, 0);
        CHECK(u && u->restarted && !ps.active());
        CHECK(!ps.tick(1024, 0));
        r = ps.find(c, "foo", {}, false);
        CHECK(r.found && r.total == 2 && !r.restarted);
        // 新输出：PF_NEW_CONTENT → hasNewOutput，total 不变
        search_feed(c, "foo again\r\n");
        u = ps.tick(1024, 0);
        CHECK(u && u->hasNewOutput && u->total == 2);
        // 当前结果被改写：found 变 false
        search_feed(c, "\x1b[1;1Hxxx");
        u = ps.tick(1024, 0);
        CHECK(u && !u->found && u->total == 1);
        ps.end();
        CHECK(!ps.active() && !ps.busy());
        itcore_destroy(c);
    }
    {   // PENDING：预算极小、方向反着走，tick 落定后 moved=true
        itcore_core *c = search_core(40, 10, 100000);
        std::string s;
        for (int i = 0; i < 5000; ++i) s += i == 10 ? "target\r\n" : "filler filler filler\r\n";
        search_feed(c, s);
        PaneSearch ps;
        auto r = ps.find(c, "target", {}, true);   // 从底部向上，target 在很早的历史里
        bool moved = r.found;
        int guard = 0;
        while (ps.busy() && guard++ < 100000) if (auto u = ps.tick(256, 0)) { r = *u; moved = moved || u->moved; }
        CHECK(r.found && r.total == 1 && moved && r.hasView);
        ps.end();
        itcore_destroy(c);
    }
}

int main(int argc, char **argv) {
    std::string w = argc > 1 ? argv[1] : "all";
    if (w == "churn") churn();
    else if (w == "viewport") viewport();
    else if (w == "keys") keys();
    else if (w == "sync") syncfocus();
    else if (w == "font") font();
    else if (w == "wordchars") wordchars();
    else if (w == "pump") pump_budget();
    else if (w == "command") local_command();
    else if (w == "search") search_driver();
    else { limits(); grid(); queue(); closer(); drag(); capture_loss(); swap_config(); if (w == "all") { churn(); viewport(); keys(); syncfocus(); font(); wordchars(); pump_budget(); local_command(); search_driver(); } }
    if (fails) return 1;
    std::printf("PANE %s ok\n", w.c_str());
    return 0;
}
