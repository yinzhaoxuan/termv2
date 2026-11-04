// SPDX-License-Identifier: GPL-3.0-or-later
#include "itcore.h"
#include "pane_logic.h"
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <utility>
#if defined(_WIN32)
#include <wchar.h>
#endif

namespace it2app {

uint64_t sessionMemoryTotal(const SessionMemoryInputs& inputs) {
    uint64_t bytes = inputs.core.total_bytes + inputs.archiveBytes;
    if (inputs.hasSnapshot) {
        bytes += inputs.snapshotRowBytes;
        if (inputs.snapshotEpoch != inputs.core.pool_epoch) bytes += inputs.snapshotPoolBytes;
    }
    return bytes;
}


ClipboardReadGate::Action ClipboardReadGate::onRequest(bool canAsk, uint64_t *id) {
    if (id) *id = 0;
    if (!canAsk) return Action::Drop;  // 无确认回调时，即使曾记住允许也拒绝
    if (sessionAllowed_) return *sessionAllowed_ ? Action::Deliver : Action::Drop;
    if (pending()) return Action::Busy;
    pendingId_ = ++nextId_;
    if (id) *id = pendingId_;
    return Action::Ask;
}

bool ClipboardReadGate::onAnswer(uint64_t id, uishell::ClipboardReadDecision decision) {
    if (!pending() || id != pendingId_) return false;
    pendingId_ = 0;
    using Decision = uishell::ClipboardReadDecision;
    if (decision == Decision::AllowSession) sessionAllowed_ = true;
    else if (decision == Decision::DenySession) sessionAllowed_ = false;
    return decision == Decision::AllowOnce || decision == Decision::AllowSession;
}

void ClipboardReadGate::reset() {
    sessionAllowed_.reset();
    pendingId_ = 0;
}

itcore_colors toCoreColors(const uishell::PaneColors& colors) {
    itcore_colors out{};
    itcore_default_colors(&out);
    out.foreground = colors.foreground; out.background = colors.background; out.cursor = colors.cursor;
    std::copy(colors.ansi.begin(), colors.ansi.end(), out.palette);
    return out;
}

uishell::PaneAppearance coreDefaultAppearance() {
    itcore_colors colors{};
    itcore_default_colors(&colors);
    uishell::PaneAppearance out{};
    out.colors.foreground = colors.foreground;
    out.colors.background = colors.background;
    out.colors.cursor = colors.cursor;
    std::copy_n(colors.palette, out.colors.ansi.size(), out.colors.ansi.begin());
    return out;
}

PumpDecision decidePumpStep(DrainPhase phase, PumpResult r, bool carryEmpty) {
    if (phase == DrainPhase::Finished) return {};
    if (r == PumpResult::Fatal) return {phase == DrainPhase::Draining, false, false, phase == DrainPhase::Running};
    if (phase == DrainPhase::Draining && r != PumpResult::FedMore && carryEmpty) return {true, false, false, false};
    return {false, r == PumpResult::FedMore || !carryEmpty, r != PumpResult::Idle, false};
}
CloseGraceAction decideCloseTimeoutGrace(DrainPhase phase, bool inputEnded, bool closeTimedOut,
                                         CloseGraceState state, bool hasClock, uint64_t nowMs,
                                         uint64_t graceMs) {
    if (phase != DrainPhase::Draining || inputEnded || !closeTimedOut) return CloseGraceAction::None;
    if (!state.armed) return CloseGraceAction::ArmAndWait;
    if (!hasClock || nowMs < state.startMs || nowMs - state.startMs < graceMs) return CloseGraceAction::Wait;
    return CloseGraceAction::Abandon;
}
FrameMode chooseFrameMode(DrainPhase phase, bool pending, bool force) {
    if (force) return FrameMode::Force;
    return phase == DrainPhase::Draining || (phase == DrainPhase::Running && pending) ? FrameMode::Immediate : FrameMode::VSync;
}
bool opAllowed(DrainPhase phase, PaneOp op) {
    switch (op) {
    case PaneOp::Input: case PaneOp::Close: case PaneOp::ResizeTransport: return phase == DrainPhase::Running;
    default: return true;
    }
}
TerminalStatus classifyTerminal(const TerminalInputs &in) {
    TerminalStatus out;
    out.exitCode = in.exitCode;
    out.droppedOutputBytes = in.coreDropped + in.queueDropped;
    if (in.timedOut) { out.failed = true; out.error = in.timeoutDetail; }
    else if (in.sshFailed) { out.failed = true; out.error = in.sshDetail; }
    else if (in.transportFailed && !in.closing) { out.failed = true; out.error = in.transportDetail.empty() ? "会话传输故障" : in.transportDetail; }
    // ui/ 接口暂不加字段；沿既有终态 errorText 回传计数，纯终态保留数值字段。
    if (out.droppedOutputBytes) {
        if (!out.error.empty()) out.error += "; ";
        out.error += "输出可能不完整，丢弃字节数=" + std::to_string(out.droppedOutputBytes);
    }
    return out;
}
SyncStartOutcome settleSyncStart(SyncStart result, std::string error) {
    SyncStartOutcome out;
    out.reconnectAllowed = true;
    if (result == SyncStart::PreStartFailed) {
        out.failed = true;
        out.error = std::move(error);
    }
    return out;
}
std::optional<std::string> immediatePreStartFailure(bool sessionStillStarting, bool transportFailed,
                                                    bool hvFailed, std::string hvError) {
    if (!sessionStillStarting || !transportFailed || !hvFailed) return std::nullopt;
    if (hvError.empty()) hvError = "会话启动失败";
    return hvError;
}
std::string formatTransportFailure(std::string_view category, std::string_view text) {
    if (category.empty() || category == "None")
        return text.empty() ? std::string("会话传输故障") : std::string(text);
    if (text.empty()) return std::string(category);
    return std::string(category) + ": " + std::string(text);
}
uishell::SessionRecovery hyperVRecovery(it2w::HvFailure failure, const std::vector<it2w::HvCandidate> &candidates) {
    uishell::SessionRecovery out;
    if (failure == it2w::HvFailure::VmNotRunning) out.kind = uishell::SessionRecovery::StartVmAndConnect;
    else if (failure == it2w::HvFailure::AddressAmbiguous) {
        out.kind = uishell::SessionRecovery::ChooseAddress;
        for (const auto &c : candidates) out.choices.push_back({c.adapterId, c.adapterName + " " + c.address});
    }
    return out;
}
uishell::SessionRecovery sshRecovery(const it2w::SshExitInfo &exit) {
    uishell::SessionRecovery out;
    if (exit.error == it2w::SshError::HostKeyUnknown && exit.hostKey) {
        const auto &key = *exit.hostKey;
        out.kind = uishell::SessionRecovery::TrustHostKey;
        out.hostKey = uishell::HostKeyOffer{key.knownHostsName, key.keyType, key.keyBase64, key.fingerprint};
    }
    return out;
}
std::string formatLocalShellStartError(std::string_view exeUtf8) {
    const std::string path = exeUtf8.empty() ? std::string("cmd.exe") : std::string(exeUtf8);
    return "本地 shell 启动失败：" + path + "。请确认该程序存在且可执行。";
}
std::string formatLocalPathError(LocalPathError error, std::string_view pathUtf8) {
    return std::string(error == LocalPathError::ExecutableNotFound ? "未找到程序：" : "工作目录不存在：") +
           std::string(pathUtf8);
}
std::string terminalLog(uint64_t attempt, const TerminalStatus &s, uint64_t coreDropped, uint64_t queueDropped) {
    return "[pane] session end attempt=" + std::to_string(attempt) + " state=" + (s.failed ? "Failed" : "Exited") +
           " dropped=" + std::to_string(s.droppedOutputBytes) + " (core=" + std::to_string(coreDropped) +
           " queue=" + std::to_string(queueDropped) + ")";
}

namespace {

std::string sanitizePhase(std::string_view phase) {
    std::string out;
    out.reserve(phase.size());
    for (unsigned char c : phase) {
        if (c <= ' ' || c == '=') out.push_back('_');
        else out.push_back(static_cast<char>(c));
    }
    if (out.empty()) out = "-";
    return out;
}

std::string sanitizeMessage(std::string_view message) {
    std::string out;
    out.reserve(message.size());
    for (unsigned char c : message) {
        if (c == '\n' || c == '\r' || c == '\0') out.push_back(' ');
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

std::FILE *openAppend(const std::filesystem::path &path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

}  // namespace

std::string formatPaneDiagLine(uint64_t sessionId, uint64_t attemptId, std::string_view phase,
                               uint64_t elapsedMs, std::optional<int64_t> errorCode,
                               uint64_t bytes, std::string_view message) {
    std::string line;
    line.reserve(80 + phase.size() + message.size());
    line += "v=1 sid=";
    line += std::to_string(sessionId);
    line += " attempt=";
    line += std::to_string(attemptId);
    line += " phase=";
    line += sanitizePhase(phase);
    line += " elapsed_ms=";
    line += std::to_string(elapsedMs);
    line += " err=";
    line += errorCode ? std::to_string(*errorCode) : "-";
    line += " bytes=";
    line += std::to_string(bytes);
    line += " msg=";
    line += sanitizeMessage(message);
    return line;
}

std::filesystem::path paneDiagDirectory(const std::filesystem::path &configDir,
                                        const std::filesystem::path &fallbackConfigDir) {
    if (!configDir.empty()) return configDir / "logs";
    if (!fallbackConfigDir.empty()) return fallbackConfigDir / "logs";
    return {};
}

uint64_t allocateSessionId() {
    static std::atomic<uint64_t> next{1};
    uint64_t id = next.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

PaneDiagLog::~PaneDiagLog() {
    std::lock_guard lock(mu_);
    closeUnlocked();
}

void PaneDiagLog::setDirectory(std::filesystem::path logsDir) {
    std::lock_guard lock(mu_);
    if (logsDir == dir_) return;
    closeUnlocked();
    dir_ = std::move(logsDir);
    size_ = 0;
}

void PaneDiagLog::setMaxBytes(uint64_t bytes) {
    std::lock_guard lock(mu_);
    maxBytes_ = bytes == 0 ? 1 : bytes;
}

int PaneDiagLog::openCount() const {
    std::lock_guard lock(mu_);
    return opens_;
}

void PaneDiagLog::closeUnlocked() {
    if (!fp_) return;
    std::fclose(fp_);
    fp_ = nullptr;
}

void PaneDiagLog::rotateUnlocked() {
    closeUnlocked();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = dir_ / "pane-diag.log";
    auto gen = [&](int n) {
        fs::path p = base;
        if (n) p += "." + std::to_string(n);
        return p;
    };
    // 当前 + .1 + .2，共 3 个。最老的 .2 丢掉。
    fs::remove(gen(2), ec);
    fs::rename(gen(1), gen(2), ec);
    fs::rename(gen(0), gen(1), ec);
    size_ = 0;
    openUnlocked(false);
}

void PaneDiagLog::openUnlocked(bool allowRotate) {
    if (fp_ || dir_.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return;
    const fs::path cur = dir_ / "pane-diag.log";
    size_ = 0;
    if (fs::exists(cur, ec) && !ec) {
        const auto sz = fs::file_size(cur, ec);
        if (!ec) size_ = sz;
        if (allowRotate && size_ > 0 && size_ >= maxBytes_) {
            rotateUnlocked();
            return;
        }
    }
    fp_ = openAppend(cur);
    if (fp_) ++opens_;
    else size_ = 0;
}

void PaneDiagLog::append(std::string_view line) {
    std::lock_guard lock(mu_);
    if (dir_.empty()) return;
    const uint64_t add = static_cast<uint64_t>(line.size()) + 1;
    if (!fp_) openUnlocked(true);
    if (!fp_) return;
    // 空文件允许一行超过上限，避免单条超长记录把滚动打成死循环。下一条再滚。
    if (size_ > 0 && size_ + add > maxBytes_) {
        rotateUnlocked();
        if (!fp_) return;
    }
    const auto n = std::fwrite(line.data(), 1, line.size(), fp_);
    if (n != line.size() || std::fputc('\n', fp_) == EOF) return;
    std::fflush(fp_);
    size_ += static_cast<uint64_t>(n) + 1;
}
void PendingEvents::flush(std::weak_ptr<char> life, Batch events) {
    for (auto &event : events) {
        if (life.expired()) return;
        event(); // 不持有 owner，也不延长令牌寿命；之后只检查局部 weak_ptr。
    }
}
bool envFlagOn(const char *value) { return value && std::string_view(value) == "1"; }
bool wantDirectSsh(std::optional<it2w::SshTransportMode> perConnection, bool envDefault) {
    return perConnection ? *perConnection == it2w::SshTransportMode::Direct : envDefault;
}
SshTransportChoice selectSshTransport(bool wantDirect, bool buildHasDirect, std::string *err) {
    if (err) err->clear();
    if (!wantDirect) return SshTransportChoice::ConPty;
    if (buildHasDirect) return SshTransportChoice::Direct;
    if (err) *err = "此构建未包含直连 SSH；把连接的 ssh.transport 改为 compatible，或去掉 ITERM2WIN_SSH_DIRECT";
    return SshTransportChoice::Unavailable;
}
void osc52DiagPingSequence(int (*reply)(void *, const char *, size_t), void *ctx,
                          void (*log)(void *, const char *), void *logCtx) {
    const std::string_view steps[] = {"PING\r", "\x1b[I", "\x1b]52;c;aGk=\x07"};
    int i = 0;
    for (auto step : steps) {
        int rc = reply(ctx, step.data(), step.size());
        char msg[96];
        std::snprintf(msg, sizeof msg, "[pane] osc52 ping step=%d rc=%d len=%zu", ++i, rc, step.size());
        if (log) log(logCtx, msg);
    }
}
std::string hexPreview(std::string_view bytes, size_t limit) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes.substr(0, limit)) {
        if (!out.empty()) out += ' ';
        out += hex[c >> 4]; out += hex[c & 15];
    }
    return out;
}
std::vector<std::string> Osc52OutputSniffer::observe(std::string_view bytes) {
    std::string joined = tail_; joined.append(bytes);
    std::vector<std::string> hits;
    size_t pos = 0;
    while ((pos = joined.find("]52;", pos)) != std::string::npos) {
        // 保留 ESC + 跨块标记；旧块中完整的标记不重复上报。
        if (pos + 4 > tail_.size()) {
            size_t start = pos && joined[pos - 1] == '\x1b' ? pos - 1 : pos;
            hits.push_back(hexPreview(std::string_view(joined).substr(start)));
        }
        pos += 4;
    }
    tail_ = joined.substr(joined.size() > 4 ? joined.size() - 4 : 0);
    return hits;
}

int applyOsc52(itcore_core *core, const Osc52Config &cfg) {
    if (!core) return -1;
    itcore_clipboard_policy p{sizeof(p), cfg.allowWrite ? 1 : 0, cfg.allowRead ? 1 : 0, cfg.maxBytes};
    return itcore_set_clipboard_policy(core, &p);
}

Osc52ReadDeliver deliverOsc52ReadReply(
    itcore_core *core, const itcore_event *req,
    const char *utf8, size_t len,
    int (*reply)(void *ctx, const char *data, size_t n), void *replyCtx,
    void (*log)(void *ctx, const char *msg), void *logCtx,
    bool logSuccess) {
    auto say = [&](const char *m) { if (log) log(logCtx, m); };
    if (!core || !reply) {
        say("[pane] osc52 read deliver skipped (no core/reply)");
        return Osc52ReadDeliver::EncodeFailed;
    }
    itcore_buffer *b = nullptr;
    const int erc = itcore_encode_clipboard_reply(core, req, utf8, len, &b);
    if (erc != 0 || !b) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "[pane] osc52 read encode failed rc=%d", erc);
        say(msg);
        if (b) itcore_buffer_release(b);
        return Osc52ReadDeliver::EncodeFailed;
    }
    const size_t n = b->length;
    const int rrc = reply(replyCtx, reinterpret_cast<const char *>(b->data), n);
    itcore_buffer_release(b);
    if (rrc != 0) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "[pane] osc52 read reply failed rc=%d len=%zu", rrc, n);
        say(msg);
        return Osc52ReadDeliver::ReplyFailed;
    }
    if (logSuccess) {
        char msg[80];
        std::snprintf(msg, sizeof(msg), "[pane] osc52 read reply ok len=%zu", n);
        say(msg);
    }
    return Osc52ReadDeliver::Ok;
}

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

std::optional<LocalShellLaunch> planLocalShellCommand(const std::wstring &shellPath, const std::wstring &command) {
    LocalShellLaunch out;
    if (command.empty()) return out;
    for (wchar_t c : command) if (c == L'\r' || c == L'\n' || c == L'\0') return std::nullopt;
    auto slash = shellPath.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? shellPath : shellPath.substr(slash + 1);
    for (auto &c : name) if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
    if (name == L"cmd.exe" || name == L"cmd") {
        out.args = {L"/s", L"/k"};
        out.rawTail = L"\"" + command + L"\"";
    } else if (name == L"powershell.exe" || name == L"powershell" || name == L"pwsh.exe" || name == L"pwsh") {
        out.args = {L"-NoExit", L"-Command", command};
    } else {
        out.rawTail = command;
    }
    return out;
}

}  // namespace it2app
