// T08 PaneBackend（Win32）。线程模型：
// - 传输回调在传输内部线程 → 只往 UiDispatchQueue post，wake = PostMessage(子窗口, WM_PANE_WAKE)。
// - 子窗口消息在 UI 线程 drain 队列：喂核心、处理核心事件、present、回调 PaneEvents。
// - 因此 PaneEvents 只会在 UI 线程（创建子窗口的线程）上被调用。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include "win32_pane_backend.h"
#include "pane_logic.h"
#include "hwnd_swap_host.h"
#include "renderer.h"
#include "grid_metrics.h"
#include "itcore.h"
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

using namespace it2w;

namespace it2app {
namespace {

constexpr UINT WM_PANE_WAKE = WM_APP + 0x41;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 50;
constexpr wchar_t kPaneClass[] = L"it2w_pane_backend";

uint64_t nowMs() { return GetTickCount64(); }

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// 两种传输的统一薄封装
struct Transport {
    virtual ~Transport() = default;
    virtual EnqueueResult input(std::string_view) = 0;
    virtual EnqueueResult reply(std::string_view) = 0;
    virtual EnqueueResult beginPaste(size_t n, bool bracketed) = 0;
    virtual EnqueueResult appendPaste(std::string_view) = 0;
    virtual void resize(GridSize, uint64_t) = 0;
    virtual void close(CloseReason) = 0;
    virtual std::optional<OutputChunk> pop() = 0;
    virtual TransportState state() const = 0;
    virtual bool waitClosed(DWORD) = 0;
    virtual uint32_t processes() = 0;
    virtual bool isSsh() const = 0;
};
struct LocalT final : Transport {
    std::unique_ptr<ConPtyTransport> t;
    EnqueueResult input(std::string_view b) override { return t->enqueueUserInput(b); }
    EnqueueResult reply(std::string_view b) override { return t->enqueueReply(b); }
    EnqueueResult beginPaste(size_t n, bool br) override { return t->beginPaste(n, br); }
    EnqueueResult appendPaste(std::string_view b) override { return t->appendPaste(b); }
    void resize(GridSize g, uint64_t gen) override { t->resize(g, gen); }
    void close(CloseReason r) override { t->close(r); }
    std::optional<OutputChunk> pop() override { return t->tryPopOutput(); }
    TransportState state() const override { return t->state(); }
    bool waitClosed(DWORD ms) override { return t->waitClosed(ms); }
    uint32_t processes() override { return t->activeProcessCount(); }
    bool isSsh() const override { return false; }
};
struct SshT final : Transport {
    std::unique_ptr<SshTransport> t;
    EnqueueResult input(std::string_view b) override { return t->enqueueUserInput(b); }
    EnqueueResult reply(std::string_view b) override { return t->enqueueReply(b); }
    EnqueueResult beginPaste(size_t n, bool br) override { return t->beginPaste(n, br); }
    EnqueueResult appendPaste(std::string_view b) override { return t->appendPaste(b); }
    void resize(GridSize g, uint64_t gen) override { t->resize(g, gen); }
    void close(CloseReason r) override { t->close(r); }
    std::optional<OutputChunk> pop() override { return t->tryPopOutput(); }
    TransportState state() const override { return t->state(); }
    bool waitClosed(DWORD ms) override { return t->waitClosed(ms); }
    uint32_t processes() override { return t->activeProcessCount(); }
    bool isSsh() const override { return true; }
};

class Win32PaneBackend final : public uishell::PaneBackend {
public:
    Win32PaneBackend(std::string connectionId, uishell::PaneEvents ev, std::shared_ptr<const PaneBackendConfig> cfg)
        : connId_(std::move(connectionId)), ev_(std::move(ev)), cfg_(std::move(cfg)) {
        status_ = {uishell::SessionState::Starting, 0, false, {}, {}};
    }
    ~Win32PaneBackend() override { teardown(); }

