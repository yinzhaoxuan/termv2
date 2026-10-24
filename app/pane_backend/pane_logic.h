// T08 PaneBackend 的平台无关部分（Linux 单测）：括号粘贴、响铃/标题限流、20×5 最小网格与
// resize generation、跨线程 → UI 线程的事件派发队列。Win32 实现见 win32_pane_backend.cpp。
#pragma once
#include "../common/it2app/limits.h"
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

typedef struct itcore_core itcore_core;

namespace it2app {

// B3：把配置的分词字符下发给核心。nullopt → 核心默认；非法（字母数字/空白/非 ASCII）返回 -1，核心保持原设置。
int applyWordChars(itcore_core *core, const std::optional<std::string> &wordChars);

// B4：OSC 52 配置下发（默认全关；读与写分别授权；maxBytes 0 用核心默认）。UI 线程调用。
struct Osc52Config { bool allowWrite = false; bool allowRead = false; uint32_t maxBytes = 0; };
int applyOsc52(itcore_core *core, const Osc52Config &cfg);

// --command：按本地 shell 的解析规则把一条命令拼进启动参数（只用于第一个本地窗格）。
//  cmd.exe          → args {/s, /k}，rawTail = "\"" + 命令 + "\""（/s 只剥最外层一对引号，命令内的引号、&、| 原样交给 cmd）
//  powershell/pwsh  → args {-NoExit, -Command, 命令}（按 CommandLineToArgvW 规则加引号）
//  其它 shell       → rawTail = 命令（原样追加，由该 shell 自行解析）
// 空命令 → 不加参数；含 CR/LF/NUL → nullopt（拒绝，窗格 Failed）。shell 名按文件名比较，不区分大小写。
struct LocalShellLaunch { std::vector<std::wstring> args; std::wstring rawTail; };
std::optional<LocalShellLaunch> planLocalShellCommand(const std::wstring &shellPath, const std::wstring &command);

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

// setFont 参数归一：空 family 保持当前；字号钳 6..72（UI 已钳，后端再兜底）；无变化返回 false
struct FontSpec { std::wstring family; float sizePt = 12.f; };
bool resolveFontChange(const FontSpec &cur, std::string_view familyUtf8, float sizePt, FontSpec *out);

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
// wake 在锁内调用：close() 返回后 wake 不会再运行，持有者可以放心销毁 wake 引用的对象。
// close() 后 post 被丢弃。drain 中回调再 post 的任务留到下一次 drain，避免饿死消息循环。
// OwnerTask：任务不捕获窗格指针，由 drain(owner) 在 UI 线程把当时还活着的窗格传进来。
class UiDispatchQueue {
public:
    using Task = std::function<void()>;
    using OwnerTask = std::function<void(void *owner)>;
    explicit UiDispatchQueue(std::function<void()> wake = {}) : wake_(std::move(wake)) {}
    void setWake(std::function<void()> wake);
    bool post(Task t);
    bool postOwner(OwnerTask t);
    std::size_t drain(void *owner = nullptr);
    void close();
    bool closed() const;
    std::size_t size() const;
private:
    mutable std::mutex mu_;
    std::deque<OwnerTask> q_;
    std::function<void()> wake_;
    bool closed_ = false;
};
// 类型安全的投递：f(Owner&) 只在 drain(&owner) 时执行
template <class Owner, class F>
bool postTo(UiDispatchQueue &q, F f) {
    return q.postOwner([f = std::move(f)](void *o) mutable { if (o) f(*static_cast<Owner *>(o)); });
}

// ---- 非阻塞关闭（T08 R1/R2）----
struct CloseOutcome {
    bool ok = true;         // 正常结束（Exited）
    bool timedOut = false;  // 超出预算被强杀
    std::string detail;
};
// PUMP-01：谁负责下一次 pump。UI 线程在 pump 期间以及续跑定时器待发期间“持有”pump，此时读线程不投递，
// 免得投递的 pump 插到 WM_PAINT / 输入前面。读线程先记 missed 再看 owned：pump 结束释放时若 missed 被置位，
// 由 UI 线程自己补投一次，不丢唤醒（seq_cst：读线程看到 owned 为真 ⇒ missed 的写先于释放）。
class PumpScheduler {
public:
    // 读线程：有新输出时是否投递 pump（同一时刻至多一条投递）
    bool readerShouldPost() {
        missed_.store(true);
        if (owned_.load()) return false;
        return !pending_.exchange(true);
    }
    // 投递的 pump 任务开始执行：清掉 pending；UI 线程正持有（续跑定时器待发）时返回 false，交给定时器
    bool postedShouldRun() { pending_.store(false); return !owned_.load(); }
    void begin() { owned_.store(true); missed_.store(false); }
    enum class Next { None, Timer, Post };
    // pump 结束：还有积压（more）→ 继续持有，调用方设续跑定时器；否则释放，期间有新数据则 Post（调用方自己投递）
    Next end(bool more) { return more ? Next::Timer : release(); }
    Next timerFailed() { return release(); }   // SetTimer 失败：退回投递
    Next release() {
        owned_.store(false);
        if (missed_.exchange(false) && !pending_.exchange(true)) return Next::Post;
        return Next::None;
    }
    void reset() { owned_.store(false); missed_.store(false); pending_.store(false); }
    bool owned() const { return owned_.load(); }
private:
    std::atomic<bool> owned_{false}, missed_{false}, pending_{false};
};

// 会话共享状态：窗格与关闭线程 / 传输回调之间唯一共享的东西。不含窗格指针、不含 PaneEvents。
struct SessionShared {
    std::shared_ptr<UiDispatchQueue> queue = std::make_shared<UiDispatchQueue>();
    PumpScheduler sched;
    std::shared_ptr<void> transport;  // 传输层对象；最后一个持有者（通常是关闭线程）负责析构
    void setCloseResult(CloseOutcome r);
    std::optional<CloseOutcome> takeCloseResult();
private:
    std::mutex mu_;
    std::optional<CloseOutcome> result_;
};
// 每个会话自己的关闭线程：job 在该线程上跑完 T03A 九步关闭与超时强杀（可阻塞），
// 结果写进 SessionShared 并 post 一个空任务唤醒 UI；UI drain 后 takeCloseResult 取结果。
// 线程只持有 shared_ptr<SessionShared>；窗格销毁（队列 close）后结果无人领取，随共享状态释放。
// 线程 detach；全局登记活跃数，程序退出时 shutdownAll 等待其结束。
class SessionCloser {
public:
    using Job = std::function<CloseOutcome(SessionShared &)>;
    using After = std::function<void(SessionShared &)>;
    // 只有第一次调用生效；返回是否本次启动。after（可空）在结果写入之后、仍在关闭线程上执行，
    // 用于超时上报后继续等待传输层真正结束并在该线程上析构。
    bool start(std::shared_ptr<SessionShared> st, Job job, After after = {});
    bool started() const { return started_; }
    // 等待所有关闭线程结束（程序退出时在 UI 线程调用）。超时返回 false：剩余线程保持 detach，
    // 子进程由 Job 对象 KILL_ON_JOB_CLOSE 随进程退出回收；登记表故意泄漏，不参与静态析构。
    static bool shutdownAll(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    static int liveThreads();
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
    // 失去鼠标捕获（WM_CAPTURECHANGED / WM_CANCELMODE，例如模态对话框弹出）：按松开收尾但不产生动作，
    // 已拖出的选区保留；之后的 move/up 都返回 None（BUG-03）
    void captureLost() { cancel(); }
    bool pressed() const { return pressed_; }
    bool dragging() const { return dragging_; }
    Cell pressCell() const { return press_; }
private:
    bool pressed_ = false, dragging_ = false;
    Cell press_;
};

// 激活点击：窗口未激活时的那一下点击只用于激活，不开始选区/不算单击（BUG-03）。
// WM_MOUSEACTIVATE 时 armed(窗口原先未激活)；随后的按下与配对的松开都被吞掉。
class ActivationClickGate {
public:
    void onMouseActivate(bool wasInactive) { armed_ = wasInactive; }
    // 非激活点击的按下清掉 swallowing_：被吞的按下若在窗格外松开（收不到 WM_LBUTTONUP），不能吃掉下一次正常松开
    bool allowDown() { if (armed_) { armed_ = false; swallowing_ = true; return false; } swallowing_ = false; return true; }
    bool allowUp() { if (swallowing_) { swallowing_ = false; return false; } return true; }
    bool allowMove() const { return !swallowing_; }
    void reset() { armed_ = swallowing_ = false; }
private:
    bool armed_ = false, swallowing_ = false;
};

// 多击计数：同一格、间隔不超过系统双击时间的连续按下累加 1→2→3→1（3 = 三击选行）
class ClickCounter {
public:
    int press(int col, int row, uint64_t nowMs, uint32_t intervalMs);
    void reset() { count_ = 0; }
private:
    int count_ = 0, col_ = -1, row_ = -1;
    uint64_t last_ = 0;
};

// 视口：offset = 离底部的行数（0 = 跟随输出）；first_row = -offset。history = 历史显示行数。
// sendKey 的平台无关决策。uiMods 为 PaneBackend::KeyMods（Shift1 Alt2 Ctrl4）；translated 为 ToUnicode
// 在当前修饰键下翻译出的码点（0 无），base 为不带 Ctrl/Alt 时该键的字符（MapVirtualKey，已按 Shift 定大小写，0 无）。
// - VK_PROCESSKEY（0xE5）：Ignore，交给输入法。
// - Ctrl+Alt（AltGr 在 Windows 上报成 Ctrl+Alt）且 translated 为可打印字符：Text，按文本发。
// - 其余：Key，交 itcore_encode_key（coreMods 为 ITCORE_MOD_*：Shift1 Ctrl2 Alt4；带 Ctrl/Alt 的字符键附 base，
//   核心据此出 Ctrl 控制字符或 Alt 的 ESC 前缀）。
struct KeyPlan {
    enum Kind { Ignore, Text, Key } kind = Ignore;
    std::string text;
    unsigned vk = 0, coreMods = 0;
    uint32_t unicode = 0;
};
KeyPlan planKey(unsigned vk, unsigned uiMods, uint32_t translated, uint32_t base);
std::string utf8Encode(uint32_t cp);
// kPrevAttemptTextBytes 见 it2app/limits.h

class Viewport {
public:
    int offset() const { return offset_; }
    int history() const { return history_; }
    int32_t firstRow() const { return -offset_; }
    bool scroll(int lines);              // 负数 = 往历史里翻；超出范围截断。返回是否变化
    bool toBottom() { return set(0); }
    // 历史行数变化：已上翻时把 offset 同步增加，保持看着同一段内容；历史被裁剪/清空时截断
    bool updateHistory(int history);
private:
    bool set(int off);
    int offset_ = 0, history_ = 0;
};

// 滚轮路由：鼠标模式 → 转发给程序；备用屏无鼠标模式 → 上下方向键；否则滚动视口
enum class WheelRoute { Forward, Arrows, Scroll };
inline WheelRoute routeWheel(bool mouseMode, bool altScreen) {
    if (mouseMode) return WheelRoute::Forward;
    return altScreen ? WheelRoute::Arrows : WheelRoute::Scroll;
}

// LNM（ANSI 20）：UI 已把 Enter 翻成 "\r"（或 Alt+Enter "\x1b\r"）后经 sendInput 送来；
// LNM 置位时补成 CR LF。只匹配整条输入恰为 Enter 的情况，避免改写其它按键序列 / 文本里的 CR。
std::string applyNewlineMode(std::string_view in, bool lnm);

// ?2026 同步输出期间暂停取快照：打开后最多 timeoutMs（ITREN_SYNC_TIMEOUT_MS）出一帧兜底；
// 关闭（含 DECSTR/RIS 清掉）的那一刻立即允许出帧。
class SyncGate {
public:
    explicit SyncGate(uint64_t timeoutMs) : timeout_(timeoutMs) {}
    // 返回 true 表示现在可以取快照并 present。
    // syncEnds = itcore_sync_end_count：与上次不同说明期间有同步结束（同一批“关→内容→开”），放行并清零计时。
    // 超时只是每 timeout 出一帧，不改变核心的同步状态。
    bool allow(bool syncOn, uint64_t nowMs, uint64_t syncEnds = 0) {
        const bool ended = syncEnds != ends_;
        ends_ = syncEnds;
        if (!syncOn) { since_.reset(); return true; }
        if (ended) { since_ = nowMs; return true; }
        if (!since_) { since_ = nowMs; return false; }
        if (nowMs - *since_ >= timeout_) { since_ = nowMs; return true; }  // 兜底帧，并重新计时
        return false;
    }
    bool holding() const { return since_.has_value(); }
private:
    uint64_t timeout_;
    uint64_t ends_ = 0;
    std::optional<uint64_t> since_;
};

// ?1004 焦点事件：模式置位且焦点状态变化时返回 CSI I / CSI O；否则空
class FocusReporter {
public:
    std::string update(bool modeSet, bool focused) {
        if (last_ && *last_ == focused) return {};
        last_ = focused;
        if (!modeSet) return {};
        return focused ? "\x1b[I" : "\x1b[O";
    }
private:
    std::optional<bool> last_;
};

}  // namespace it2app
