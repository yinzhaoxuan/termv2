#include "pane_logic.h"
#include <thread>
#include <condition_variable>
#include <algorithm>

namespace it2app {

bool RateLimiter::allow(uint64_t now) {
    if (last_ && now - *last_ < interval_) return false;
    last_ = now;
    return true;
}

std::optional<std::string> TitleThrottle::offer(std::string title, uint64_t now) {
    if (rl_.allow(now)) { pending_.reset(); return title; }
    pending_ = std::move(title);
    return std::nullopt;
}
std::optional<std::string> TitleThrottle::flush(uint64_t now) {
    if (!pending_ || !rl_.allow(now)) return std::nullopt;
    auto t = std::move(pending_);
    pending_.reset();
    return t;
}

GridDims clampGrid(int cols, int rows) {
    return {uint16_t(std::clamp(cols, int(kMinCols), 65535)), uint16_t(std::clamp(rows, int(kMinRows), 65535))};
}

std::optional<uint64_t> ResizeTracker::update(int cols, int rows) {
    GridDims d = clampGrid(cols, rows);
    if (d.cols == dims_.cols && d.rows == dims_.rows) return std::nullopt;
    dims_ = d;
    return ++gen_;
}

void UiDispatchQueue::setWake(std::function<void()> w) {
    std::lock_guard l(mu_);
    wake_ = std::move(w);
    if (!closed_ && !q_.empty() && wake_) wake_();
}
bool UiDispatchQueue::post(Task t) {
    return postOwner([t = std::move(t)](void *) { if (t) t(); });
}
bool UiDispatchQueue::postOwner(OwnerTask t) {
    std::lock_guard l(mu_);
    if (closed_) return false;
    bool wasEmpty = q_.empty();
    q_.push_back(std::move(t));
    if (wasEmpty && wake_) wake_();
    return true;
}
std::size_t UiDispatchQueue::drain(void *owner) {
    std::deque<OwnerTask> batch;
    { std::lock_guard l(mu_); batch.swap(q_); }
    for (auto &t : batch) if (t) t(owner);
    return batch.size();
}
void UiDispatchQueue::close() {
    std::deque<OwnerTask> dead;  // 在锁外析构任务（任务捕获物的析构可能很重）
    { std::lock_guard l(mu_); closed_ = true; dead.swap(q_); wake_ = nullptr; }
}
bool UiDispatchQueue::closed() const { std::lock_guard l(mu_); return closed_; }
std::size_t UiDispatchQueue::size() const { std::lock_guard l(mu_); return q_.size(); }

void SessionShared::setCloseResult(CloseOutcome r) { std::lock_guard l(mu_); result_ = std::move(r); }
std::optional<CloseOutcome> SessionShared::takeCloseResult() {
    std::lock_guard l(mu_);
    auto r = std::move(result_);
    result_.reset();
    return r;
}

namespace {
struct CloserRegistry {
    std::mutex mu;
    std::condition_variable cv;
    int live = 0;
};
CloserRegistry &registry() {
    static CloserRegistry *r = new CloserRegistry;  // 故意泄漏：detach 线程可能晚于静态析构
    return *r;
}
}  // namespace

bool SessionCloser::start(std::shared_ptr<SessionShared> st, Job job, After after) {
    if (started_ || !st) return false;
    started_ = true;
    auto &reg = registry();
    { std::lock_guard l(reg.mu); ++reg.live; }
    std::thread([st = std::move(st), job = std::move(job), after = std::move(after)]() mutable {
        CloseOutcome r = job ? job(*st) : CloseOutcome{};
        job = nullptr;  // 先释放 job 持有的资源（析构也在本线程）
        st->setCloseResult(std::move(r));
        st->queue->post([] {});  // 唤醒 UI；队列已 close 则丢弃
        if (after) after(*st);
        after = nullptr;
        st.reset();
        auto &rg = registry();
        std::lock_guard l(rg.mu);
        --rg.live;
        rg.cv.notify_all();
    }).detach();
    return true;
}

bool SessionCloser::shutdownAll(std::chrono::milliseconds timeout) {
    auto &reg = registry();
    std::unique_lock l(reg.mu);
    return reg.cv.wait_for(l, timeout, [&] { return reg.live == 0; });
}
int SessionCloser::liveThreads() {
    auto &reg = registry();
    std::lock_guard l(reg.mu);
    return reg.live;
}

DragSelect::Act DragSelect::move(Cell c) {
    if (!pressed_) return Act::None;
    if (dragging_) return Act::Extend;
    if (c == press_) return Act::None;
    dragging_ = true;
    return Act::Start;
}

DragSelect::Act DragSelect::up(Cell c) {
    if (!pressed_) return Act::None;
    bool was = dragging_ || !(c == press_);
    pressed_ = dragging_ = false;
    return was ? Act::Finish : Act::Click;
}

int ClickCounter::press(int col, int row, uint64_t nowMs, uint32_t intervalMs) {
    bool chain = count_ > 0 && col == col_ && row == row_ && nowMs - last_ <= intervalMs;
    count_ = chain ? (count_ % 3) + 1 : 1;
    col_ = col; row_ = row; last_ = nowMs;
    return count_;
}

bool Viewport::set(int off) {
    off = std::clamp(off, 0, std::max(0, history_));  // 调用方已在 int64 中算好再收窄
    if (off == offset_) return false;
    offset_ = off;
    return true;
}
// lines 可能是 INT_MIN / INT_MAX（UI 一次滚到顶/底）：在 int64 中相减并截断，不取反、不溢出
bool Viewport::scroll(int lines) {
    int64_t off = int64_t(offset_) - int64_t(lines);
    off = std::clamp<int64_t>(off, 0, std::max(0, history_));
    return set(int(off));
}
bool Viewport::updateHistory(int history) {
    history = std::max(0, history);
    int old = history_, oldOff = offset_;
    history_ = history;
    int64_t off = offset_ > 0 && history > old ? int64_t(offset_) + (int64_t(history) - old) : offset_;
    set(int(std::clamp<int64_t>(off, 0, history)));
    return history_ != old || offset_ != oldOff;
}

std::string applyNewlineMode(std::string_view in, bool lnm) {
    if (lnm && (in == "\r" || in == "\x1b\r")) return std::string(in) + "\n";
    return std::string(in);
}

}  // namespace it2app