    bool attach(void *parentHwnd, const uishell::PixelRect &rc, float dpi) override;
    void setBounds(const uishell::PixelRect &rc, float dpi) override;
    void setFocused(bool f) override;
    void sendInput(std::string_view utf8) override;
    void paste(std::string_view utf8) override;
    uishell::CaretRect caretRect() const override;
    uishell::SessionStatus status() const override;
    std::string selectedText() const override;
    void clearSelection() override;
    void requestClose() override;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    bool startSession();
    void fail(std::string text);
    void pump();                       // UI 线程：取输出 → 喂核心 → 事件 → present
    void drainCoreEvents();
    void present();
    void releaseSnap();
    void onTransportState(TransportState, std::optional<TransportFault>);
    void finishSession();
    void publishStatus();
    void flushTitle();
    void handleClick(int px, int py);
    void handleMouse(UINT msg, WPARAM wp, LPARAM lp);
    void applyBounds(const uishell::PixelRect &rc, float dpi);
    void teardown();
    void startCloser(CloseReason why);
    void onCloseDone(const CloseOutcome &r);
    void syncSelection();               // 选区有/无变化 → onSelectionChanged
    void selectionChanged();            // 选区显示变化：重绘

    std::string connId_;
    uishell::PaneEvents ev_;
    std::shared_ptr<const PaneBackendConfig> cfg_;
    std::shared_ptr<UiDispatchQueue> queue_ = std::make_shared<UiDispatchQueue>();
    std::atomic<HWND> hwnd_{nullptr};
    HWND parent_ = nullptr;
    std::unique_ptr<iterm_app::HwndSwapChainHost> host_;
    std::unique_ptr<itren::D2DRenderer> renderer_;
    itren::GridMetrics metrics_{};
    itcore_core *core_ = nullptr;
    itcore_snapshot *snap_ = nullptr;
    // shared：关闭线程持有一份，传输层的阻塞关闭与析构都在那条线程上完成
    std::shared_ptr<Transport> transport_;
    SessionCloser closer_;
    std::optional<CloseOutcome> closeOutcome_;
    DragSelect drag_;
    bool selActive_ = false, dragSelStarted_ = false;
    ResizeTracker resize_;
    RateLimiter bell_{1000};
    TitleThrottle title_{100};
    std::shared_ptr<std::atomic<bool>> pumpPending_ = std::make_shared<std::atomic<bool>>(false);
    mutable std::mutex statusMu_;
    uishell::SessionStatus status_;
    std::optional<SshExitInfo> sshExit_;
    std::optional<uint32_t> localExit_;
    bool focused_ = false, closing_ = false, finished_ = false, skipChar_ = false;
    int downCol_ = -1, downRow_ = -1;
};

}  // namespace

LRESULT CALLBACK Win32PaneBackend::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto *self = reinterpret_cast<Win32PaneBackend *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self || m == WM_NCCREATE) return DefWindowProcW(h, m, w, l);
    return self->onMessage(m, w, l);
}

bool Win32PaneBackend::attach(void *parentHwnd, const uishell::PixelRect &rc, float dpi) {
    if (hwnd_ || !parentHwnd) return false;
    static ATOM atom = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Win32PaneBackend::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        wc.lpszClassName = kPaneClass;
        return RegisterClassExW(&wc);
    }();
    if (!atom) return false;
    parent_ = static_cast<HWND>(parentHwnd);
    HWND h = CreateWindowExW(0, kPaneClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             rc.x, rc.y, rc.w > 1 ? rc.w : 1, rc.h > 1 ? rc.h : 1, parent_, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (!h) return false;
    hwnd_ = h;
    host_ = std::make_unique<iterm_app::HwndSwapChainHost>(h);
    float scale = dpi > 0 ? dpi : host_->dpi_scale();
    metrics_ = itren::GridMetrics::from_font(7.2f, 10.f, 3.f, 0.f, scale);
    uint16_t c = 80, r = 24;
    metrics_.grid_size(rc.w, rc.h, c, r);
    resize_ = ResizeTracker({c, r});
    renderer_ = std::make_unique<itren::D2DRenderer>(host_.get());
    if (!renderer_->init(metrics_, cfg_->fontFamily.c_str(), cfg_->fontSizePt))
        std::fprintf(stderr, "[pane] D2DRenderer::init 失败（设备丢失时 present 内重试）\n");
    // 传输线程的 post 从这里开始能唤醒 UI 线程；attach 之前排队的任务立即补唤醒
    // wake 只在队列未 close 时被调用（close 在析构最先执行），故可引用 this
    queue_->setWake([this] { if (HWND w = hwnd_.load()) PostMessageW(w, WM_PANE_WAKE, 0, 0); });
    SetTimer(h, kTickTimer, kTickMs, nullptr);
    if (!startSession()) return true;  // 窗口已建，状态为 Failed，UI 可显示错误
    return true;
}

