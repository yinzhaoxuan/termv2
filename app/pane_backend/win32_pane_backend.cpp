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
#include "frame_policy.h"
#include "grid_metrics.h"
#include "selection_hit.h"
#include "itcore.h"
#include <algorithm>
#include <climits>
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

struct HyperVT final : Transport {
    std::unique_ptr<HyperVTransport> t;
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
    // 析构：先断开 PaneEvents，再 teardown（close 队列 → 之后的投递一律丢弃）
    ~Win32PaneBackend() override { ev_ = {}; teardown(); }

    bool attach(void *parentHwnd, const uishell::PixelRect &rc, float dpi) override;
    void setBounds(const uishell::PixelRect &rc, float dpi) override;
    void setFocused(bool f) override;
    void sendInput(std::string_view utf8) override;
    void paste(std::string_view utf8) override;
    void sendKey(unsigned vk, unsigned mods) override;
    std::uint64_t previousAttemptId() const override { return prevAttemptId_; }
    std::string previousAttemptText() const override { return prevAttemptText_; }
    void reconnect() override;
    uishell::CaretRect caretRect() const override;
    uishell::SessionStatus status() const override;
    std::string selectedText() const override;
    void clearSelection() override;
    void requestClose() override;
    void scrollViewport(int lines) override;
    void scrollToBottom() override;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    bool startSession();
    void fail(std::string text);
    void pushCellPixelSize();          // GridMetrics → itcore_set_cell_pixel_size
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
    void drainUi();                     // UI 线程：执行投递任务，领取关闭结果
    void syncViewport(bool force = false);  // 历史行数 / 偏移变化 → onViewportChanged
    void handleWheel(WPARAM wp, LPARAM lp);
    void autoScrollTick();
    bool altScreen() const { return core_ && itcore_mode_query(core_, 1049) == ITCORE_MODE_SET; }

    std::string connId_;
    uishell::PaneEvents ev_;
    std::shared_ptr<const PaneBackendConfig> cfg_;
    // 与关闭线程 / 传输回调共享的唯一状态（不含 this、不含 PaneEvents）
    std::shared_ptr<SessionShared> shared_ = std::make_shared<SessionShared>();
    std::shared_ptr<UiDispatchQueue> queue_ = shared_->queue;
    std::atomic<HWND> hwnd_{nullptr};
    HWND parent_ = nullptr;
    std::unique_ptr<iterm_app::HwndSwapChainHost> host_;
    std::unique_ptr<itren::D2DRenderer> renderer_;
    itren::GridMetrics metrics_{};
    SyncGate sync_{ITREN_SYNC_TIMEOUT_MS};   // ?2026 暂停快照（与 D2D 共用常量）
    FocusReporter focusRep_;                 // ?1004
    void reportFocus();
    itcore_core *core_ = nullptr;
    itcore_snapshot *snap_ = nullptr;
    // 同一对象也挂在 shared_->transport：关闭线程经共享状态持有，阻塞关闭与析构都在那条线程上完成
    std::shared_ptr<Transport> transport_;
    SessionCloser closer_;
    std::optional<CloseOutcome> closeOutcome_;
    DragSelect drag_;
    bool selActive_ = false, dragSelStarted_ = false;
    ResizeTracker resize_;
    RateLimiter bell_{1000};
    TitleThrottle title_{100};
    Viewport vp_;
    bool vpNotified_ = false;
    ClickCounter clicks_;
    int clickUnit_ = 0;                 // 0 字符 / 1 词 / 2 行（双击 / 三击后拖动按单位扩展）
    int dragEdge_ = 0;                  // 拖选到上(-1)/下(+1)边缘外：tick 自动滚动
    int dragCol_ = 0;
    int wheelAccum_ = 0;
    mutable std::mutex statusMu_;
    uishell::SessionStatus status_;
    std::optional<SshExitInfo> sshExit_;
    std::optional<HvAction> hvResult_;   // T09 解析结果（Fail 时作为错误文本）
    std::optional<uint32_t> localExit_;
    bool focused_ = false, closing_ = false, finished_ = false, skipChar_ = false;
    int downCol_ = -1, downRow_ = -1;
    std::uint64_t attempt_ = 1;          // 当前 attempt；reconnect +1
    std::uint64_t prevAttemptId_ = 0;    // 0 = 无旧归档
    std::string prevAttemptText_;        // 旧 attempt 只读逻辑文本（≤ kPrevAttemptTextBytes）
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
    // wake 在队列锁内调用、close 时清空，只捕获窗口句柄（不捕获 this）
    queue_->setWake([h] { PostMessageW(h, WM_PANE_WAKE, 0, 0); });
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
    // 首次 feed 前：GridMetrics 单元格像素尺寸（CSI 14/16 t）与配置色（OSC 10/11/12/4 查询与 104/110–112 恢复）
    pushCellPixelSize();
    if (cfg_->colors) itcore_set_colors(core_, &*cfg_->colors);
    renderer_->set_expected_generation(resize_.generation());

