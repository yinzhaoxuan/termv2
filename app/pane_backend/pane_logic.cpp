#include "pane_logic.h"
#include <algorithm>

namespace it2app {

std::string sanitizePasteBody(std::string_view body) {
    std::string s;
    s.reserve(body.size());
    for (char c : body) if (c != '\0') s.push_back(c);
    static const std::string_view marks[] = {"\x1b[200~", "\x1b[201~", "\xc2\x9b" "200~", "\xc2\x9b" "201~"};
    for (bool changed = true; changed;) {
        changed = false;
        for (auto m : marks) {
            for (std::size_t p; (p = s.find(m)) != std::string::npos;) { s.erase(p, m.size()); changed = true; }
        }
    }
    return s;
}

std::string buildPaste(std::string_view body, bool bracketed) {
    std::string clean = sanitizePasteBody(body);
    if (!bracketed) return clean;
    return "\x1b[200~" + clean + "\x1b[201~";
}

bool bracketedFromProbe(std::string_view e) { return e.rfind("\x1b[200~", 0) == 0; }

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

}  // namespace it2app