bool Win32PaneBackend::startSession() {
    itcore_options opt{};
    opt.abi_version = ITCORE_ABI_VERSION;
    opt.struct_size = sizeof(opt);
    opt.columns = resize_.dims().cols;
    opt.rows = resize_.dims().rows;
    opt.scrollback_lines = cfg_->scrollbackLines;
    opt.memory_limit_bytes = cfg_->memoryLimitBytes;
    if (itcore_create(&opt, &core_) != 0 || !core_) { core_ = nullptr; fail("itcore_create 失败"); return false; }
    itcore_resize(core_, resize_.dims().cols, resize_.dims().rows, resize_.generation());
    renderer_->set_expected_generation(resize_.generation());

    GridSize gs{resize_.dims().cols, resize_.dims().rows};
    // 传输线程回调只捕获共享队列（不捕获 this）：窗格销毁后传输仍可能在关闭线程上回调，
    // 此时队列已 close，post 被丢弃；被 post 的任务只在 UI 线程 drain 时（对象存活）执行。
    auto q = queue_;
    auto pending = pumpPending_;
    auto onState = [this, q](SessionId, AttemptId, TransportState s, std::optional<TransportFault> f) {
        q->post([this, s, f] { onTransportState(s, f); });
    };
    auto onOut = [this, q, pending](SessionId, AttemptId) {
        if (!pending->exchange(true)) q->post([this, pending] { *pending = false; pump(); });
    };
    auto onDiag = [](SessionId, AttemptId, const DiagRecord &d) {
        std::fprintf(stderr, "[pane] diag %s %llums %s\n", d.stage.c_str(), (unsigned long long)d.elapsedMs, d.detail.c_str());
    };
    if (connId_.empty()) {
        TransportCallbacks cb;
        cb.onStateChanged = onState;
        cb.onOutputAvailable = onOut;
        cb.onExited = [this, q](SessionId, AttemptId, uint32_t code) { q->post([this, code] { localExit_ = code; }); };
        cb.onDiagnostic = onDiag;
        auto lt = std::make_unique<LocalT>();
        lt->t = std::make_unique<ConPtyTransport>(cb);
        LaunchSpec spec;
        spec.executable = cfg_->localShell;
        if (spec.executable.empty()) {
            wchar_t buf[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
            spec.executable = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\Windows\\System32\\cmd.exe";
        }
        spec.initialSize = gs;  // environment 为空 → 继承
        auto *raw = lt->t.get();
        transport_ = std::move(lt);
        if (!raw->start(SessionId{1}, AttemptId{1}, spec)) { fail("本地 shell 启动失败"); return false; }
    } else {
        std::string err;
        std::optional<SshLaunchOptions> o = cfg_->resolveSsh ? cfg_->resolveSsh(connId_, &err) : std::nullopt;
        if (!o) { fail(err.empty() ? "找不到连接配置" : err); return false; }
        o->initialSize = gs;
        SshCallbacks cb;
        cb.onStateChanged = onState;
        cb.onOutputAvailable = onOut;
        cb.onFinished = [this, q](SessionId, AttemptId, const SshExitInfo &x) { q->post([this, x] { sshExit_ = x; }); };
        cb.onDiagnostic = onDiag;
        auto st = std::make_unique<SshT>();
        st->t = std::make_unique<SshTransport>(cb);
        auto *raw = st->t.get();
        transport_ = std::move(st);
        if (auto se = raw->start(SessionId{1}, AttemptId{1}, *o)) {
            fail("SSH 配置快照无效：" + se->field + " " + se->reason);
            return false;
        }
    }
    return true;
}

void Win32PaneBackend::fail(std::string text) {
    { std::lock_guard l(statusMu_); status_.state = uishell::SessionState::Failed; status_.errorText = std::move(text); }
    finished_ = true;
    publishStatus();
}

void Win32PaneBackend::publishStatus() {
    if (ev_.onStatus) ev_.onStatus(status());
}

void Win32PaneBackend::onTransportState(TransportState s, std::optional<TransportFault>) {
    if (s == TransportState::Running) {
        { std::lock_guard l(statusMu_); if (status_.state == uishell::SessionState::Starting) status_.state = uishell::SessionState::Running; }
        publishStatus();
    } else if (s == TransportState::Closed || s == TransportState::Failed) {
        // 回调排在 onExited/onFinished 之后（同一 closer 线程先后 post），退出信息此时已到。
        // 用户关闭中：由关闭线程的结果（onCloseDone）收尾，以便上报超时。
        if (closer_.started() && !closeOutcome_) return;
        finishSession();
    }
}

// T03A §1.4 第 8 步：传输 Closed/Failed 后，UI 线程排空剩余输出、finish_input、发布最终快照与状态
void Win32PaneBackend::finishSession() {
    if (finished_) return;
    finished_ = true;
    pump();
    if (core_) itcore_finish_input(core_);
    drainCoreEvents();
    present();
    {
        std::lock_guard l(statusMu_);
        bool failed = transport_ && transport_->state() == TransportState::Failed && !closing_;
        if (closeOutcome_ && closeOutcome_->timedOut) {
            status_.state = uishell::SessionState::Failed;
            status_.errorText = closeOutcome_->detail;
        } else if (sshExit_ && sshExit_->error != SshError::None && sshExit_->error != SshError::RemoteExit) {
            status_.state = uishell::SessionState::Failed;
            status_.errorText = std::string(sshErrorName(sshExit_->error)) + (sshExit_->detail.empty() ? "" : ": " + sshExit_->detail);
            status_.exitCode = int(sshExit_->exitCode);
        } else if (failed) {
            status_.state = uishell::SessionState::Failed;
            status_.errorText = "会话传输故障";
        } else {
            status_.state = uishell::SessionState::Exited;
            status_.exitCode = sshExit_ ? int(sshExit_->exitCode) : int(localExit_.value_or(0));
        }
        status_.hasForegroundJob = false;
    }
    publishStatus();
}

void Win32PaneBackend::releaseSnap() {
    if (snap_) {
        if (renderer_) renderer_->set_snapshot(nullptr);
        itcore_snapshot_release(snap_);
        snap_ = nullptr;
    }
}

void Win32PaneBackend::drainCoreEvents() {
    if (!core_) return;
    while (itcore_event *e = itcore_next_event(core_)) {
        size_t n = 0;
        const uint8_t *p = itcore_event_data(e, &n);
        switch (itcore_event_type(e)) {
        case ITCORE_EV_REPLY_BYTES:
            if (p && n && transport_) transport_->reply(std::string_view(reinterpret_cast<const char *>(p), n));
            break;
        case ITCORE_EV_BELL:
            if (bell_.allow(nowMs()) && ev_.onBell) ev_.onBell();
            break;
        case ITCORE_EV_TITLE:
            if (auto t = title_.offer(std::string(reinterpret_cast<const char *>(p), p ? n : 0), nowMs())) {
                { std::lock_guard l(statusMu_); status_.title = *t; }
                publishStatus();
            }
            break;
        default: break;
        }
        itcore_event_release(e);
    }
}

void Win32PaneBackend::flushTitle() {
    if (auto t = title_.flush(nowMs())) {
        { std::lock_guard l(statusMu_); status_.title = *t; }
        publishStatus();
    }
}

void Win32PaneBackend::pump() {
    if (!core_ || !transport_) return;
    bool any = false;
    while (auto chunk = transport_->pop()) {
        any = true;
        const auto &b = chunk->bytes;
        size_t off = 0;
        while (off < b.size()) {
            auto fr = itcore_feed(core_, reinterpret_cast<const uint8_t *>(b.data() + off), b.size() - off);
            off += fr.accepted ? fr.accepted : b.size() - off;
            drainCoreEvents();
            if (fr.status == ITCORE_FATAL) { transport_->close(CloseReason::Fault); return; }
        }
    }
    drainCoreEvents();
    if (any) syncSelection();  // 输出可能清掉选区（切备用屏 / RIS / ED 3）
    if (any) {
        present();
        if (ev_.onRedrawRequested) ev_.onRedrawRequested();
    }
}

void Win32PaneBackend::present() {
    if (!renderer_ || !core_) return;
    releaseSnap();
    snap_ = itcore_acquire_snapshot(core_, 0, resize_.dims().rows);
    if (!snap_) return;
    renderer_->set_expected_generation(resize_.generation());
    renderer_->set_snapshot(snap_);
    renderer_->present_frame();  // 设备丢失在 present_frame 内 recover_device
}

void Win32PaneBackend::applyBounds(const uishell::PixelRect &rc, float dpi) {
    HWND h = hwnd_;
    if (!h) return;
    SetWindowPos(h, nullptr, rc.x, rc.y, rc.w, rc.h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!renderer_) return;
    float scale = dpi > 0 ? dpi : host_->dpi_scale();
    if (scale != metrics_.dpi_scale) metrics_ = itren::GridMetrics::from_font(7.2f, 10.f, 3.f, 0.f, scale);
    renderer_->metrics() = metrics_;
    uint16_t c = 0, r = 0;
    metrics_.grid_size(rc.w, rc.h, c, r);  // 已钳到 ≥20×5；ResizeTracker 再兜底
    if (auto gen = resize_.update(c, r)) {
        // T03A：先 itcore_resize(gen)，再 transport.resize(gen)；渲染按 expected generation 保持旧布局直到快照对齐
        if (core_) itcore_resize(core_, resize_.dims().cols, resize_.dims().rows, *gen);
        if (transport_ && !closing_) transport_->resize(GridSize{resize_.dims().cols, resize_.dims().rows}, *gen);
        renderer_->set_expected_generation(*gen);
        releaseSnap();
    }
    renderer_->resize_surface();
    present();
}

void Win32PaneBackend::setBounds(const uishell::PixelRect &rc, float dpi) { applyBounds(rc, dpi); }

void Win32PaneBackend::setFocused(bool f) {
    focused_ = f;
    HWND h = hwnd_;
    if (!h) return;
    if (f && GetFocus() != h) SetFocus(h);
    present();  // 光标样式随焦点变化（渲染器目前未区分，留作实机项）
}

void Win32PaneBackend::sendInput(std::string_view utf8) {
    if (!transport_ || closing_ || finished_ || utf8.empty()) return;
    auto r = transport_->input(utf8);
    if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] enqueueUserInput %d\n", int(r));
}