    GridSize gs{resize_.dims().cols, resize_.dims().rows};
    // 传输线程回调只捕获共享状态的 weak_ptr（传输本身挂在共享状态上，避免环），不捕获 this。
    // 投递的任务以 Win32PaneBackend& 为参数，只在 UI 线程 drain(this) 时执行；窗格销毁后队列已 close，投递被丢弃。
    std::weak_ptr<SessionShared> ws = shared_;
    using P = Win32PaneBackend;
    auto onState = [ws](SessionId, AttemptId, TransportState s, std::optional<TransportFault> f) {
        if (auto sh = ws.lock()) postTo<P>(*sh->queue, [s, f](P &p) { p.onTransportState(s, f); });
    };
    auto onOut = [ws](SessionId, AttemptId) {
        auto sh = ws.lock();
        if (sh && !sh->pumpPending.exchange(true))
            postTo<P>(*sh->queue, [](P &p) { p.shared_->pumpPending = false; p.pump(); });
    };
    auto onDiag = [](SessionId, AttemptId, const DiagRecord &d) {
        std::fprintf(stderr, "[pane] diag %s %llums %s\n", d.stage.c_str(), (unsigned long long)d.elapsedMs, d.detail.c_str());
    };
    if (connId_.empty()) {
        TransportCallbacks cb;
        cb.onStateChanged = onState;
        cb.onOutputAvailable = onOut;
        cb.onExited = [ws](SessionId, AttemptId, uint32_t code) {
            if (auto sh = ws.lock()) postTo<P>(*sh->queue, [code](P &p) { p.localExit_ = code; });
        };
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
        shared_->transport = transport_;
        if (!raw->start(SessionId{1}, AttemptId{attempt_}, spec)) { fail("本地 shell 启动失败"); return false; }
    } else if (std::string herr; cfg_->resolveHyperV && [&] {
                   auto h = cfg_->resolveHyperV(connId_, &herr);
                   if (!h) return !herr.empty();   // 有错误文本：是 Hyper-V 连接但配置无效
                   auto id = normalizeVmId(h->policy.vmId);
                   if (!id) { herr = "Hyper-V VM ID 格式无效"; return true; }
                   h->policy.vmId = *id;
                   h->ssh.initialSize = gs;
                   HyperVCallbacks cb;
                   cb.onStateChanged = onState;
                   cb.onOutputAvailable = onOut;
                   cb.onFinished = [ws](SessionId, AttemptId, const SshExitInfo &x) {
                       if (auto sh = ws.lock()) postTo<P>(*sh->queue, [x](P &p) { p.sshExit_ = x; });
                   };
                   cb.onDiagnostic = onDiag;
                   cb.onResolved = [ws](SessionId, AttemptId, const HvAction &a) {
                       if (auto sh = ws.lock()) postTo<P>(*sh->queue, [a](P &p) {
                           p.hvResult_ = a;
                           if (a.kind == HvAction::Connect) {
                               std::lock_guard l(p.statusMu_);
                               p.status_.title = (a.fallback ? "手动地址 " : "Hyper-V ") + a.host;
                           }
                           if (a.kind == HvAction::Connect) p.publishStatus();
                       });
                   };
                   auto ht = std::make_unique<HyperVT>();
                   ht->t = std::make_unique<HyperVTransport>(cb);
                   auto *raw = ht->t.get();
                   transport_ = std::move(ht);
                   shared_->transport = transport_;
                   raw->start(SessionId{1}, AttemptId{attempt_}, std::move(*h));
                   herr.clear();
                   return true;
               }()) {
        if (!herr.empty()) { fail(herr); return false; }
    } else {
        std::string err;
        std::optional<SshLaunchOptions> o = cfg_->resolveSsh ? cfg_->resolveSsh(connId_, &err) : std::nullopt;
        if (!o) { fail(err.empty() ? "找不到连接配置" : err); return false; }
        o->initialSize = gs;
        SshCallbacks cb;
        cb.onStateChanged = onState;
        cb.onOutputAvailable = onOut;
        cb.onFinished = [ws](SessionId, AttemptId, const SshExitInfo &x) {
            if (auto sh = ws.lock()) postTo<P>(*sh->queue, [x](P &p) { p.sshExit_ = x; });
        };
        cb.onDiagnostic = onDiag;
        auto st = std::make_unique<SshT>();
        st->t = std::make_unique<SshTransport>(cb);
        auto *raw = st->t.get();
        transport_ = std::move(st);
        shared_->transport = transport_;
        if (auto se = raw->start(SessionId{1}, AttemptId{attempt_}, *o)) {
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
        } else if (failed && hvResult_ && hvResult_->kind == HvAction::Fail) {
            status_.state = uishell::SessionState::Failed;
            status_.errorText = std::string(hvFailureName(hvResult_->failure)) + ": " + hvResult_->text;
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
        syncViewport();        // 已上翻时偏移随历史增长，视图停在原处
        present();
    }
}

void Win32PaneBackend::present() {
    if (!renderer_ || !core_) return;
    // ?2026：同步输出期间不取快照，超过 ITREN_SYNC_TIMEOUT_MS 兜底出一帧；关闭（含 DECSTR/RIS）后立即出帧
    if (!sync_.allow(itcore_mode_query(core_, 2026) == ITCORE_MODE_SET, nowMs())) return;
    releaseSnap();
    snap_ = itcore_acquire_snapshot(core_, vp_.firstRow(), resize_.dims().rows);  // 视口偏移：负 first_row 显示历史
    if (!snap_) return;
    renderer_->set_expected_generation(resize_.generation());
    renderer_->set_snapshot(snap_);
    renderer_->set_focused(focused_);
    renderer_->set_clock(nowMs());
    renderer_->present_frame();  // 设备丢失在 present_frame 内 recover_device
}

void Win32PaneBackend::pushCellPixelSize() {
    if (!core_) return;
    auto clamp16 = [](int v) { return uint16_t(v < 0 ? 0 : v > 65535 ? 65535 : v); };
    itcore_set_cell_pixel_size(core_, clamp16(metrics_.cell_w_px()), clamp16(metrics_.cell_h_px()));
}

void Win32PaneBackend::applyBounds(const uishell::PixelRect &rc, float dpi) {
    HWND h = hwnd_;
    if (!h) return;
    SetWindowPos(h, nullptr, rc.x, rc.y, rc.w, rc.h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!renderer_) return;
    float scale = dpi > 0 ? dpi : host_->dpi_scale();
    if (scale != metrics_.dpi_scale) metrics_ = itren::GridMetrics::from_font(7.2f, 10.f, 3.f, 0.f, scale);
    renderer_->metrics() = metrics_;
    pushCellPixelSize();
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
    reportFocus();
    present();  // 失焦：块光标画空心框、停止闪烁
}

void Win32PaneBackend::reportFocus() {
    // ?1004：焦点变化时向程序发 CSI I / CSI O（不回到底部，不经 LNM）
    const bool on = core_ && itcore_mode_query(core_, 1004) == ITCORE_MODE_SET;
    auto bytes = focusRep_.update(on, focused_);
    if (bytes.empty() || !transport_ || closing_ || finished_) return;
    transport_->input(bytes);
}

void Win32PaneBackend::sendInput(std::string_view utf8) {
    if (!transport_ || closing_ || finished_ || utf8.empty()) return;
    scrollToBottom();  // 用户一有输入就回到底部
    if (renderer_) renderer_->note_input(nowMs());  // 输入后光标常亮一个周期
    const bool lnm = core_ && itcore_ansi_mode_query(core_, 20) == ITCORE_MODE_SET;
    const std::string bytes = applyNewlineMode(utf8, lnm);  // LNM：UI 翻译的 Enter 补 LF
    auto r = transport_->input(bytes);
    if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] enqueueUserInput %d\n", int(r));
}

