// T08 PaneBackend 的平台无关部分（Linux 单测）：括号粘贴、响铃/标题限流、20×5 最小网格与
// resize generation、跨线程 → UI 线程的事件派发队列。Win32 实现见 win32_pane_backend.cpp。
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace it2app {

// 粘贴正文清洗：去掉 NUL，以及所有括号粘贴起止标记（ESC[200~ / ESC[201~ 及其 8 位 CSI
// 形式 U+009B 的 UTF-8 编码 C2 9B）。反复删除直到不再出现，防止拼接出新的结束标记。
std::string sanitizePasteBody(std::string_view body);
// 完整粘贴字节：bracketed 时加 ESC[200~ … ESC[201~；否则只返回清洗后的正文。
std::string buildPaste(std::string_view body, bool bracketed);
// 从 itcore_encode_paste("") 的输出判断 DECSET 2004 是否打开（核心快照未暴露该模式）。
bool bracketedFromProbe(std::string_view encodedEmptyPaste);

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

}  // namespace it2app
