#include "pane_logic.h"
#include <thread>
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
    bool kick;
    { std::lock_guard l(mu_); wake_ = std::move(w); kick = !q_.empty() && wake_; }
    if (kick) wake_();
}
bool UiDispatchQueue::post(Task t) {
    std::function<void()> w;
    {
        std::lock_guard l(mu_);
        if (closed_) return false;
        bool wasEmpty = q_.empty();
        q_.push_back(std::move(t));
        if (wasEmpty) w = wake_;
    }
    if (w) w();
    return true;
}
std::size_t UiDispatchQueue::drain() {
    std::deque<Task> batch;
    { std::lock_guard l(mu_); batch.swap(q_); }
    for (auto &t : batch) if (t) t();
    return batch.size();
}
void UiDispatchQueue::close() { std::lock_guard l(mu_); closed_ = true; q_.clear(); }
std::size_t UiDispatchQueue::size() const { std::lock_guard l(mu_); return q_.size(); }

bool SessionCloser::start(std::shared_ptr<UiDispatchQueue> q, Job job, Done done, std::function<void()> after) {
    if (started_) return false;
    started_ = true;
    std::thread([q = std::move(q), job = std::move(job), done = std::move(done), after = std::move(after)]() mutable {
        CloseOutcome r = job ? job() : CloseOutcome{};
        job = nullptr;  // 先释放 job 持有的资源（析构也在本线程）
        if (q && done) q->post([done = std::move(done), r = std::move(r)] { done(r); });
        q.reset();
        if (after) after();
        after = nullptr;
    }).detach();
    return true;
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

}  // namespace it2app