void Win32PaneBackend::paste(std::string_view utf8) {
    if (!transport_ || !core_ || closing_ || finished_ || utf8.empty()) return;
    scrollToBottom();
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

// 特殊键：决策在 planKey（Linux 单测），这里只取 ToUnicode / MapVirtualKey 的结果。
void Win32PaneBackend::sendKey(unsigned vk, unsigned mods) {
    if (!transport_ || !core_ || closing_ || finished_) return;
    uint32_t translated = 0, base = 0;
    if (vk != VK_PROCESSKEY) {
        BYTE ks[256] = {};
        if (GetKeyboardState(ks)) {
            wchar_t buf[4] = {};
            // 第 4 位标志：不改变内核键盘状态（Win10 1607+），避免吞掉死键
            int n = ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), ks, buf, 4, 0x4);
            if (n == 1) translated = buf[0];
            else if (n == 2 && IS_HIGH_SURROGATE(buf[0]) && IS_LOW_SURROGATE(buf[1]))
                translated = 0x10000u + ((uint32_t(buf[0]) - 0xD800u) << 10) + (uint32_t(buf[1]) - 0xDC00u);
            // 不带 Ctrl/Alt 的字符（Alt+字母的 ESC 前缀、Ctrl+字母的控制字符都基于它）
            ks[VK_CONTROL] = ks[VK_LCONTROL] = ks[VK_RCONTROL] = 0;
            ks[VK_MENU] = ks[VK_LMENU] = ks[VK_RMENU] = 0;
            if (!(mods & uishell::PaneBackend::ModShift)) ks[VK_SHIFT] = ks[VK_LSHIFT] = ks[VK_RSHIFT] = 0;
            else ks[VK_SHIFT] = 0x80;
            wchar_t b2[4] = {};
            if (ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), ks, b2, 4, 0x4) == 1) base = b2[0];
        }
    }
    KeyPlan plan = planKey(vk, mods, translated, base);
    if (plan.kind == KeyPlan::Ignore) return;
    if (plan.kind == KeyPlan::Text) { sendInput(plan.text); return; }
    itcore_key_event ke{};
    ke.struct_size = sizeof(ke);
    ke.vk = plan.vk;
    ke.modifiers = plan.coreMods;
    ke.unicode = plan.unicode;
    itcore_buffer *b = nullptr;
    if (itcore_encode_key(core_, &ke, &b) == 0 && b) {
        std::string bytes(reinterpret_cast<const char *>(b->data), b->length);
        itcore_buffer_release(b);
        if (!bytes.empty()) {
            scrollToBottom();
            auto r = transport_->input(bytes);  // 已编码，不再做 LNM 处理（核心按 LNM 编码 Enter）
            if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] sendKey enqueue %d\n", int(r));
        }
    } else if (b) {
        itcore_buffer_release(b);
    }
}