void Win32PaneBackend::paste(std::string_view utf8) {
    if (!transport_ || !core_ || closing_ || finished_ || utf8.empty()) return;
    auto rejected = [&] { if (ev_.onPasteRejected) ev_.onPasteRejected(utf8.size(), kPasteLimitBytes); };
    // 核心按 DECSET 2004 决定是否加括号标记，并清洗正文（NUL / 内嵌起止标记）
    itcore_buffer *enc = nullptr;
    int rc = itcore_encode_paste(core_, utf8.data(), utf8.size(), &enc);
    if (rc == -2) { rejected(); return; }
    if (rc != 0 || !enc) return;
    std::string bytes(reinterpret_cast<const char *>(enc->data), enc->length);
    itcore_buffer_release(enc);
    if (bytes.empty()) return;
    // 走传输的粘贴事务（与用户输入不交错）；标记已在 bytes 里，事务不再补
    auto r = transport_->beginPaste(bytes.size(), false);
    if (r == EnqueueResult::Ok) r = transport_->appendPaste(bytes);
    if (r == EnqueueResult::TooLarge) rejected();
    else if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] paste %d (%zu bytes)\n", int(r), bytes.size());
}

uishell::CaretRect Win32PaneBackend::caretRect() const {
    if (!renderer_ || !snap_) return {0, 0, 0, 0, false};
    uint16_t c = 0, r = 0;
    int vis = 0;
    itcore_snapshot_cursor(snap_, &c, &r, &vis);
    auto rc = renderer_->caret_rect_px();  // 含预编辑锚点
    return {rc.x, rc.y, rc.w, rc.h, true};
}

