#include "itcore.h"
#include "pane_logic.h"
#include <thread>
#include <condition_variable>
#include <algorithm>

namespace it2app {

int applyWordChars(itcore_core *core, const std::optional<std::string> &wc) {
    if (!core) return -1;
    return wc ? itcore_set_word_chars(core, wc->data(), wc->size()) : itcore_set_word_chars(core, nullptr, 0);
}

namespace {
std::wstring utf8ToWide(std::string_view s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD; size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 31u) << 6) | (s[i + 1] & 63u); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 15u) << 12) | ((s[i + 1] & 63u) << 6) | (s[i + 2] & 63u); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 7u) << 18) | ((s[i + 1] & 63u) << 12) | ((s[i + 2] & 63u) << 6) | (s[i + 3] & 63u); n = 4; }
        i += n;
        if (cp >= 0x10000 && sizeof(wchar_t) == 2) {
            cp -= 0x10000; w.push_back(wchar_t(0xD800 + (cp >> 10))); w.push_back(wchar_t(0xDC00 + (cp & 0x3FF)));
        } else w.push_back(wchar_t(cp));
    }
    return w;
}
}  // namespace

bool resolveFontChange(const FontSpec &cur, std::string_view familyUtf8, float sizePt, FontSpec *out) {
    FontSpec n = cur;
    if (!familyUtf8.empty()) n.family = utf8ToWide(familyUtf8);
    if (sizePt == sizePt) n.sizePt = sizePt < 6.f ? 6.f : sizePt > 72.f ? 72.f : sizePt;  // NaN 保持当前
    if (out) *out = n;
    return n.family != cur.family || n.sizePt != cur.sizePt;
}

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

std::string utf8Encode(uint32_t cp) {
    std::string o;
    if (cp < 0x80) o.push_back(char(cp));
    else if (cp < 0x800) { o.push_back(char(0xC0 | (cp >> 6))); o.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { if (cp >= 0xD800 && cp <= 0xDFFF) return {}; o.push_back(char(0xE0 | (cp >> 12))); o.push_back(char(0x80 | ((cp >> 6) & 0x3F))); o.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp <= 0x10FFFF) { o.push_back(char(0xF0 | (cp >> 18))); o.push_back(char(0x80 | ((cp >> 12) & 0x3F))); o.push_back(char(0x80 | ((cp >> 6) & 0x3F))); o.push_back(char(0x80 | (cp & 0x3F))); }
    return o;
}

KeyPlan planKey(unsigned vk, unsigned uiMods, uint32_t translated, uint32_t base) {
    KeyPlan p;
    if (vk == 0xE5 || vk == 0) return p;  // VK_PROCESSKEY：输入法组字中
    const bool shift = uiMods & 1, alt = uiMods & 2, ctrl = uiMods & 4;
    if (ctrl && alt && translated >= 0x20 && translated != 0x7F) {  // AltGr 字符
        p.text = utf8Encode(translated);
        if (!p.text.empty()) { p.kind = KeyPlan::Text; return p; }
    }
    p.kind = KeyPlan::Key;
    p.vk = vk;
    p.coreMods = (shift ? 1u : 0u) | (ctrl ? 2u : 0u) | (alt ? 4u : 0u);
    if ((ctrl || alt) && base >= 0x20 && base != 0x7F) p.unicode = base;
    return p;
}

}  // namespace it2app