// 12.3 / 15.4：归档旧 attempt 的逻辑文本（只一个、≤8 MiB、不留 core/传输/链接），旧传输交给它自己的
// 关闭线程（旧队列先 close，旧回调的投递一律丢弃），再用新的共享状态启动新 core + 进程，attempt +1。
void Win32PaneBackend::reconnect() {
    HWND h = hwnd_;
    if (!h || !renderer_) return;
    if (core_) {
        itcore_buffer *b = nullptr;
        if (itcore_export_text(core_, kPrevAttemptTextBytes, &b) == 0 && b) {
            prevAttemptText_.assign(reinterpret_cast<const char *>(b->data), b->length);
            itcore_buffer_release(b);
        } else {
            prevAttemptText_.clear();
        }
        prevAttemptId_ = attempt_;
    }
    queue_->close();
    if (transport_ && !closer_.started()) startCloser(CloseReason::User);
    transport_.reset();
    if (!closer_.started()) shared_->transport.reset();
    releaseSnap();
    if (core_) { itcore_destroy(core_); core_ = nullptr; }

    shared_ = std::make_shared<SessionShared>();
    queue_ = shared_->queue;
    closer_ = SessionCloser{};
    closeOutcome_.reset(); sshExit_.reset(); hvResult_.reset(); localExit_.reset();
    closing_ = finished_ = false;
    selActive_ = dragSelStarted_ = false;
    drag_.cancel();
    dragEdge_ = 0;
    vp_.toBottom();
    ++attempt_;
    { std::lock_guard l(statusMu_); status_ = {uishell::SessionState::Starting, 0, false, {}, {}}; }
    queue_->setWake([h] { PostMessageW(h, WM_PANE_WAKE, 0, 0); });
    publishStatus();
    if (startSession()) present();
    syncViewport(true);
    syncSelection();
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

// 关闭线程只拿 shared_ptr<SessionShared>：结果写进共享状态，UI drain 时 drainUi() 领取并调 onCloseDone。
// 窗格先销毁则无人领取；传输在本线程等到真正结束后析构（超时仍不结束时由 Job KILL_ON_JOB_CLOSE 兜底）。
void Win32PaneBackend::startCloser(CloseReason why) {
    if (!shared_->transport) return;
    closer_.start(shared_,
        [why](SessionShared &sh) {
            CloseOutcome r;
            auto t = std::static_pointer_cast<Transport>(sh.transport);
            t->close(why);  // T03A §1.4 九步由传输层 closer 线程执行；这里只等它在预算内结束
            if (!t->waitClosed(kCloserBudgetMs)) { r.ok = false; r.timedOut = true; r.detail = "关闭超时，已强制结束"; }
            return r;
        },
        [](SessionShared &sh) {
            if (auto t = std::static_pointer_cast<Transport>(sh.transport)) t->waitClosed(INFINITE);
            sh.transport.reset();
        });
}

void Win32PaneBackend::drainUi() {
    queue_->drain(this);
    if (auto r = shared_->takeCloseResult()) onCloseDone(*r);
}

void Win32PaneBackend::syncViewport(bool force) {
    if (!core_) return;
    bool changed = vp_.updateHistory(int(std::min<uint32_t>(itcore_history_rows(core_), INT_MAX)));
    if ((changed || force || !vpNotified_) && ev_.onViewportChanged) {
        vpNotified_ = true;
        ev_.onViewportChanged(vp_.offset(), vp_.history());
    }
}

void Win32PaneBackend::scrollViewport(int lines) {
    if (!core_ || !lines) return;
    vp_.updateHistory(int(std::min<uint32_t>(itcore_history_rows(core_), INT_MAX)));
    if (!vp_.scroll(lines)) return;
    present();
    syncViewport(true);
}

void Win32PaneBackend::scrollToBottom() {
    if (!core_ || !vp_.toBottom()) return;
    present();
    syncViewport(true);
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
        me.action = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? ITCORE_MOUSE_PRESS : msg == WM_LBUTTONUP ? ITCORE_MOUSE_RELEASE : ITCORE_MOUSE_MOVE;
        if (wp & MK_CONTROL) me.modifiers |= ITCORE_MOD_CTRL;
        itcore_buffer *b = nullptr;
        if (itcore_encode_mouse(core_, &me, &b) == 0 && b) {
            sendInput(std::string_view(reinterpret_cast<const char *>(b->data), b->length));
            itcore_buffer_release(b);
        }
        return;
    }
    // 本地：拖动选择（核心存 (逻辑行, 字符) 端点，跟随滚动/重排），单击清选区并检查链接；
    // 双击选词、三击选逻辑行，之后拖动按同一单位扩展；拖出上下边缘由 tick 自动滚动
    if (!core_) return;
    const int32_t first = vp_.firstRow();
    DragSelect::Cell cell{c, r};
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        downCol_ = c; downRow_ = r; drag_.down(cell); dragSelStarted_ = false; dragEdge_ = 0;
        int n = clicks_.press(c, r, GetTickCount64(), GetDoubleClickTime());
        clickUnit_ = n >= 2 ? n - 1 : 0;
        if (clickUnit_) {
            itcore_selection_start_unit(core_, first, c, r, clickUnit_);
            dragSelStarted_ = true;
            selectionChanged();
        }
        return;
    }
    if (msg == WM_MOUSEMOVE && drag_.pressed()) {
        // selection_hit：宽字符尾格吸附到首格；越出网格上/下边缘时按距离给出自动滚动速度
        auto sh = itren::selection_hit(metrics_, x, y, cols, rows, snap_);
        dragEdge_ = sh.autoscroll_dir * std::max(1, sh.autoscroll_lines);
        dragCol_ = sh.col;
        if (!sh.autoscroll_dir) { c = sh.col; r = sh.row; cell = DragSelect::Cell{c, r}; }
    }
    auto startSel = [&] {
        auto p = drag_.pressCell();
        itcore_selection_start(core_, first, uint16_t(p.col), uint16_t(p.row));
        dragSelStarted_ = true;
    };
    DragSelect::Act act = msg == WM_LBUTTONUP ? drag_.up(cell) : drag_.move(cell);
    if (clickUnit_ && act != DragSelect::Act::None) {
        if (act != DragSelect::Act::Click && itcore_selection_extend(core_, first, c, r)) selectionChanged();
        act = DragSelect::Act::None;
    }
    switch (act) {
    case DragSelect::Act::Start:
        startSel();
        itcore_selection_extend(core_, first, c, r);
        selectionChanged();
        break;
    case DragSelect::Act::Extend:
        if (itcore_selection_extend(core_, first, c, r)) selectionChanged();
        break;
    case DragSelect::Act::Finish:
        if (!dragSelStarted_) startSel();
        itcore_selection_extend(core_, first, c, r);
        selectionChanged();
        break;
    case DragSelect::Act::Click:
        if (itcore_selection_active(core_)) { itcore_selection_clear(core_); selectionChanged(); }
        handleClick(x, y);
        break;
    case DragSelect::Act::None: break;
    }
    if (msg == WM_LBUTTONUP) { downCol_ = downRow_ = -1; dragEdge_ = 0; }
}

