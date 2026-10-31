// T08 PaneBackend（Win32）。线程模型：
// - 传输回调在传输内部线程 → 只往 UiDispatchQueue post，wake = PostMessage(子窗口, WM_PANE_WAKE)。
// - 子窗口消息在 UI 线程 drain 队列：喂核心、处理核心事件、present、回调 PaneEvents。
// - 因此 PaneEvents 只会在 UI 线程（创建子窗口的线程）上被调用。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include "win32_pane_backend.h"
#include "ssh_transport.h"
#include "pane_logic.h"
#include "hwnd_swap_host.h"
#include "renderer.h"
#include "frame_policy.h"
#include "grid_metrics.h"
#include "selection_hit.h"
#include "frame_pump.h"
#include "pane_search.h"
#include "itcore.h"
#include <algorithm>
#include <climits>
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

using namespace it2w;

namespace it2app {
namespace {

constexpr UINT WM_PANE_WAKE = WM_APP + 0x41;
constexpr UINT WM_PANE_EVENTS = WM_APP + 0x42; // pump 返回后才允许发送 PaneEvents
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kPumpContTimer = 2;  // 一次性：pump 续跑（FedMore 或 Fed+missed）
constexpr UINT_PTR kSearchTimer = 3;    // 搜索分批计数 / PENDING 落定：busy 期间每 kSearchMs 续跑一次
constexpr UINT kSearchMs = 10;
constexpr UINT kPumpContMs = 8;         // PERF-02：续跑间隔（原 1ms 太密，会话树/对话框 WM_PAINT 轮不到）
constexpr uint32_t kSearchBudgetCells = 256 * 1024, kSearchBudgetUs = 4000;  // 每次 step：UI 线程最多约 4ms
constexpr UINT kTickMs = 50;
constexpr wchar_t kPaneClass[] = L"it2w_pane_backend";

uint64_t nowMs() { return GetTickCount64(); }
// pump 预算计时：steady_clock（Windows 上即 QueryPerformanceCounter），GetTickCount64 只有 10–16ms 精度，卡不住 8ms
uint64_t budgetNowMs(void *) {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// frame_pump 诊断（核心拒收、收尾跳片）→ 调试器输出
void pumpLog(void *, const char *msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

SshTransportFactory makeSshFactory() {
    return [](SshCallbacks cb) -> std::unique_ptr<SshSessionTransport> {
        return std::make_unique<SshTransport>(std::move(cb));
    };
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
    virtual uint64_t discardedBytes() const = 0;
    // CAP-01 P06：经 ConPTY 的传输不支持 OSC 52 读；直连传输将来可覆盖为 true。
    virtual bool supportsOsc52Read() const { return false; }
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
    uint64_t discardedBytes() const override { return t->discardedBytes(); }
    bool waitClosed(DWORD ms) override { return t->waitClosed(ms); }
    uint32_t processes() override { return t->activeProcessCount(); }
    bool isSsh() const override { return false; }
};
struct SshT final : Transport {
    std::unique_ptr<SshSessionTransport> t;
    EnqueueResult input(std::string_view b) override { return t->enqueueUserInput(b); }
    EnqueueResult reply(std::string_view b) override { return t->enqueueReply(b); }
    EnqueueResult beginPaste(size_t n, bool br) override { return t->beginPaste(n, br); }
    EnqueueResult appendPaste(std::string_view b) override { return t->appendPaste(b); }
    void resize(GridSize g, uint64_t gen) override { t->resize(g, gen); }
    void close(CloseReason r) override { t->close(r); }
    std::optional<OutputChunk> pop() override { return t->tryPopOutput(); }
    TransportState state() const override { return t->state(); }
    uint64_t discardedBytes() const override { return t->discardedBytes(); }
    bool waitClosed(DWORD ms) override { return t->waitClosed(ms); }
    uint32_t processes() override { return t->activeProcessCount(); }
    bool isSsh() const override { return true; }
    bool supportsOsc52Read() const override { return t->supportsOsc52Read(); }
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
    uint64_t discardedBytes() const override { return t->discardedBytes(); }
    bool waitClosed(DWORD ms) override { return t->waitClosed(ms); }
    uint32_t processes() override { return t->activeProcessCount(); }
    bool isSsh() const override { return true; }
    bool supportsOsc52Read() const override { return t->supportsOsc52Read(); }
};

class Win32PaneBackend final : public uishell::PaneBackend {
public:
    Win32PaneBackend(std::string connectionId, uishell::PaneEvents ev, std::shared_ptr<const PaneBackendConfig> cfg,
                     std::optional<std::wstring> localCommand = std::nullopt)
        : connId_(std::move(connectionId)), ev_(std::move(ev)), cfg_(std::move(cfg)), localCommand_(std::move(localCommand)) {
        status_ = {uishell::SessionState::Starting, 0, false, {}, {}};
    }
    // 析构：先断开 PaneEvents，再 teardown（close 队列 → 之后的投递一律丢弃）
    ~Win32PaneBackend() override { teardown(); }

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
    void setFont(std::string_view family, float sizePt) override;
    uishell::CellSize cellSize() const override;
    uishell::SearchResult findInScrollback(std::string_view needleUtf8, const uishell::SearchOptions &opt, bool backward) override;
    void clearSearch() override;

private:
    void applyFont(FontSpec f);         // UI 线程：重建字体 → GridMetrics → 像素尺寸 → 网格 → generation
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    bool startSession();
    void fail(std::string text);
    void pushCellPixelSize();          // GridMetrics → itcore_set_cell_pixel_size
    void pump(); // 单轮预算；绝不派发消息/调用 PaneEvents
    friend class it2app::PumpDriver;
    bool armPumpTimer() { HWND h = hwnd_; return h && SetTimer(h, kPumpContTimer, kPumpContMs, nullptr); }
    void cancelPumpTimer() { if (HWND h = hwnd_) KillTimer(h, kPumpContTimer); }
    std::optional<OutputChunk> popOutput() { return transport_ ? transport_->pop() : std::nullopt; }
    bool pumpInputEnded() const {
        if (!transport_) return true;
        auto state = transport_->state();
        return state == TransportState::Closed || state == TransportState::Failed;
    }
    void afterFeed() { drainCoreEvents(); }
    void finishPumpInput(itcore_core *core) { itcore_finish_input(core); }
    void presentPump(FrameMode mode);
    void closePumpFault() { if (transport_) transport_->close(CloseReason::Fault); }
    void completePump(uint64_t dropped);
    template<class Fn, class... Args> void deferEvent(Fn fn, Args... args) {
        if (!fn) return;
        pendingEvents_.add([fn = std::move(fn), args...] { fn(args...); });
        if (!eventsPosted_) if (HWND h = hwnd_)
            eventsPosted_ = PostMessageW(h, WM_PANE_EVENTS, 0, 0) != 0;
    }
    void flushEvents(); // 取局部副本；之后不得再访问 this
    void drainCoreEvents();
    void setClipboardUtf8(std::string_view u8);
    std::string getClipboardUtf8();
    void present(bool force = false, itren::PresentMode mode = itren::PresentMode::VSync);  // force：绕过 SyncGate；mode：续跑用 Immediate
    void syncSearchHighlight();        // 把 PaneSearch::ranges() 建进 D2DRenderer 高亮层（无活动搜索则 clear）
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
    void endDragOnCaptureLoss();
    void searchTick();                  // UI 线程：按预算 step，状态变化 → onSearchUpdated
    void armSearchTimer();
    void revealSearchRow(int32_t viewRow);  // 当前结果不在视口内时滚动，使其位于视口中部
    void endSearch(bool notifyRestarted);   // itcore_destroy 之前调用（句柄归核心所有）
    bool altScreen() const { return core_ && itcore_mode_query(core_, 1049) == ITCORE_MODE_SET; }
    static bool inputPendingThunk(void *) { return HIWORD(GetQueueStatus(QS_INPUT | QS_PAINT)) != 0; }

    std::string connId_;
    uishell::PaneEvents ev_;
    std::shared_ptr<const PaneBackendConfig> cfg_;
    PumpDriver pumpDriver_;
    TerminalStatus terminal_;
    std::shared_ptr<char> life_ = std::make_shared<char>(); // 每次 reconnect 换令牌
    PendingEvents pendingEvents_;
    bool eventsPosted_ = false;
    // 仅诊断用：默认关闭，会向远端 stdin 注入 PING/伪应答。只认环境变量值 1。
    const bool osc52Ping_ = envFlagOn(std::getenv("ITERM2WIN_OSC52_PING"));
    std::optional<std::wstring> localCommand_;  // --command，仅第一个本地窗格；重连（新 attempt）同样带上
    // 与关闭线程 / 传输回调共享的唯一状态（不含 this、不含 PaneEvents）
    std::shared_ptr<SessionShared> shared_ = std::make_shared<SessionShared>();
    std::shared_ptr<UiDispatchQueue> queue_ = shared_->queue;
    std::atomic<HWND> hwnd_{nullptr};
    HWND parent_ = nullptr;
    std::unique_ptr<iterm_app::HwndSwapChainHost> host_;
    std::unique_ptr<itren::D2DRenderer> renderer_;
    itren::GridMetrics metrics_{};
    FontSpec font_;                     // 当前字体（attach 时取 cfg_，setFont 改）
    uishell::PixelRect bounds_{0, 0, 0, 0};
    bool sized_ = false;                // 已知格尺寸（attach 成功后）
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
    PaneSearch search_;                 // 滚动历史搜索（草案 itcore_search v0.2）；高亮层待渲染接入 ranges()
    bool vpNotified_ = false;
    ClickCounter clicks_;
    int clickUnit_ = 0;                 // 0 字符 / 1 词 / 2 行（双击 / 三击后拖动按单位扩展）
    it2app::ActivationClickGate activateGate_;
    int dragEdge_ = 0;                  // 拖选到上(-1)/下(+1)边缘外：tick 自动滚动
    bool dragEmpty_ = false;            // 最近一次移动的 drag_end 为空选区
    uint16_t pressBoundary_ = 0;        // 按下点的列边界（selection_hit.boundary），drag_end 判方向/空选区
    int dragCol_ = 0;                   // 自动滚动时端点列（按 boundary 与方向换算，右半算进选区）
    int wheelAccum_ = 0;
    mutable std::mutex statusMu_;
    uishell::SessionStatus status_;
    std::optional<SshExitInfo> sshExit_;
    std::optional<HvAction> hvResult_;   // T09 解析结果（Fail 时作为错误文本）
    std::optional<uint32_t> localExit_;
    bool focused_ = false, closing_ = false, finished_ = false, skipChar_ = false;
    bool pendingStart_ = false;             // M17：网格仍是 20×5 下限时推迟 startSession（ConPTY 过小会丢启动输出）
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
    font_ = {cfg_->fontFamily, cfg_->fontSizePt};
    bounds_ = rc;
    renderer_ = std::make_unique<itren::D2DRenderer>(host_.get());
    metrics_ = itren::GridMetrics::from_font(itren::estimate_font_metrics(font_.sizePt), scale);
    const bool initOk = renderer_->init(metrics_, font_.family.c_str(), font_.sizePt);
    if (!initOk)
        std::fprintf(stderr, "[pane] D2DRenderer::init 失败（设备丢失时 present 内重试）\n");
    // 格尺寸取 DirectWrite 实测主字体度量（init 失败时为按字号估算）
    // init 之后一定实测；实测失败（无设备/字体族找不到）才退回估算并记日志
    const auto fm = renderer_->font_metrics();
    if (!fm.measured)
        std::fprintf(stderr, "[pane] DirectWrite 字体度量失败（%ls %.1fpt，renderer init=%s），格尺寸按字号估算\n", font_.family.c_str(), double(font_.sizePt), initOk ? "ok" : "failed");
    metrics_ = itren::GridMetrics::from_font(fm, scale);
    renderer_->metrics() = metrics_;
    sized_ = true;
    uint16_t c = 80, r = 24;
    metrics_.grid_size(rc.w, rc.h, c, r);
    resize_ = ResizeTracker({c, r});
    // 传输线程的 post 从这里开始能唤醒 UI 线程；attach 之前排队的任务立即补唤醒
    // wake 在队列锁内调用、close 时清空，只捕获窗口句柄（不捕获 this）
    queue_->setWake([h] { PostMessageW(h, WM_PANE_WAKE, 0, 0); });
    SetTimer(h, kTickTimer, kTickMs, nullptr);
    // M17：ConPTY 在过小网格（钳制后仍 20×5）上创建会丢启动输出；先建窗口/渲染器，等首次真正变大再 startSession。
    // 推迟期间保持 Starting（构造时已是），不 Failed。
    if (gridAtSessionFloor(resize_.dims())) {
        pendingStart_ = true;
        publishStatus();
        return true;
    }
    pendingStart_ = false;
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
    applyOsc52(core_, cfg_->osc52);
    if (applyWordChars(core_, cfg_->wordChars) != 0) std::fprintf(stderr, "[pane] wordChars 非法，使用默认\n");
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
        if (sh && sh->sched.readerShouldPost())
            postTo<P>(*sh->queue, [](P &p) { if (p.shared_->sched.postedShouldRun()) p.pump(); });
    };
    auto onDiag = [](SessionId, AttemptId, const DiagRecord &d) {
        std::fprintf(stderr, "[pane] diag %s %llums %s\n", d.stage.c_str(), (unsigned long long)d.elapsedMs, d.detail.c_str());
    };
    std::function<void(SessionId, AttemptId, std::string_view)> outputTap;
    if (osc52Ping_) {
        auto sniffer = std::make_shared<Osc52OutputSniffer>();
        outputTap = [sniffer](SessionId, AttemptId, std::string_view bytes) {
            for (const auto &hex : sniffer->observe(bytes)) pumpLog(nullptr, ("[pane] osc52 out hex=" + hex).c_str());
        };
    }
    if (connId_.empty()) {
        TransportCallbacks cb;
        cb.onStateChanged = onState;
        cb.onOutputAvailable = onOut;
        cb.onExited = [ws](SessionId, AttemptId, uint32_t code) {
            if (auto sh = ws.lock()) postTo<P>(*sh->queue, [code](P &p) { p.localExit_ = code; });
        };
        cb.onDiagnostic = onDiag;
        cb.onOutputTap = outputTap;
        auto lt = std::make_unique<LocalT>();
        lt->t = std::make_unique<ConPtyTransport>(cb);
        LaunchSpec spec;
        spec.executable = cfg_->localShell;
        if (spec.executable.empty()) {
            wchar_t buf[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
            spec.executable = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\Windows\\System32\\cmd.exe";
        }
        if (localCommand_) {
            auto plan = planLocalShellCommand(spec.executable, *localCommand_);
            if (!plan) { fail("--command 含换行，已拒绝"); return false; }
            spec.arguments = std::move(plan->args);
            spec.rawArgumentTail = std::move(plan->rawTail);
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
                   cb.onOutputTap = outputTap;
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
                   ht->t = std::make_unique<HyperVTransport>(cb, makeSshFactory());
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
        cb.onOutputTap = outputTap;
        auto st = std::make_unique<SshT>();
        st->t = makeSshFactory()(cb);
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
    pumpDriver_.stop(*this, shared_->sched);
    pendingStart_ = false;
    publishStatus();
}

void Win32PaneBackend::publishStatus() {
    deferEvent(ev_.onStatus, status());
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

// 传输已终止后只进入 Draining；所有 feed/finish_input 由后续低优先级定时器推进。
void Win32PaneBackend::finishSession() {
    if (pumpDriver_.phase != DrainPhase::Running) return;
    finished_ = true;
    pendingStart_ = false;
    pumpDriver_.requestFinish(*this, shared_->sched);
}

void Win32PaneBackend::completePump(uint64_t dropped) {
    TerminalInputs in;
    in.transportFailed = transport_ && transport_->state() == TransportState::Failed;
    in.closing = closing_;
    in.timedOut = closeOutcome_ && closeOutcome_->timedOut;
    if (closeOutcome_) in.timeoutDetail = closeOutcome_->detail;
    in.sshFailed = sshExit_ && sshExit_->error != SshError::None && sshExit_->error != SshError::RemoteExit;
    if (in.sshFailed) in.sshDetail = std::string(sshErrorName(sshExit_->error)) + (sshExit_->detail.empty() ? "" : ": " + sshExit_->detail);
    if (hvResult_ && hvResult_->kind == HvAction::Fail)
        in.transportDetail = std::string(hvFailureName(hvResult_->failure)) + ": " + hvResult_->text;
    in.exitCode = sshExit_ ? int(sshExit_->exitCode) : int(localExit_.value_or(0));
    in.coreDropped = dropped;
    in.queueDropped = transport_ ? transport_->discardedBytes() : 0;
    terminal_ = classifyTerminal(in);
    {
        std::lock_guard l(statusMu_);
        status_.state = terminal_.failed ? uishell::SessionState::Failed : uishell::SessionState::Exited;
        status_.errorText = terminal_.error;
        status_.exitCode = terminal_.exitCode;
        status_.hasForegroundJob = false;
    }
    pumpLog(nullptr, terminalLog(attempt_, terminal_, dropped, in.queueDropped).c_str());
    publishStatus(); // phase 已是 Finished；只排队，回调不出现在 pump 栈中。
}

void Win32PaneBackend::flushEvents() {
    eventsPosted_ = false;
    auto life = std::weak_ptr<char>(life_);
    auto events = pendingEvents_.take();
    PendingEvents::flush(std::move(life), std::move(events)); // 尾调用：可能销毁或重连本对象
}

void Win32PaneBackend::releaseSnap() {
    if (snap_) {
        if (renderer_) renderer_->set_snapshot(nullptr);
        itcore_snapshot_release(snap_);
        snap_ = nullptr;
    }
}

// B4：OSC 52 剪贴板读写（UI 线程；失败静默，不阻塞输出）
void Win32PaneBackend::setClipboardUtf8(std::string_view u8) {
    int n = u8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), nullptr, 0);
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t(n) + 1) * sizeof(wchar_t))) {
        if (auto *w = static_cast<wchar_t *>(GlobalLock(g))) {
            if (n) MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), w, n);
            w[n] = 0;
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else GlobalFree(g);
    }
    CloseClipboard();
}

std::string Win32PaneBackend::getClipboardUtf8() {
    std::string out;
    if (!OpenClipboard(hwnd_)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (auto *w = static_cast<const wchar_t *>(GlobalLock(h))) { out = toUtf8(w); GlobalUnlock(h); }
    CloseClipboard();
    return out;
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
            if (bell_.allow(nowMs())) deferEvent(ev_.onBell);
            break;
        case ITCORE_EV_TITLE:
            if (auto t = title_.offer(std::string(reinterpret_cast<const char *>(p), p ? n : 0), nowMs())) {
                { std::lock_guard l(statusMu_); status_.title = *t; }
                publishStatus();
            }
            break;
        case ITCORE_EV_CLIPBOARD_WRITE:
            setClipboardUtf8(std::string_view(reinterpret_cast<const char *>(p), p ? n : 0));
            break;
        case ITCORE_EV_CLIPBOARD_REQUEST:
            // 核心 REQUEST 也包括写拒绝/非法选择器；事件本身无法区分读写。
            pumpLog(nullptr, "[pane] osc52 read denied");
            break;
        case ITCORE_EV_CLIPBOARD_READ:
            pumpLog(nullptr, "[pane] osc52 read event");
            if (!transport_) {
                pumpLog(nullptr, "[pane] osc52 read skipped (no transport)");
            } else if (!osc52ReadEffective(cfg_->osc52.allowRead, transport_->supportsOsc52Read())) {
                // CAP-01 P06：策略关，或传输（当前均为 ConPTY）不支持读 → 不交付应答。
                if (!cfg_->osc52.allowRead)
                    pumpLog(nullptr, "[pane] osc52 read denied");
                else
                    pumpLog(nullptr, "[pane] osc52 read unsupported (transport)");
            } else {
                std::string cur = getClipboardUtf8();
                auto replyFn = [](void *p, const char *data, size_t n) -> int {
                    return int(static_cast<Transport *>(p)->reply(std::string_view(data, n)));
                };
                if (osc52Ping_) osc52DiagPingSequence(replyFn, transport_.get(), pumpLog, nullptr);
                deliverOsc52ReadReply(core_, e, cur.data(), cur.size(), replyFn, transport_.get(), pumpLog, nullptr, true); // OSC52-05：成功日志常开
            }
            break;
        case ITCORE_EV_SEARCH_INVALIDATED:  // resize / 主备屏切换 / ED3 / RIS：下一次 step 得到 RESTARTED
            if (renderer_) renderer_->clear_search_highlight();  // 先清层，避免失效结果继续黄/橙
            armSearchTimer();
            break; // 当前 pump 的出帧统一由 PumpDriver 选择模式
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
    PumpBudget budget = pumpDriver_.phase == DrainPhase::Draining ? PumpBudget::finalSlice() : PumpBudget{};
    budget.nowMs = budgetNowMs;
    budget.log = pumpLog;
    budget.yieldUi = inputPendingThunk; // 只查 GetQueueStatus；绝不 Peek/Dispatch。
    pumpDriver_.step(*this, core_, shared_->sched, budget);
}

void Win32PaneBackend::presentPump(FrameMode mode) {
    syncSelection();
    syncViewport();
    present(mode == FrameMode::Force, mode == FrameMode::Immediate ? itren::PresentMode::Immediate : itren::PresentMode::VSync);
}

void Win32PaneBackend::syncSearchHighlight() {
    if (!renderer_) return;
    if (!core_ || !search_.active()) { renderer_->clear_search_highlight(); return; }
    const uint16_t rows = resize_.dims().rows, cols = resize_.dims().cols;
    const auto rr = search_.ranges(vp_.firstRow(), rows);
    std::vector<itren::SearchRange> v;
    v.reserve(rr.size());
    for (const auto &r : rr)  // itcore_search_range.is_current 是 uint8_t，SearchRange 用 bool
        v.push_back({r.row, r.start_col, r.end_col, r.is_current != 0});
    // PaneSearch::ranges 入参是视口行，内部换成绝对行；返回的 row 也是绝对行号
    const int64_t first_abs =
        int64_t(itcore_scrollback_overflow(core_)) + int64_t(itcore_history_rows(core_)) + vp_.firstRow();
    itren::SearchHighlight hl;
    hl.build(v, first_abs, rows, cols);
    renderer_->set_search_highlight(std::move(hl));
}

void Win32PaneBackend::present(bool force, itren::PresentMode mode) {
    if (!renderer_ || !core_) return;
    // ?2026：同步输出期间不取快照，超过 ITREN_SYNC_TIMEOUT_MS 兜底出一帧；关闭（含 DECSTR/RIS）后立即出帧
    // 同一批内“关→内容→开”由 itcore_sync_end_count 变化放行；超时只是每 150ms 出一帧，不清同步状态
    if (!it2app::frame_due(sync_, core_, nowMs(), force)) return;
    releaseSnap();
    snap_ = itcore_acquire_snapshot(core_, vp_.firstRow(), resize_.dims().rows);  // 视口偏移：负 first_row 显示历史
    if (!snap_) return;
    renderer_->set_expected_generation(resize_.generation());
    renderer_->set_snapshot(snap_);
    renderer_->set_focused(focused_);
    renderer_->set_clock(nowMs());
    syncSearchHighlight();  // 每帧按当前视口重建高亮；无活动搜索 → clear（hit=None，画面与接入前一致）
    // force（改尺寸）→ Force 绕过节流；否则按 mode（续跑 Immediate / 空闲 VSync）
    renderer_->present_frame(force ? itren::PresentMode::Force : mode);
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
    bounds_ = rc;
    if (!renderer_) return;
    float scale = dpi > 0 ? dpi : host_->dpi_scale();
    if (scale != metrics_.dpi_scale) metrics_ = itren::GridMetrics::from_font(renderer_->font_metrics(), scale);
    renderer_->metrics() = metrics_;
    pushCellPixelSize();
    uint16_t c = 0, r = 0;
    metrics_.grid_size(rc.w, rc.h, c, r);  // 已钳到 ≥20×5；ResizeTracker 再兜底
    if (auto gen = resize_.update(c, r)) {
        // T03A：先 itcore_resize(gen)，再 transport.resize(gen)；渲染按 expected generation 保持旧布局直到快照对齐
        if (core_) itcore_resize(core_, resize_.dims().cols, resize_.dims().rows, *gen);
        if (transport_ && !closing_ && opAllowed(pumpDriver_.phase, PaneOp::ResizeTransport)) transport_->resize(GridSize{resize_.dims().cols, resize_.dims().rows}, *gen);
        renderer_->set_expected_generation(*gen);
        releaseSnap();
    }
    // M17：推迟的 startSession —— 网格严格大于下限后才启 ConPTY/SSH（避免 20×5 丢 --command 启动输出）
    if (pendingStart_ && !closing_ && !finished_ && gridReadyForSessionStart(resize_.dims())) {
        pendingStart_ = false;
        if (!startSession()) { /* fail() 已置 Failed */ }
    }
    renderer_->resize_surface();
    present(true);  // 改尺寸绕过 SyncGate，强制出一帧
}

void Win32PaneBackend::setBounds(const uishell::PixelRect &rc, float dpi) { applyBounds(rc, dpi); }

// 异步：只投递到 UI 队列，下一次 drain 时 applyFont（不在调用栈里重建 DWrite / 改网格）
void Win32PaneBackend::setFont(std::string_view family, float sizePt) {
    FontSpec next;
    if (!resolveFontChange(font_, family, sizePt, &next)) return;
    queue_->postOwner([next](void *o) { if (o) static_cast<Win32PaneBackend *>(o)->applyFont(next); });
}

void Win32PaneBackend::applyFont(FontSpec f) {
    if (!renderer_ || !hwnd_) return;
    if (f.family == font_.family && f.sizePt == font_.sizePt) return;
    if (!renderer_->set_font(f.family.c_str(), f.sizePt)) {
        std::fprintf(stderr, "[pane] setFont 失败，保持当前字体\n");
        return;
    }
    font_ = f;
    metrics_ = itren::GridMetrics::from_font(renderer_->font_metrics(), metrics_.dpi_scale);
    // 同 applyBounds 顺序：metrics → itcore_set_cell_pixel_size → grid → generation → itcore_resize → transport.resize
    applyBounds(bounds_, metrics_.dpi_scale);
    publishStatus();  // 网格变化经 onStatus 通知 UI（UI 随后读 cellSize()）
}

uishell::CellSize Win32PaneBackend::cellSize() const {
    if (!sized_) return {0, 0};
    return {metrics_.cell_w_px(), metrics_.cell_h_px()};
}

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
    if (bytes.empty() || !transport_ || closing_ || finished_ || !opAllowed(pumpDriver_.phase, PaneOp::Input)) return;
    transport_->input(bytes);
}

void Win32PaneBackend::sendInput(std::string_view utf8) {
    if (!transport_ || closing_ || finished_ || !opAllowed(pumpDriver_.phase, PaneOp::Input) || utf8.empty()) return;
    scrollToBottom();  // 用户一有输入就回到底部
    if (renderer_) renderer_->note_input(nowMs());  // 输入后光标常亮一个周期
    const bool lnm = core_ && itcore_ansi_mode_query(core_, 20) == ITCORE_MODE_SET;
    const std::string bytes = applyNewlineMode(utf8, lnm);  // LNM：UI 翻译的 Enter 补 LF
    auto r = transport_->input(bytes);
    if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] enqueueUserInput %d\n", int(r));
}

