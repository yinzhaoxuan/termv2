// T08 PaneBackend 平台无关逻辑单测（PANE-01）
#include "pane_logic.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
using namespace it2app;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static void paste() {
    CHECK(buildPaste("ls", false) == "ls");
    CHECK(buildPaste("ls", true) == "\x1b[200~ls\x1b[201~");
    CHECK(buildPaste("a\x1b[201~rm -rf ~\n", true) == "\x1b[200~arm -rf ~\n\x1b[201~");
    // 拼接绕过：删除内层后外层又组成结束标记
    CHECK(sanitizePasteBody("x\x1b[20\x1b[201~1~y") == "xy");
    CHECK(sanitizePasteBody("\xc2\x9b" "201~z") == "z");
    CHECK(sanitizePasteBody(std::string("a\0b", 3)) == "ab");
    CHECK(sanitizePasteBody("\x1b[A 中文") == "\x1b[A 中文");  // 其他 ESC 保留
    CHECK(buildPaste("\x1b[201~", false).empty());          // 非括号模式也去标记
    CHECK(bracketedFromProbe("\x1b[200~\x1b[201~"));
    CHECK(!bracketedFromProbe(""));
}
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
int main() {
    paste(); limits(); grid(); queue();
    if (fails) return 1;
    std::puts("PANE-01 ok");
    return 0;
}