// 拖选越过上/下边缘：每个 tick 滚 1 行并把选区端点延到边缘行
void Win32PaneBackend::autoScrollTick() {
    if (!dragEdge_ || !drag_.pressed() || !core_ || !dragSelStarted_) return;
    uint16_t rows = resize_.dims().rows;
    scrollViewport(dragEdge_);  // |dragEdge_| = 每节拍行数（selection_hit，封顶 10）
    uint16_t row = dragEdge_ < 0 ? 0 : uint16_t(rows - 1);
    if (itcore_selection_extend(core_, vp_.firstRow(), uint16_t(dragCol_), row)) selectionChanged();
}

// 滚轮：鼠标模式转发给程序（Shift 强制本地）；备用屏无鼠标模式发上/下方向键；否则滚动视口。每格 3 行。
void Win32PaneBackend::handleWheel(WPARAM wp, LPARAM lp) {
    if (!core_) return;
    wheelAccum_ += GET_WHEEL_DELTA_WPARAM(wp);
    int notches = wheelAccum_ / WHEEL_DELTA;
    if (!notches) return;
    wheelAccum_ -= notches * WHEEL_DELTA;
    UINT keys = GET_KEYSTATE_WPARAM(wp);
    bool mouseMode = snap_ && itcore_snapshot_mouse_mode(snap_) != 0 && !(keys & MK_SHIFT);
    switch (routeWheel(mouseMode, altScreen())) {
    case WheelRoute::Forward: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        uint16_t c = 0, r = 0;
        metrics_.hit_test(pt.x, pt.y, itcore_snapshot_columns(snap_), itcore_snapshot_rows(snap_), c, r);
        for (int i = 0; i < std::abs(notches); ++i) {
            itcore_mouse_event me{};
            me.struct_size = sizeof(me);
            me.col = c; me.row = r;
            me.button = notches > 0 ? ITCORE_MBUTTON_WHEEL_UP : ITCORE_MBUTTON_WHEEL_DOWN;
            me.action = ITCORE_MOUSE_WHEEL;
            if (keys & MK_CONTROL) me.modifiers |= ITCORE_MOD_CTRL;
            itcore_buffer *b = nullptr;
            if (itcore_encode_mouse(core_, &me, &b) == 0 && b) {
                sendInput(std::string_view(reinterpret_cast<const char *>(b->data), b->length));
                itcore_buffer_release(b);
            }
        }
        break;
    }
    case WheelRoute::Arrows:
        for (int i = 0; i < std::abs(notches) * 3; ++i) {
            itcore_key_event ke{};
            ke.struct_size = sizeof(ke);
            ke.vk = notches > 0 ? ITCORE_VK_UP : ITCORE_VK_DOWN;
            itcore_buffer *b = nullptr;
            if (itcore_encode_key(core_, &ke, &b) == 0 && b) {
                sendInput(std::string_view(reinterpret_cast<const char *>(b->data), b->length));
                itcore_buffer_release(b);
            }
        }
        break;
    case WheelRoute::Scroll:
        scrollViewport(-notches * 3);  // 向上滚（delta>0）= 往历史里翻
        break;
    }
}

