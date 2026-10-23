// T08 PaneBackend 的平台无关部分（Linux 单测）：括号粘贴、响铃/标题限流、20×5 最小网格与
// resize generation、跨线程 → UI 线程的事件派发队列。Win32 实现见 win32_pane_backend.cpp。
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace it2app {

// 粘贴：括号粘贴标记与正文清洗都在核心 itcore_encode_paste 里（由 DECSET 2004 决定），此处不再重复。
inline constexpr std::size_t kPasteLimitBytes = 32u * 1024u;  // 与核心 / InputScheduler 上限一致

// 最小间隔限流：allow() 在距上次放行 ≥ intervalMs 时返回 true。
class RateLimiter {
public:
    explicit RateLimiter(uint64_t intervalMs) : interval_(intervalMs) {}
    bool allow(uint64_t nowMs);
private:
    uint64_t interval_;
    std::optional<uint64_t> last_;
};

// 标题限流（≤10/s）：超频的标题暂存，flush 时补发最后一个，不丢最终标题。
class TitleThrottle {
public:
    explicit TitleThrottle(uint64_t intervalMs = 100) : rl_(intervalMs) {}
    std::optional<std::string> offer(std::string title, uint64_t nowMs);
    std::optional<std::string> flush(uint64_t nowMs);
    bool pending() const { return pending_.has_value(); }
private:
    RateLimiter rl_;
    std::optional<std::string> pending_;
};

struct GridDims { uint16_t cols = 80, rows = 24; };
inline constexpr uint16_t kMinCols = 20, kMinRows = 5;
GridDims clampGrid(int cols, int rows);

// 网格变化 → generation 递增；同尺寸不递增（只需 ResizeBuffers）。
class ResizeTracker {
public:
    explicit ResizeTracker(GridDims initial = {}) : dims_(clampGrid(initial.cols, initial.rows)) {}
    // 返回新 generation（网格变化时），否则 nullopt
    std::optional<uint64_t> update(int cols, int rows);
    GridDims dims() const { return dims_; }
    uint64_t generation() const { return gen_; }
private:
    GridDims dims_;
    uint64_t gen_ = 1;
};

// 任意线程 post，UI 线程 drain。队列由空变非空时调用 wake（可在任意线程，须廉价，如 PostMessage）。
// close() 后 post 被丢弃。drain 中回调再 post 的任务留到下一次 drain，避免饿死消息循环。
class UiDispatchQueue {
public:
    using Task = std::function<void()>;
    explicit UiDispatchQueue(std::function<void()> wake = {}) : wake_(std::move(wake)) {}
    void setWake(std::function<void()> wake);
    bool post(Task t);
    std::size_t drain();
    void close();
    std::size_t size() const;
private:
    mutable std::mutex mu_;
    std::deque<Task> q_;
    std::function<void()> wake_;
    bool closed_ = false;
};

// ---- 非阻塞关闭（T08 R1）----
// 每个会话自己的关闭线程：job 在该线程上跑完 T03A 九步关闭与超时强杀（可阻塞），
// 结果 post 到 UI 队列，done 在 UI 线程 drain 时执行。start() 立即返回；多个会话并行关闭。
// 队列被 close（UI 侧对象销毁）后结果丢弃，done 不会再被调用；线程 detach，不被 join。
struct CloseOutcome {
    bool ok = true;         // 正常结束（Exited）
    bool timedOut = false;  // 超出预算被强杀
    std::string detail;
};
class SessionCloser {
public:
    using Job = std::function<CloseOutcome()>;
    using Done = std::function<void(const CloseOutcome &)>;
    // 只有第一次调用生效；返回是否本次启动。after（可空）在 post 结果之后、仍在关闭线程上执行，
    // 用于超时上报后继续等待传输层真正结束并在该线程上析构。
    bool start(std::shared_ptr<UiDispatchQueue> q, Job job, Done done, std::function<void()> after = {});
    bool started() const { return started_; }
private:
    bool started_ = false;
};

// 本地拖选手势：按下不立即建选区；移到另一格才开始（起点 = 按下格）；未移动的单击 = Click
// （清除选区 / 链接激活）。坐标为格坐标。
class DragSelect {
public:
    enum class Act { None, Start, Extend, Finish, Click };
    struct Cell { int col = 0, row = 0; bool operator==(const Cell &o) const { return col == o.col && row == o.row; } };
    void down(Cell c) { pressed_ = true; dragging_ = false; press_ = c; }
    Act move(Cell c);
    Act up(Cell c);
    void cancel() { pressed_ = dragging_ = false; }
    bool pressed() const { return pressed_; }
    bool dragging() const { return dragging_; }
    Cell pressCell() const { return press_; }
private:
    bool pressed_ = false, dragging_ = false;
    Cell press_;
};

}  // namespace it2app