void Win32PaneBackend::paste(std::string_view utf8) {
    if (!transport_ || !core_ || closing_ || finished_ || !opAllowed(pumpDriver_.phase, PaneOp::Input) || utf8.empty()) return;
    scrollToBottom();
    auto rejected = [&] { deferEvent(ev_.onPasteRejected, utf8.size(), kPasteLimitBytes); };
    if (utf8.size() > kPasteLimitBytes) { rejected(); return; }
    // 唯一来源：核心 encode_paste（实时 ?2004、剥离标记、换行归一、上限）
    itcore_buffer *buf = nullptr;
    int er = itcore_encode_paste(core_, utf8.data(), utf8.size(), &buf);
    if (er == -2) { rejected(); return; }
    if (er != 0 || !buf) { if (buf) itcore_buffer_release(buf); return; }
    std::string bytes(reinterpret_cast<const char *>(buf->data), buf->length);
    itcore_buffer_release(buf);
    if (bytes.empty()) return;
    // 走传输的粘贴事务（与用户输入不交错）；标记已在 bytes 里，事务不再补
    auto r = transport_->beginPaste(bytes.size(), false);
    if (r == EnqueueResult::Ok) r = transport_->appendPaste(bytes);
    if (r == EnqueueResult::TooLarge) rejected();
    else if (r != EnqueueResult::Ok) std::fprintf(stderr, "[pane] paste %d (%zu bytes)\n", int(r), bytes.size());
}

