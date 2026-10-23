// T08 PaneBackend 平台无关逻辑单测（PANE-01）
#include "pane_logic.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <thread>
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
// 模拟一个会话：job 在关闭线程上睡 sleepMs（代表九步关闭 / 超时强杀）
struct FakeSession {
    std::shared_ptr<UiDispatchQueue> q = std::make_shared<UiDispatchQueue>();
    SessionCloser closer;
    std::atomic<int> *jobsDone;
    std::thread::id uiThread = std::this_thread::get_id();
    bool statusDelivered = false, statusOnUi = false, timedOut = false;
    std::shared_ptr<int> transport = std::make_shared<int>(7);  // 代表传输层对象
    explicit FakeSession(std::atomic<int> *d) : jobsDone(d) {}
    void requestClose(int sleepMs, bool timeout = false) {
        auto t = transport;  // job 持有传输层，析构在关闭线程
        transport.reset();
        closer.start(q, [t, sleepMs, timeout, d = jobsDone]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            t.reset();
            d->fetch_add(1);
            CloseOutcome r; r.ok = !timeout; r.timedOut = timeout;
            return r;
        }, [this](const CloseOutcome &r) {
            statusDelivered = true;
            statusOnUi = std::this_thread::get_id() == uiThread;
            timedOut = r.timedOut;
        });
    }
    ~FakeSession() { q->close(); }
};
static bool pumpUntil(std::vector<FakeSession *> ss, long budgetMs) {
    auto t0 = Clock::now();
    for (;;) {
        bool all = true;
        for (auto *s : ss) { s->q->drain(); all = all && s->statusDelivered; }
        if (all) return true;
        if (msSince(t0) > budgetMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static void closer() {
    std::atomic<int> done{0};
    {
        FakeSession s(&done);
        auto t0 = Clock::now();
        s.requestClose(150);
        CHECK(msSince(t0) < 50);               // 立即返回
        s.requestClose(150);                   // 重复请求无效
        CHECK(!s.statusDelivered);             // 不在调用线程同步回调
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!s.statusDelivered);             // 未 drain 前不回调（只经 UI 队列）
        CHECK(pumpUntil({&s}, 2000));
        CHECK(s.statusOnUi && !s.timedOut);
    }
    {   // 超时强杀路径
        FakeSession s(&done);
        s.requestClose(10, true);
        CHECK(pumpUntil({&s}, 2000) && s.timedOut);
    }
    {   // 8 个会话并行关闭：总耗时 ≈ 单个，而非 8 倍
        std::vector<std::unique_ptr<FakeSession>> v;
        std::vector<FakeSession *> raw;
        for (int i = 0; i < 8; ++i) { v.push_back(std::make_unique<FakeSession>(&done)); raw.push_back(v.back().get()); }
        auto t0 = Clock::now();
        for (auto *s : raw) s->requestClose(200);
        CHECK(msSince(t0) < 50);
        CHECK(pumpUntil(raw, 3000));
        long total = msSince(t0);
        CHECK(total < 800);
        for (auto *s : raw) CHECK(s->statusOnUi);
    }
    {   // 关闭中销毁：析构立即返回，done 不再被调用（队列已 close）
        int before = done.load();
        auto t0 = Clock::now();
        auto q = std::shared_ptr<UiDispatchQueue>();
        {
            FakeSession s(&done);
            q = s.q;
            s.requestClose(150);
        }
        CHECK(msSince(t0) < 50);
        auto t1 = Clock::now();
        while (done.load() == before && msSince(t1) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(done.load() == before + 1);      // 关闭仍在后台跑完
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(q->drain() == 0);                // 结果被丢弃
    }
    // 等所有 detach 线程收尾
    auto t2 = Clock::now();
    while (done.load() < 11 && msSince(t2) < 3000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(done.load() == 11);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
int main() {
    limits(); grid(); queue(); closer(); drag();
    if (fails) return 1;
    std::puts("PANE-01 ok");
    return 0;
}