LRESULT Win32PaneBackend::onMessage(UINT m, WPARAM w, LPARAM l) {
    HWND h = hwnd_;
    switch (m) {
    case WM_PANE_WAKE: drainUi(); return 0;
    case WM_TIMER:
        if (w == kTickTimer) { drainUi(); pump(); flushTitle(); autoScrollTick(); if (sync_.holding() || (renderer_ && renderer_->wants_animation())) present(); }
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
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetCapture(h); handleMouse(m, w, l); return 0;
    case WM_MOUSEWHEEL: handleWheel(w, l); return 0;
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
                // 子句属性（ATTR_INPUT/TARGET_CONVERTED/CONVERTED…）按 UTF-16 单元给出，折算到 cluster（跳过低代理）
                LONG na = ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
                std::vector<uint8_t> raw(na > 0 ? size_t(na) : 0);
                if (na > 0) ImmGetCompositionStringW(imc, GCS_COMPATTR, raw.data(), na);
                LONG cur16 = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
                size_t idx = 0;
                pe.cursor = SIZE_MAX;
                for (size_t i = 0; i < ws.size(); ++i) {
                    if (ws[i] >= 0xDC00 && ws[i] <= 0xDFFF) continue;
                    if (LONG(i) == cur16) pe.cursor = idx;
                    uint8_t a = i < raw.size() ? raw[i] : 0;
                    pe.attrs.push_back(a == ATTR_TARGET_CONVERTED || a == ATTR_TARGET_NOTCONVERTED ? 1 : a == ATTR_CONVERTED ? 2 : 0);
                    ++idx;
                }
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
        reportFocus();
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
    case WM_KILLFOCUS: focused_ = false; reportFocus(); present(); return 0;
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
    if (!closer_.started()) shared_->transport.reset();
    releaseSnap();
    if (renderer_) { renderer_->shutdown(); renderer_.reset(); }
    if (HWND h = hwnd_.exchange(nullptr)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    host_.reset();
    if (core_) { itcore_destroy(core_); core_ = nullptr; }
}

bool shutdownPaneBackends(unsigned timeoutMs) {
    return SessionCloser::shutdownAll(std::chrono::milliseconds(timeoutMs));
}

uishell::PaneBackendFactory makeWin32PaneBackendFactory(PaneBackendConfig cfg) {
    auto shared = std::make_shared<const PaneBackendConfig>(std::move(cfg));
    return [shared](const std::string &connectionId, uishell::PaneEvents ev) -> std::unique_ptr<uishell::PaneBackend> {
        return std::make_unique<Win32PaneBackend>(connectionId, std::move(ev), shared);
    };
}

}  // namespace it2app