// 特殊键：决策在 planKey（Linux 单测），这里只取 ToUnicode / MapVirtualKey 的结果。
void Win32PaneBackend::sendKey(unsigned vk, unsigned mods) {
    if (!transport_ || !core_ || closing_ || finished_ || !opAllowed(pumpDriver_.phase, PaneOp::Input)) return;
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
    life_.reset(); // 使正在 flush 的旧 attempt 局部批次失效
    life_ = std::make_shared<char>();
    pendingEvents_.clear();
    pumpDriver_.reset(*this, shared_->sched);
    terminal_ = {};
    queue_->close();
    if (transport_ && !closer_.started()) startCloser(CloseReason::User);
    transport_.reset();
    if (!closer_.started()) shared_->transport.reset();
    releaseSnap();
    endSearch(true);  // 新 attempt 是新核心：旧搜索失效，UI 用同一 needle 重搜
    if (core_) { itcore_destroy(core_); core_ = nullptr; }

    shared_ = std::make_shared<SessionShared>();
    queue_ = shared_->queue;
    closer_ = SessionCloser{};
    closeOutcome_.reset(); sshExit_.reset(); hvResult_.reset(); localExit_.reset();
    closing_ = finished_ = false;
    KillTimer(h, kPumpContTimer);   // 旧 attempt 的续跑定时器；新的 shared_ 自带全新的 PumpScheduler
    KillTimer(h, kSearchTimer);
    selActive_ = dragSelStarted_ = false;
    drag_.cancel();
    dragEdge_ = 0;
    vp_.toBottom();
    ++attempt_;
    { std::lock_guard l(statusMu_); status_ = {uishell::SessionState::Starting, 0, false, {}, {}}; }
    queue_->setWake([h] { PostMessageW(h, WM_PANE_WAKE, 0, 0); });
    publishStatus();
    // 重连时窗口通常已有真实尺寸；若仍卡在下限则继续推迟
    if (gridAtSessionFloor(resize_.dims())) {
        pendingStart_ = true;
    } else {
        pendingStart_ = false;
        if (startSession()) present();
    }
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
        deferEvent(ev_.onViewportChanged, vp_.offset(), vp_.history());
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

namespace {
uishell::SearchResult toUiSearch(const PaneSearch::State &s) {
    uishell::SearchResult r;
    r.found = s.found;
    r.index = s.index;
    r.total = s.total;
    r.hasNewOutput = s.hasNewOutput;
    r.restarted = s.restarted;
    r.pending = s.pending;
    return r;
}
}  // namespace

uishell::SearchResult Win32PaneBackend::findInScrollback(std::string_view needleUtf8, const uishell::SearchOptions &opt, bool backward) {
    if (!core_) return {};
    PaneSearch::Options o;
    o.caseMode = opt.caseMode == uishell::SearchCase::Sensitive ? uint8_t(ITCORE_SEARCH_CASE_SENSITIVE)
               : opt.caseMode == uishell::SearchCase::Insensitive ? uint8_t(ITCORE_SEARCH_CASE_INSENSITIVE)
                                                                  : uint8_t(ITCORE_SEARCH_CASE_SMART);
    o.regex = opt.regex;  // v0.1：true → found=false（PaneSearch 不退回子串搜索）
    o.wrap = opt.wrapAround;
    const PaneSearch::State s = search_.find(core_, needleUtf8, o, backward);
    if (s.found && s.hasView) revealSearchRow(s.viewRow);  // 含「已在视口」：reveal 内 present（SRCH-HL-03）
    else present();  // 未找到 / 无视口坐标：清或刷新高亮
    armSearchTimer();
    return toUiSearch(s);
}

void Win32PaneBackend::clearSearch() {
    search_.end();
    if (HWND h = hwnd_) KillTimer(h, kSearchTimer);
    present();  // syncSearchHighlight → clear；视口不动
}

void Win32PaneBackend::endSearch(bool notifyRestarted) {
    if (HWND h = hwnd_) KillTimer(h, kSearchTimer);
    if (!search_.active()) return;
    search_.end();
    present();  // RESTARTED / reconnect：清高亮层
    if (notifyRestarted && ev_.onSearchUpdated) {
        uishell::SearchResult r;
        r.restarted = true;
        deferEvent(ev_.onSearchUpdated, r);
    }
}

void Win32PaneBackend::armSearchTimer() {
    if (!search_.busy() && !search_.active()) return;
    if (HWND h = hwnd_) SetTimer(h, kSearchTimer, kSearchMs, nullptr);
}

void Win32PaneBackend::searchTick() {
    if (!core_ || !search_.active()) {
        if (HWND h = hwnd_) KillTimer(h, kSearchTimer);
        return;
    }
    if (auto u = search_.tick(kSearchBudgetCells, kSearchBudgetUs)) {
        if (u->moved && u->hasView) revealSearchRow(u->viewRow);
        else present();  // 高亮刷新（含 tick 内 RESTARTED→end 后清层）；reveal 已 present
        deferEvent(ev_.onSearchUpdated, toUiSearch(*u));
    }
    // 计数完成、无挂起移动后停掉快速定时器；之后新输出 / 改写由 50ms tick 发现，失效由事件重新挂上
    if (!search_.busy()) if (HWND h = hwnd_) KillTimer(h, kSearchTimer);
}

void Win32PaneBackend::revealSearchRow(int32_t viewRow) {
    if (!core_) return;
    vp_.updateHistory(int(std::min<uint32_t>(itcore_history_rows(core_), INT_MAX)));
    const int rows = resize_.dims().rows;
    const int32_t first = vp_.firstRow();
    // SRCH-HL-03：命中已在视口内也必须 present。findInScrollback 在 found&&hasView 时只调
    // reveal、不走 else present；若此处早退，syncSearchHighlight 永不跑，单命中在屏内
    // （CJK/emoji/HIDDEN/SOFTWRAP 等）就会出现「1/1 但无黄/橙」——真机 M20/M24b。
    if (viewRow >= first && viewRow < first + rows) { present(); return; }
    const int64_t target = int64_t(viewRow) - rows / 2;
    const int64_t delta = std::clamp<int64_t>(target - first, INT_MIN, INT_MAX);
    if (!vp_.scroll(int(delta))) { present(); return; }  // 夹紧后无位移：仍刷高亮
    present();
    syncViewport(true);
}

void Win32PaneBackend::onCloseDone(const CloseOutcome &r) {
    closeOutcome_ = r;
    finishSession(); // 只请求 Draining；不在关闭结果回调栈里同步 feed/EOF
}

void Win32PaneBackend::requestClose() {
    if (closing_ || finished_ || !opAllowed(pumpDriver_.phase, PaneOp::Close)) return;
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
    deferEvent(ev_.onSelectionChanged, now);
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
    deferEvent(ev_.onLinkActivated, std::string(url, ul)); // 复制 URL，离开鼠标处理栈后才调用 UI
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
        dragEmpty_ = false;
        pressBoundary_ = itren::selection_hit(metrics_, x, y, cols, rows, snap_).boundary;
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
        // 端点按 boundary 换算（右半算进选区）；自动滚动时列仍按同一 x 的网格内命中（SEL-01）
        auto sh = itren::selection_hit_autoscroll(metrics_, x, y, cols, rows, snap_);
        auto de = itren::drag_end(sh, uint16_t(drag_.pressCell().row), pressBoundary_, cols);
        dragEdge_ = de.edge;
        dragCol_ = de.col;
        dragEmpty_ = de.empty && !clickUnit_;
        if (dragEmpty_) {
            // 终点回到按下点（或首行行首左边界）：没有选区
            if (dragSelStarted_ && itcore_selection_active(core_)) { itcore_selection_clear(core_); selectionChanged(); }
            dragSelStarted_ = false;
            return;
        }
        if (!sh.autoscroll_dir) { c = de.col; r = de.row; cell = DragSelect::Cell{c, r}; }
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
        if (!dragSelStarted_) startSel();  // 曾回到空选区（drag_end.empty）后重新拖出
        if (itcore_selection_extend(core_, first, c, r)) selectionChanged();
        break;
    case DragSelect::Act::Finish:
        if (dragEmpty_) break;             // 松开时终点与按下点重合：无选区
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

void Win32PaneBackend::endDragOnCaptureLoss() {
    drag_.captureLost();
    dragSelStarted_ = false;
    dragEdge_ = 0;  // 自动滚动没有独立定时器（共用 kTickTimer 驱动输出/呈现，不能 KillTimer）：
                    // autoScrollTick 以 dragEdge_ 与 drag_.pressed() 为门，这里清零即停止
    downCol_ = downRow_ = -1;
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
    case WM_PANE_EVENTS: flushEvents(); return 0; // 回调之后不得访问 this
    case WM_PANE_WAKE: drainUi(); return 0;
    case WM_TIMER:
        if (w == kPumpContTimer) {
            KillTimer(h, kPumpContTimer);  // 一次性；持有状态延续到 pump() 内部，期间读线程不投递
            pump();
            return 0;
        }
        if (w == kSearchTimer) { searchTick(); return 0; }
        if (w == kTickTimer) {
            drainUi();
            pump();
            flushTitle(); autoScrollTick();
            if (search_.active()) searchTick();
            if (sync_.holding() || (renderer_ && renderer_->wants_animation())) present();
        }
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
        // 顶层窗口原先未激活：这一下点击只激活，不开始选区（BUG-03）
        activateGate_.onMouseActivate(GetForegroundWindow() != GetAncestor(h, GA_ROOT));
        if (parent_) SendMessageW(parent_, m, w, l); // 转发可能销毁 this，后面只返回常量
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!activateGate_.allowDown()) return 0;
        SetCapture(h); handleMouse(m, w, l); return 0;
    case WM_CAPTURECHANGED: case WM_CANCELMODE:
        // 模态对话框等抢走捕获：结束拖动、停自动滚动，已有选区保留（BUG-03）
        if (m == WM_CANCELMODE && GetCapture() == h) ReleaseCapture();
        endDragOnCaptureLoss();
        return m == WM_CANCELMODE ? 0 : DefWindowProcW(h, m, w, l);
    case WM_MOUSEWHEEL: handleWheel(w, l); return 0;
    case WM_LBUTTONUP: if (!activateGate_.allowUp()) return 0;
        // 先按松开收尾再释放捕获：ReleaseCapture 会同步发 WM_CAPTURECHANGED，顺序反了会把正常结束当成取消
        handleMouse(m, w, l); ReleaseCapture(); return 0;
    case WM_MOUSEMOVE: if ((w & MK_LBUTTON) && activateGate_.allowMove()) handleMouse(m, w, l); return 0;
    // 转发必须尾调用：父窗口可能同步销毁/重连 backend，返回后不得访问 this。
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
            // 簇与列宽按核心当前屏幕（备用屏 VS16 不扩宽），与上屏后网格一致
            itren::fill_preedit_clusters(pe, core_);
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
        KillTimer(h, kPumpContTimer);
        KillTimer(h, kSearchTimer);
        hwnd_ = nullptr;  // 之后的 post 不再 PostMessage
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void Win32PaneBackend::teardown() {
    life_.reset();
    ev_ = {};
    pendingEvents_.clear();
    pumpDriver_.stop(*this, shared_->sched);
    // GWLP_USERDATA 清空后 WM_DESTROY 不再进入本对象，因此必须显式撤所有定时器。
    if (HWND h = hwnd_) {
        KillTimer(h, kTickTimer); KillTimer(h, kPumpContTimer); KillTimer(h, kSearchTimer);
    }
    queue_->close();
    if (transport_ && !closer_.started()) startCloser(CloseReason::User);
    transport_.reset();
    if (!closer_.started()) shared_->transport.reset();
    endSearch(false);
    releaseSnap();
    if (renderer_) { renderer_->shutdown(); renderer_.reset(); }
    if (HWND h = hwnd_.exchange(nullptr)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    host_.reset();
    if (core_) { itcore_destroy(core_); core_ = nullptr; } // 最后一次核心访问
}

bool shutdownPaneBackends(unsigned timeoutMs) {
    return SessionCloser::shutdownAll(std::chrono::milliseconds(timeoutMs));
}

uishell::PaneBackendFactory makeWin32PaneBackendFactory(PaneBackendConfig cfg) {
    auto shared = std::make_shared<const PaneBackendConfig>(std::move(cfg));
    auto commandTaken = std::make_shared<bool>(false);  // 工厂只在 UI 线程调用
    return [shared, commandTaken](const std::string &connectionId, uishell::PaneEvents ev) -> std::unique_ptr<uishell::PaneBackend> {
        std::optional<std::wstring> cmd;
        if (connectionId.empty() && shared->localCommand && !*commandTaken) { *commandTaken = true; cmd = shared->localCommand; }
        return std::make_unique<Win32PaneBackend>(connectionId, std::move(ev), shared, std::move(cmd));
    };
}

}  // namespace it2app