uishell::SessionStatus Win32PaneBackend::status() const {
    uishell::SessionStatus s;
    { std::lock_guard l(statusMu_); s = status_; }
    if (s.state == uishell::SessionState::Running && transport_) {
        // 本地：Job 内除 shell 外还有进程 → 有前台任务（ConPTY 的 conhost 不在我们的 Job 里，待实机确认）。
        // SSH：远端进程不可见；连接存活即视为有前台任务，关闭时总需确认。
        s.hasForegroundJob = transport_->isSsh() ? true : transport_->processes() > 1;
    }
    return s;
}

// 关闭线程预算：传输层自身 3 s 预算（含强杀）+ 余量；超出即上报 Failed，线程继续等传输结束
constexpr DWORD kCloserBudgetMs = 5000;

void Win32PaneBackend::startCloser(CloseReason why) {
    std::shared_ptr<Transport> t = transport_;
    closer_.start(queue_,
        [t, why]() {
            CloseOutcome r;
            t->close(why);  // T03A §1.4 九步由传输层 closer 线程执行；这里只等它在预算内结束
            if (!t->waitClosed(kCloserBudgetMs)) { r.ok = false; r.timedOut = true; r.detail = "关闭超时，已强制结束"; }
            return r;
        },
        [this](const CloseOutcome &r) { onCloseDone(r); },
        [t]() mutable { t->waitClosed(INFINITE); t.reset(); });
}

void Win32PaneBackend::onCloseDone(const CloseOutcome &r) {
    closeOutcome_ = r;
    finishSession();  // UI 线程：排空剩余输出、finish_input、最终快照，onStatus(Exited/Failed)
}

void Win32PaneBackend::requestClose() {
    if (closing_ || finished_) return;
    closing_ = true;
    if (transport_) startCloser(CloseReason::User);  // 立即返回；结果经 UI 队列回到 onStatus
    else finishSession();
}

std::string Win32PaneBackend::selectedText() const {
    if (!core_) return {};
    itcore_buffer *b = nullptr;
    if (itcore_selection_text(core_, &b) != 0 || !b) return {};
    std::string s(reinterpret_cast<const char *>(b->data), b->length);
    itcore_buffer_release(b);
    return s;
}

void Win32PaneBackend::clearSelection() {
    drag_.cancel();
    if (!core_ || !itcore_selection_active(core_)) { syncSelection(); return; }
    itcore_selection_clear(core_);
    selectionChanged();
}

void Win32PaneBackend::selectionChanged() {
    drainCoreEvents();
    present();
    if (ev_.onRedrawRequested) ev_.onRedrawRequested();
    syncSelection();
}

void Win32PaneBackend::syncSelection() {
    bool now = core_ && itcore_selection_active(core_);
    if (now == selActive_) return;
    selActive_ = now;
    if (ev_.onSelectionChanged) ev_.onSelectionChanged(now);
}

void Win32PaneBackend::handleClick(int px, int py) {
    if (!snap_ || !ev_.onLinkActivated) return;
    uint16_t cols = itcore_snapshot_columns(snap_), rows = itcore_snapshot_rows(snap_);
    if (!cols || !rows) return;
    uint16_t c = 0, r = 0;
    metrics_.hit_test(px, py, cols, rows, c, r);
    if (c != downCol_ || r != downRow_) return;  // 拖动不算点击
    const itcore_cell *row = itcore_snapshot_row(snap_, r, nullptr, nullptr);
    if (!row || !row[c].hyperlink_id) return;
    const char *url = nullptr, *id = nullptr;
    size_t ul = 0, il = 0;
    if (!itcore_snapshot_hyperlink(snap_, row[c].hyperlink_id, &url, &ul, &id, &il) || !url) return;
    ev_.onLinkActivated(std::string(url, ul));  // 只交出 URL，由 UI 跑 checkLink
}

void Win32PaneBackend::handleMouse(UINT msg, WPARAM wp, LPARAM lp) {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    uint32_t mode = snap_ ? itcore_snapshot_mouse_mode(snap_) : 0;
    uint16_t cols = snap_ ? itcore_snapshot_columns(snap_) : 0, rows = snap_ ? itcore_snapshot_rows(snap_) : 0;
    if (!cols || !rows) return;
    uint16_t c = 0, r = 0;
    metrics_.hit_test(x, y, cols, rows, c, r);
    if (mode && !(wp & MK_SHIFT) && core_) {  // Shift 强制本地（点链接）
        itcore_mouse_event me{};
        me.struct_size = sizeof(me);
        me.col = c; me.row = r;
        me.button = msg == WM_MOUSEMOVE ? ((wp & MK_LBUTTON) ? ITCORE_MBUTTON_LEFT : ITCORE_MBUTTON_NONE) : ITCORE_MBUTTON_LEFT;
        me.action = msg == WM_LBUTTONDOWN ? ITCORE_MOUSE_PRESS : msg == WM_LBUTTONUP ? ITCORE_MOUSE_RELEASE : ITCORE_MOUSE_MOVE;
        if (wp & MK_CONTROL) me.modifiers |= ITCORE_MOD_CTRL;
        itcore_buffer *b = nullptr;
        if (itcore_encode_mouse(core_, &me, &b) == 0 && b) {
            sendInput(std::string_view(reinterpret_cast<const char *>(b->data), b->length));
            itcore_buffer_release(b);
        }
        return;
    }
    // 本地：拖动选择（核心存 (逻辑行, 字符) 端点，跟随滚动/重排），单击清选区并检查链接
    if (!core_) return;
    DragSelect::Cell cell{c, r};
    if (msg == WM_LBUTTONDOWN) { downCol_ = c; downRow_ = r; drag_.down(cell); dragSelStarted_ = false; return; }
    auto startSel = [&] {
        auto p = drag_.pressCell();
        itcore_selection_start(core_, 0, uint16_t(p.col), uint16_t(p.row));
        dragSelStarted_ = true;
    };
    DragSelect::Act act = msg == WM_LBUTTONUP ? drag_.up(cell) : drag_.move(cell);
    switch (act) {
    case DragSelect::Act::Start:
        startSel();
        itcore_selection_extend(core_, 0, c, r);
        selectionChanged();
        break;
    case DragSelect::Act::Extend:
        if (itcore_selection_extend(core_, 0, c, r)) selectionChanged();
        break;
    case DragSelect::Act::Finish:
        if (!dragSelStarted_) startSel();
        itcore_selection_extend(core_, 0, c, r);
        selectionChanged();
        break;
    case DragSelect::Act::Click:
        if (itcore_selection_active(core_)) { itcore_selection_clear(core_); selectionChanged(); }
        handleClick(x, y);
        break;
    case DragSelect::Act::None: break;
    }
    if (msg == WM_LBUTTONUP) downCol_ = downRow_ = -1;
}

LRESULT Win32PaneBackend::onMessage(UINT m, WPARAM w, LPARAM l) {
    HWND h = hwnd_;
    switch (m) {
    case WM_PANE_WAKE: queue_->drain(); return 0;
    case WM_TIMER:
        if (w == kTickTimer) { queue_->drain(); pump(); flushTitle(); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        present();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE:
        // 点击窗格：交给 UI 决定焦点（UI 再调 setFocused）
        if (parent_) SendMessageW(parent_, m, w, l);
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN: SetCapture(h); handleMouse(m, w, l); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); handleMouse(m, w, l); return 0;
    case WM_MOUSEMOVE: if (w & MK_LBUTTON) handleMouse(m, w, l); return 0;
    // 键盘：接口约定按键由 UI 翻译，子窗口拿到焦点后原样转给父窗口（外壳）处理，再由它调 sendInput
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
        if (parent_) return SendMessageW(parent_, m, w, l);
        break;
    case WM_CHAR:
        if (skipChar_) { skipChar_ = false; return 0; }
        if (parent_) return SendMessageW(parent_, m, w, l);
        break;
    // IME 必须在持有焦点的窗口上处理（候选窗定位、预编辑由渲染器画）
    case WM_IME_COMPOSITION: {
        HIMC imc = ImmGetContext(h);
        if (!imc) break;
        if (l & GCS_RESULTSTR) {
            LONG nb = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
            if (nb > 0) {
                std::wstring ws(size_t(nb / sizeof(wchar_t)), L'\0');
                ImmGetCompositionStringW(imc, GCS_RESULTSTR, ws.data(), nb);
                auto u8 = toUtf8(ws);
                if (core_ && !u8.empty()) {
                    itcore_buffer *b = nullptr;
                    if (itcore_encode_text(core_, u8.data(), u8.size(), &b) == 0 && b) {
                        sendInput(std::string_view(reinterpret_cast<const char *>(b->data), b->length));
                        itcore_buffer_release(b);
                    }
                    skipChar_ = true;
                }
            }
            if (renderer_) renderer_->clear_preedit();
        } else if (l & GCS_COMPSTR) {
            LONG nb = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
            itren::Preedit pe;
            if (nb > 0) {
                std::wstring ws(size_t(nb / sizeof(wchar_t)), L'\0');
                ImmGetCompositionStringW(imc, GCS_COMPSTR, ws.data(), nb);
                pe.utf8 = toUtf8(ws);
            }
            if (snap_) { int vis = 0; itcore_snapshot_cursor(snap_, &pe.anchor_col, &pe.anchor_row, &vis); }
            if (renderer_) renderer_->set_preedit(pe);
        }
        auto cr = caretRect();
        if (cr.valid) {
            CANDIDATEFORM cf{};
            cf.dwStyle = CFS_EXCLUDE;
            cf.ptCurrentPos = {cr.x, cr.y + cr.h};
            cf.rcArea = {cr.x, cr.y, cr.x + cr.w, cr.y + cr.h};
            ImmSetCandidateWindow(imc, &cf);
        }
        ImmReleaseContext(h, imc);
        present();
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        if (renderer_) renderer_->clear_preedit();
        present();
        break;
    case WM_IME_REQUEST:
        if (w == IMR_QUERYCHARPOSITION) {
            auto cr = caretRect();
            if (!cr.valid) break;
            auto *q = reinterpret_cast<IMECHARPOSITION *>(l);
            POINT pt{cr.x, cr.y};
            ClientToScreen(h, &pt);
            q->pt = pt;
            q->cLineHeight = UINT(cr.h);
            RECT rc{};
            GetClientRect(h, &rc);
            MapWindowPoints(h, nullptr, reinterpret_cast<POINT *>(&rc), 2);
            q->rcDocument = rc;
            return 1;
        }
        break;
    case WM_SETFOCUS: {
        focused_ = true;
        auto cr = caretRect();
        if (HIMC imc = ImmGetContext(h)) {
            COMPOSITIONFORM cf{};
            cf.dwStyle = CFS_POINT;
            if (cr.valid) cf.ptCurrentPos = {cr.x, cr.y + cr.h};
            ImmSetCompositionWindow(imc, &cf);
            ImmReleaseContext(h, imc);
        }
        return 0;
    }
    case WM_KILLFOCUS: focused_ = false; return 0;
    case WM_DESTROY:
        KillTimer(h, kTickTimer);
        hwnd_ = nullptr;  // 之后的 post 不再 PostMessage
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void Win32PaneBackend::teardown() {
    // 不阻塞 UI：先 close 队列（此后 PaneEvents 不再被调用，传输回调的 post 被丢弃），
    // 再把传输交给关闭线程（若用户关闭尚未开始），本线程只放掉自己的引用，不 join、不等待。
    queue_->close();
    if (transport_ && !closer_.started()) startCloser(CloseReason::User);
    transport_.reset();
    releaseSnap();
    if (renderer_) { renderer_->shutdown(); renderer_.reset(); }
    if (HWND h = hwnd_.exchange(nullptr)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    host_.reset();
    if (core_) { itcore_destroy(core_); core_ = nullptr; }
}

uishell::PaneBackendFactory makeWin32PaneBackendFactory(PaneBackendConfig cfg) {
    auto shared = std::make_shared<const PaneBackendConfig>(std::move(cfg));
    return [shared](const std::string &connectionId, uishell::PaneEvents ev) -> std::unique_ptr<uishell::PaneBackend> {
        return std::make_unique<Win32PaneBackend>(connectionId, std::move(ev), shared);
    };
}

}  // namespace it2app
