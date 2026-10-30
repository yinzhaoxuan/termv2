# ADR-001 pump/收尾状态机

- 状态：已采纳并实现（2026-10-31）
- 实现：Codex（gpt-6.1-sol xhigh）协助；设计经 Claude（opus high）评审，架构师定稿
- 日期：2026-10-31
- 基线：main `c1d475d`（代码同 `d4f0744`）
- 关联：NEW-01、NEW-05、DS-1、GAP-02、GAP-08、E1、PERF-01/02/03、OSC52-04/05、DS-11
- 改动范围：
  - `app/pane_backend/` 下的 `win32_pane_backend.cpp`、`frame_pump.h`、`pane_logic.h/.cpp`
  - `platform/windows/conpty/conpty_transport.cpp`（只加诊断）
  - `ui/include/uishell/pane_backend.h`（只加一个带默认值的字段）
  - `ui/shell_win32/main.cpp` 不改

---

## 1. 背景

### 1.1 现状调用链

| 环节 | 位置 | 行为 |
|---|---|---|
| 读线程唤醒 | `win32_pane_backend.cpp:346-350` `onOut` | `readerShouldPost` → `postTo` → `pump()` |
| 续跑和周期 | `:1269-1283` `onMessage(WM_TIMER)` | `kPumpContTimer` / `kTickTimer` 触发 `if (!pumping_) pump()`；tick 先调 `drainUi()`，**这一步不受守卫**（`:1277`） |
| pump | `:613-664` `pump(bool drainAll)` | `PumpReentrancyGuard`（`:615`）→ `pump_core` / `pump_core_final`，`yieldUi = yieldUiThunk` |
| 片间让出 | `:588-611` `yieldUiMessages` | `PeekMessageW(nullptr, PM_REMOVE)` 加 `DispatchMessageW`，派发**本线程所有窗口**的消息，只跳过本窗格的 WAKE 和三个定时器 |
| 预算核心 | `frame_pump.h:62-115` `pump_core` | 片间调用 `budget.yieldUi`（`:113`）；final 路径跳片时只写日志（`:98-107`） |
| 收尾 | `win32:465-497` `finishSession` → `frame_pump.h:135-142` `finish_session_input` | **同步执行**：`pump(true)` → `pump_core_final`（片间再 yield，`:127`）→ KillTimer → `sched.reset` → `itcore_finish_input` |
| UI 关闭 | `main.cpp:551-559` `closeActivePane` → `:527-530` `confirm`（MessageBoxW）→ `:493-503` `destroyPane` | `:497` 先 `requestClose()` 再 `backend.reset()` |

### 1.2 问题

**(a) 释放后使用（NEW-01）**

1. 洪峰中 `yieldUiMessages` 派发了 `WM_KEYDOWN(Ctrl+Shift+W)`。
2. UI 的 `confirm` 弹框，用户确认后进入 `destroyPane`。
3. `~Win32PaneBackend`（`:132`）→ `teardown`（`:1423-1439`）→ `itcore_destroy(core_)`。
4. 栈退回到 `pump_core` 后继续 `afterFeed()` 和 `itcore_feed(core, …)`，访问的是已释放对象。

换成 `reconnect()`（`:855-902`）也一样：外层 pump 手里的 `core`，以及 `sched`（属于旧 `shared_`）都已失效。

**(b) GAP-02 复发（NEW-01）**

1. 外层 pump 在 yield 时进入任意一个 MessageBox 的嵌套循环。
2. 嵌套循环派发本窗格的 `kTickTimer` → `drainUi()` → `onTransportState(Closed)` → `finishSession`。
3. 内层 `pump(true)` 被守卫挡回，什么都没喂；随后 `itcore_finish_input`，发布 Exited。
4. MessageBox 关闭后，外层 pump 继续 `itcore_feed`，核心返回 FATAL，触发 `transport_->close(Fault)`。结果是尾部丢失、误报故障。

**根因**：PERF-02 为了让 MessageBox 尽快弹出，在 pump 栈帧里做了通用消息派发。PERF-03 的守卫只阻止「再次进入 pump」，挡不住两件事：在 pump 栈帧下销毁或重置对象，以及在 pump 栈帧下收尾。

**NEW-05**：final 路径跳过的字节只写进 `OutputDebugStringA`（`pumpLog`，`:52-55`）；`OutputQueue::discarded_`（`io_queues.cpp:113,117`）也没有上报。用户看不到「输出不完整」。

---

## 2. 决策

保留 UI 线程模型，但**禁止在 pump 栈帧内派发消息**。收尾改成异步三态 `Running → Draining → Finished`，由一次性续跑定时器分多轮推进。会话执行器（E1）不在本 ADR 范围内，见 §8 备选 A2。

### 2.1 去掉嵌套派发和 PERF-03 守卫

需要删除：

- `Win32PaneBackend::yieldUiMessages`（`:588-611`）、`yieldUiThunk`（`:583-586`）及其声明（`:191-195`）。
- 成员 `finishingPump_`（`:246`）和 `pumping_`（`:247`）。
- `PumpReentrancyGuard`（`pane_logic.h:174-190`），以及 `onMessage` 里的两处 `if (!pumping_)`（`:1272`、`:1278`）。
- `pump_core_final`（`frame_pump.h:120-129`）和 `finish_session_input`（`:135-142`），由 §2.2 的 `pump_step` / `complete_drain` 取代。

新的 yield 只查询，不取消息：

```cpp
// win32_pane_backend.cpp，替代 yieldUiThunk
static bool inputPendingThunk(void *) {
    return HIWORD(GetQueueStatus(QS_INPUT | QS_PAINT)) != 0;   // 不 Peek、不 Dispatch
}
```

`PumpBudget::yieldUi` 的契约（`frame_pump.h:35-38`）改为：

- 只能查询；返回 true 表示提前结束本轮并返回 FedMore。
- 实现里不得 Peek/Dispatch，不得弹框，不得调用任何回调。

`pump_core` 片间的检查点（`:113`）保持原样。这样 yield 就等于「提前返回 FedMore」，后续由一次性的 `kPumpContTimer` 续跑。WM_TIMER 只在队列里没有其他消息时才生成，所以输入和 WM_PAINT 自然会先被处理。禁止用 Post 续跑的规定（`pane_logic.h:136-139`）不变。

可选：调试版保留一个 `inPump_` 计数，发现重入时 `OutputDebugString` 并 `__debugbreak()`。它只是断言，不再起守卫作用。

### 2.2 收尾状态机

在 `pane_logic.h` 新增：

```cpp
enum class DrainPhase : uint8_t { Running, Draining, Finished };
struct PumpStats { uint64_t droppedBytes = 0; uint32_t skippedSlices = 0; };
```

| 迁移 | 触发 | 动作 |
|---|---|---|
| Running → Draining | `finishSession()`，来源有三个：`onTransportState(Closed/Failed)`（`:452-462`）、`onCloseDone`（`:1054`）、没有 transport 时的 `requestClose`（`:1063`） | 设 `phase_=Draining` 和 `finished_=true`（挡住用户输入），`pendingStart_=false`。**不同步喂数据**，只调用 `SetTimer(kPumpContTimer)`；SetTimer 失败时由 `kTickTimer` 推进 |
| Draining → Draining | 每次 `kPumpContTimer` / `kTickTimer` / WAKE 调用 `pumpStep()` | 用 `PumpBudget::finalSlice()`（final=true，64 KiB / 8 ms，遇 NEED_DRAIN 重试 `finalRetries` 次）喂一轮；结果是 FedMore 就再挂续跑定时器 |
| Draining → Finished | 两种情况之一：某一轮 `pump_core` 返回 Fed/Idle（说明 `pop()` 已返回空，`frame_pump.h:80`）**并且** `carry_.empty()`；或者 `inputFinished_` 已因 Fatal 置位 | 执行 `completeDrain()`，顺序是：KillTimer → `sched.reset()` → `itcore_finish_input` → `inputFinished_=true` → `drainCoreEvents` → `present(VSync)` → 计算终态（含丢弃字节数）→ `phase_=Finished` → **最后**才 `publishStatus()` |
| Draining/Finished → Running | `reconnect()` | 放弃剩余排空（旧 attempt 已归档），按现有 `:855-902` 重建；`phase_=Running`，`stats_={}` |
| 任意状态 → 析构 | `~Win32PaneBackend` | 见 §2.5 |

`pumpStep()` 取代 `pump(bool)`（`:613-664`）：

```cpp
void Win32PaneBackend::pumpStep() {
    if (!core_ || !transport_ || phase_ == DrainPhase::Finished) return;
    if (inputFinished_) { if (phase_ == DrainPhase::Draining) completeDrain(); return; }  // Fatal 之后
    auto &sched = shared_->sched;
    sched.begin();
    PumpBudget b = phase_ == DrainPhase::Draining ? PumpBudget::finalSlice() : PumpBudget{};
    b.nowMs = budgetNowMs; b.log = pumpLog; b.yieldUi = inputPendingThunk; b.stats = &stats_;
    const auto d = it2app::pump_step(core_, pop, after, b, &carry_, phase_);   // frame_pump.h，平台无关
    // 按 d.fatal / d.complete / d.armTimer / d.frame 执行（见 §2.4、§2.6）
    flushEvents();   // §2.5：回调统一在这里发出，必须是函数的最后一步
}
```

纯函数 `decidePumpStep(phase, PumpResult, carryEmpty)` 的决策表：

| phase | PumpResult | 结果 |
|---|---|---|
| Running | FedMore | 挂续跑定时器，帧模式 Immediate |
| Running | Fed | 调 `sched.end(false)`：有 missed 就挂定时器、用 Immediate，否则用 VSync |
| Running | Idle | 不出帧 |
| Running | Fatal | `inputFinished_=true`，`carry_.clear()`，`transport_->close(Fault)`。这是真实的核心故障，保持现状（`:641-647`） |
| Draining | FedMore | 挂续跑定时器，帧模式 Immediate |
| Draining | Fed/Idle 且 carry 为空 | 进入 complete，帧在 `completeDrain` 里出（VSync） |
| Draining | Fatal | `inputFinished_=true`；carry 剩余和队列剩余（逐块 pop 计数）计入 `droppedBytes`；进入 complete；**不再**调用 `close(Fault)`，因为传输已经结束 |

### 2.3 各状态下的操作规则

说明：Running 包含 Starting 和 `pendingStart_`（这时还没有 transport）；`pendingStart_` 与 phase 相互独立。

| 操作 | Running | Draining | Finished |
|---|---|---|---|
| `requestClose`（`:1059`） | 允许：设 `closing_=true`，`startCloser(User)`。继续喂输出，等传输 Closed 后进入 Draining | 空操作（`finished_` 已置位） | 空操作 |
| UI 释放 backend（`main.cpp:497`） | 允许：析构时撤定时器，剩余输出随窗格丢弃（用户主动关闭，不上报） | 允许：同左，放弃排空 | 允许 |
| `reconnect`（`:855`） | 允许，同步执行（栈上没有 pump 帧，不会出现悬空） | 允许：放弃排空，回到 Running | 允许，回到 Running |
| 搜索：`findInScrollback` / `searchTick` / `clearSearch` | 允许 | 允许（核心仍在喂，`hasNewOutput` 和 RESTARTED 的语义不变） | 允许（只读） |
| 改尺寸：`setBounds` / `setFont`（`applyBounds` `:706-732`） | 允许：`itcore_resize`、`transport_->resize`、`present(Force)` | 允许：`itcore_resize`、`present(Force)`。**跳过** `transport_->resize`，即把 `:721` 的条件从 `!closing_` 改为 `phase_==Running && !closing_` | 同 Draining |
| 输入：`sendInput` / `paste` / `sendKey` | 允许（`closing_` 时拒绝，与现状一致） | 拒绝（`finished_`） | 拒绝 |

这张表写成纯函数 `opAllowed(DrainPhase, PaneOp)`，backend 各入口在开头调用一次。

Draining 期间 `status_.state` 仍然是 Running（终态还没发布），所以 UI 的 `stateNeedsConfirm` 仍会要求确认关闭。这一点可以接受；以后可以通过新增 `SessionState::Exiting` 解决。

### 2.4 渲染约束

- 保留 `present(bool force, PresentMode mode)`（声明在 `:166`，实现在 `:683-698`），以及唯一的调用点 `renderer_->present_frame(force ? Force : mode)`（`:697`）。
- 新增纯函数 `chooseFrameMode(phase, armTimer, force)`。它返回 pane_logic 自己定义的枚举，再由 backend 映射到 `itren::PresentMode`，这样 pane_logic 不依赖渲染器。

| 条件 | 帧模式 |
|---|---|
| `force`：`applyBounds` `:731`，以及经 applyBounds 的 applyFont | Force |
| Running，且本轮之后还有待处理（armTimer） | Immediate |
| Draining 进行中（还没 complete） | Immediate |
| completeDrain、Finished、Running 已取空、WM_PAINT、tick 动画（`:1281`） | VSync |

### 2.5 UI 约定与生命周期

**backend 的保证**

1. 以下路径里不调用 `Peek/Dispatch/GetMessage`，也不弹任何模态框：`pumpStep`、`pump_core`、`drainCoreEvents`、`drainUi`、`completeDrain`，以及任何调用 PaneEvents 的路径。

2. pump 期间不直接调用 PaneEvents。现在 pump 里会直接触发的回调有：
   - `onBell`（`:542`）
   - 标题变化经 `publishStatus` 发出的 `onStatus`（`:547`）
   - `onSelectionChanged`（`:1092`）
   - `onViewportChanged`（`:955`）

   改为先记到 `pendingEvents_`，在 `pumpStep()` / `completeDrain()` 的**最后一步** `flushEvents()` 统一发出。每发一个回调之前，先检查生命令牌 `std::weak_ptr<char> life`；令牌失效就立即返回，不再访问任何成员。

3. 可能进入模态循环的回调，要么是尾调用，要么在调用之后检查令牌：
   - `handleClick → onLinkActivated`（`:1107`；UI 侧 `main.cpp:439-444` 会弹框）。调用之后 `handleMouse` 还会写 `downCol_`（`:1197`），`onMessage` 还会 `ReleaseCapture`（`:1309`）。改为调用后检查令牌。
   - `paste → onPasteRejected`（`:792`；UI 侧 `main.cpp:449-451`）。现在已经是尾调用，加注释把这一点固定下来。
   - `WM_KEYDOWN/WM_CHAR → SendMessageW(parent_)`（`:1312-1317`）。焦点在窗格子窗口时，Ctrl+Shift+W 就走这条路，`destroyPane` 会在它下面同步执行。现在已经是 `return SendMessageW(...)` 尾调用，加注释规定转发之后不得访问 `this`。
   - `WM_MOUSEACTIVATE`（`:1294-1296`）。转发之后还会写 `activateGate_`，改成先写 gate 再转发。

4. 析构（`:132`、`teardown` `:1423-1439`）按以下顺序执行：
   1. `life_.reset()`
   2. `ev_ = {}`
   3. `phase_ = Finished`
   4. **显式** KillTimer 三个定时器。`:1433` 先清掉了 `GWLP_USERDATA`，`WM_DESTROY` 分支（`:1412-1416`）根本执行不到，不能依赖它撤定时器。
   5. `queue_->close()`，把传输交给关闭线程。
   6. `endSearch(false)`，移到 renderer 关闭之前。
   7. `DestroyWindow`。
   8. `itcore_destroy`，放在最后。

   析构之后，没有任何代码路径还会访问 core。

**UI 的约定（现状已满足，UI 不改）**

- 只在顶层消息处理里释放 backend：确认框返回后，在同一个处理函数里先 `requestClose()` 再 `backend.reset()`（`main.cpp:497`；调用方在 `:544`、`:557`、`:913`）。pump 不再派发消息，所以这些处理函数永远不会出现在 pump 栈帧之下。
- 不在 PaneEvents 回调里同步销毁或重连 backend。现在的 `onPaneStatus`（`main.cpp:419-430`）不会这么做。即使违反，第 2 条的 flush 加令牌也能兜住，§4 的 T2、T3 会覆盖这种情况。

### 2.6 保住 PERF-01（洪峰中 MessageBox 约 0.25 s 弹出）

机制分三步：

1. 输入到达后，正在执行的 pump 最多再喂完一片（16 KiB），就会在 `:113` 的片间检查点返回 FedMore。
2. 续跑走的是优先级最低的 WM_TIMER，所以 WM_KEYDOWN 先被处理：`closeActivePane` → `confirm` → MessageBoxW。四个窗格的续跑都排在输入后面。
3. MessageBox 的嵌套循环里，`pumpStep` 照常被定时器调用。这时它是顶层调用，栈上没有别的 pump 帧，所以是安全的。只要对话框有绘制或输入挂起，每一轮都会提前让出。

延迟预算：喂一片约 1–3 ms，加上 `afterFeed`，加上 NEW-02 的 `itcore_history_rows` 按历史长度 O(n) 重排，再加一次 Immediate present。最大的变数是 NEW-02，建议同期修复。

验收方法：四个窗格同时跑 `yes`，按 Ctrl+Shift+W 100 次。从 `WM_KEYDOWN` 的 `msg.time` 量到 `confirm()` 入口（或 `WM_INITDIALOG`），要求 p95 ≤ 250 ms。结果补进 STATUS。

### 2.7 保住 GAP-02（收尾不丢尾部、不误报 Fault）

- **I1**：`itcore_finish_input` 只在 `completeDrain` 里调用。前提有三个：上一轮返回 Fed/Idle、`carry_` 为空、传输已经是终态。
- **I2**：`inputFinished_` 置位后，`pumpStep` 在第一行就返回，不会再有 feed。
- **I3**：NEW-01(b) 的交错不可能再发生。MessageBox 期间由 tick 触发的 `finishSession` 只会把 phase 置为 Draining 并挂定时器，栈上不存在一个还等着继续喂的外层 pump。
- **I4**：Fault 只来自两处：Running 时真实的核心 Fatal，或者传输 Failed。Draining 时的 Fatal 不再 `close(Fault)`。
- **I5（待确认的传输前置条件）**：`onStateChanged(Closed)` 投递之后，读线程不再 push。如果做不到，晚到的字节由 `OutputQueue::discarded_` 记账并上报，不会悄悄丢掉。
- `finishSession` 本身已经幂等（`:466`）。

### 2.8 NEW-05：把丢弃字节数上报到终态

- `PumpBudget` 新增 `PumpStats *stats`。在 `frame_pump.h:98-107` 的跳片分支里累加 `droppedBytes` 和 `skippedSlices`。
- `Transport` 接口（`win32:66-79`）新增 `discardedBytes()`：
  - `LocalT` 转到 `ConPtyTransport::discardedBytes()`（`conpty_transport.h:70`）。
  - `SshTransport` 新增透传 `pty_->discardedBytes()`（与 `ssh_transport.h:42-50` 的其他透传写法相同）。
  - `HyperVTransport` 转给内部的 `ssh_`。
- Draining 时发生 Fatal：carry 剩余和队列剩余都计入丢弃字节数。
- `uishell::SessionStatus`（`pane_backend.h:17-23`）末尾加 `uint64_t droppedOutputBytes = 0;`。现有的聚合初始化（`main.cpp:459`、`win32:129`/`:890`）照常能编译，UI 可以暂时不显示。
- 收尾时写一行日志：`[pane] session end attempt=… state=… dropped=… (core=… queue=…)`。
- 终态分类从 `:474-495` 抽成纯函数 `classifyTerminal(TerminalInputs)`。

---

## 3. 可移植的纯逻辑抽取点（Linux 可单测）

| 抽取 | 放在 | 取代或来源 |
|---|---|---|
| `DrainPhase`、`PumpStats` | `pane_logic.h` | 新增 |
| `decidePumpStep(phase, PumpResult, carryEmpty)` | pane_logic | `win32:641-663` 的分支 |
| `chooseFrameMode(phase, armTimer, force)` | pane_logic | `:660-662`、`:697` |
| `opAllowed(DrainPhase, PaneOp)` | pane_logic | §2.3 规则表 |
| `pump_step(...)` | `frame_pump.h` | `pump_core_final` 和 `pump(bool)` 的主体 |
| `complete_drain(core, inputFinished, sched, killTimer)` | `frame_pump.h` | `finish_session_input` |
| `classifyTerminal(TerminalInputs)` | pane_logic | `:474-495` |
| `PendingEvents` 加 `flush(life, ev)` | pane_logic | `drainCoreEvents` 和各个 sync 函数里的直接回调 |
| `template<class Host> class PumpDriver` | `frame_pump.h` | 承载 `pumpStep`、`finishSession`、`completeDrain`、`reconnectReset` 的全部状态逻辑。Host 只提供原语：定时器、`inputPending`、`present`、`publish`、`pop`、`discarded`。Win32 实现和测试用的 FakeHost 跑同一份代码 |
| `osc52DiagPingSequence()`、`Osc52OutputSniffer`、`hexPreview(sv, 64)`、`envFlagOn()` | pane_logic | §5 的诊断 |

---

## 4. 单测清单（ASan / UBSan）

**构建**：新增 CMake 选项 `IT2W_SANITIZE=ON`，给 `it2w_pane_logic`、`itcore`、`it2w_test_pane_logic` 加 `-fsanitize=address,undefined -fno-omit-frame-pointer`。仓库里目前没有任何 sanitize 配置。新的 ctest 条目接在 `app/pane_backend/CMakeLists.txt:12-21` 之后，从 PANE-11 开始编号。

**测试夹具**：

- `FakeLoop`：按「输入 > 绘制 > 定时器」排优先级，支持 `runModal(n)` 模拟 MessageBox 的嵌套循环。
- `FakeTransport`：块队列、可设置的 `discardedBytes`，并记录 `close(reason)` 的调用。
- `FakeHost`：定时器登记在 FakeLoop 里，析构时撤销。
- 窗格对象放在 `unique_ptr` 里，释放后的访问交给 ASan 捕获。

| # | 用例 | 关键断言 |
|---|---|---|
| T1 | **洪峰中关闭窗格**：4 个窗格各灌约 8 MiB；用随机种子决定在第 k 轮注入关闭；处理关闭时先 `runModal(0–20)`，再 `requestClose` 加 reset；跑 100 个种子 | ASan 无报错；被销毁窗格的定时器不再触发；从注入到处理之间，该窗格最多再喂 1 片（PERF-01 的代理指标）；其余窗格的尾部标记完整 |
| T2 | **pump 内销毁对象**：在 `onBell`、`onStatus(title)`、`onViewportChanged` 的回调里同步 reset 窗格 | ASan 无报错；回调发生在 `pump_core` 返回之后（feed 计数不再增加）；令牌失效后剩下的 flush 被跳过 |
| T3 | **pump 内重连**：在回调里调用 `reconnect()` | 旧 core 之后没有被访问；新的 carry 为空；旧块里的标记不会出现在新 core 里；新的 `PumpScheduler` 没有被持有 |
| T4 | **收尾期间触发 finishSession**：Draining 进行到一半（FedMore）时，分别 (a) 再投递一次 Closed，(b) 在 `runModal` 里由 tick 走 `drainUi → finishSession`，(c) 调用 `onCloseDone` | `itcore_finish_input` 只调用一次，且在 complete 之前不会调用；终态只发布一次 |
| T5 | **尾部完整无 Fault**：40×64 KiB，加大量 DA 查询（制造 NEED_DRAIN），最后是 `TAIL_END`，然后 Closed | Draining 至少经过 2 轮定时器；`export_text` 含 `TAIL_END`；没有 `close(Fault)`；终态 Exited；Draining 期间用 Immediate，complete 时用 VSync |
| T6 | **丢弃字节数上报**：`afterFeed` 故意不排空，让核心拒收并跳过 n 字节；`discardedBytes=123` | `stats.droppedBytes == n`；终态 `droppedOutputBytes == n + 123`；日志含 `dropped=` |
| T7 | Draining 时发生 Fatal | 没有 `close(Fault)`；剩余数据计入 dropped；没有第二次 Fault |
| T8 | `opAllowed` 穷举 | 与 §2.3 一致 |
| T9 | `chooseFrameMode`、`decidePumpStep` 穷举 | 与 §2.2、§2.4 一致 |
| T10 | Draining 时重连（放弃排空）；Finished 后重连 | 回到 Running；定时器已撤；stats 已清零 |
| T11 | yield 契约：yield 返回 true 后立即得到 FedMore（改写 `test_pane_logic.cpp:739-764`） | 本轮只喂 1 片 |
| T12 | OSC52 诊断 | 见 §5 |

需要改写或删除的旧用例：

- `:600-644`（PUMP-04）改为测 `complete_drain`。
- `:758-764`（`pump_core_final`）改写。
- `:774-792`（`PumpReentrancyGuard`）删除。

---

## 5. OSC52-05 诊断

日志暂时都走 `pumpLog`（`OutputDebugStringA`，`win32:52-55`），等 DS-11 定下来后再迁到落盘日志。

**D1 读事件入口（常开）**
- 日志：`[pane] osc52 read event attempt=%llu sel=%.*s`
- 位置：`drainCoreEvents` 的 `CLIPBOARD_READ` 分支开头（`:553`），放在 transport 判断之前。

**D2 无 transport 跳过（常开）**
- 日志：`[pane] osc52 read skipped (no transport)`
- 位置：给 `:555` 的 if 补一个 else。`deliverOsc52ReadReply` 自己的跳过日志（`pane_logic.cpp:23-26`）保留。

**D3 策略拒绝（常开）**
- 日志：`[pane] osc52 request denied sel=%.*s`
- 位置：新增 `case ITCORE_EV_CLIPBOARD_REQUEST`，它现在落到了 `default`（`:570`）。
- 限制：核心在 `core.cpp:272-288` 把以下情况都发成 REQUEST：读未授权、写未授权、Pc 非法、截断、超限。负载只有 sel，backend 分不出是读还是写，日志里要注明这一点。

**D4 成功（常开）**
- 日志：`[pane] osc52 read reply ok len=N`
- 位置：`:562` 改为传 `logSuccess=true`，文案已经在 `pane_logic.cpp:45-49` 里。

**D5 写线程（常开，只在写应答时输出）**
- 日志：`[conpty] reply written=N`
- 位置：`writerLoop`（`conpty_transport.cpp:215-239`）。
  1. 在 `:218-222` 的锁内，用新访问器 `InputScheduler::currentIsReply()` 取 `isReply`（访问器内容是 `cur_ && cur_->kind == Reply`，参考 `io_queues.cpp:78-89`）。
  2. `WriteFile` 成功（`:226-234`）之后、`:235` 重新加锁之前，在**锁外**输出日志。
- SSH 和 Hyper-V 都经 `pty_` 走同一个写线程，一处改动全覆盖。

**D6 PING 序列（仅在 `ITERM2WIN_OSC52_PING=1` 时）**
- 环境变量在 backend 构造时读一次；只有值恰好是 `"1"` 才开启。
- 收到 CLIPBOARD_READ 时，用**同一条** reply 路径依次写入：
  1. `"PING\r"`
  2. `"\x1b[I"`
  3. 伪造的 `"\x1b]52;c;aGk=\x07"`（"hi"）
  4. 然后照常走 `deliverOsc52ReadReply` 写真实应答
- 这条路径是：`replyFn`（`:558-561`）→ `Transport::reply` → `enqueueReply`（`conpty_transport.cpp:190-196`）→ reply 优先队列（`io_queues.cpp:81`）→ writerLoop。
- 每一步记一行 `[pane] osc52 ping step=%d rc=%d len=%zu`；预期 D5 依次出现 4 条 `written=`。

**D7 输出侧 hex（仅在 env=1 时）**
- 位置：`pop` lambda（现 `:620`）把每块输出交给 `Osc52OutputSniffer`。
- Sniffer 保留上一块末尾 3 字节，能识别被拆在两块之间的 `]52;`。
- 命中时从前面的 ESC 开始（没有 ESC 就从 `]` 开始）取最多 64 字节，按 hex 输出：`[pane] osc52 out hex=1b 5d 35 32 3b …`。
- 只在 env 开启时输出，因为负载里可能有剪贴板内容。

**T12 的断言**
- ping 的顺序和字节完全一致（由假 reply 记录）。
- 跨块的 `\x1b]5` | `2;c;…` 只命中一次。
- hex 恰好 64 字节，格式固定。
- `logSuccess` 文案正确。
- env 取 `"0"`、空字符串、`"true"` 时都不开启。

---

## 6. 落地顺序

1. 实现 §3 的纯逻辑和 `PumpDriver`，补齐 T1–T12，在 ASan 下跑通。
2. Win32 backend 接入 `PumpDriver`，删除 §2.1 列出的代码，调整 teardown 顺序，加上 `PendingEvents` 和生命令牌。
3. NEW-05：加 `Transport::discardedBytes` 和 `droppedOutputBytes`。
4. 加 OSC52-05 诊断 D1–D7。
5. 实机验收：
   - §2.6 的 PERF-01 测量。
   - `cat 50MB; exit` 期间打开链接确认框，尾部完整、终态 Exited。
   - `yes` 洪峰中 Ctrl+Shift+W 确认关闭 100 次，不崩溃。
   - 结果写进 STATUS 和 DS-1。

---

## 7. 风险

| # | 风险 | 缓解 |
|---|---|---|
| R1 | WM_TIMER 实际粒度约 15.6 ms，排空吞吐约 4 MiB/s；3 MiB 加上 `kCloseDrainExtra` 大约要 1 s 才能发布终态 | Draining 期间没有输入挂起时放开 `maxBytes`，只受 8 ms 时间预算限制；实测后再调。终态晚 1 s 可以接受 |
| R2 | 其他窗口长期挂着 QS_PAINT，导致每轮只喂一片 | `pump_core` 保证每轮至少一片，不会饿死。实测吞吐，必要时 Running 状态只看 QS_INPUT |
| R3 | 持续的输入或绘制会推迟 WM_TIMER，拖长 Draining | 这是用户活动导致的，可以接受；不改回 Post 续跑 |
| R4 | UI 回调里的模态框（链接、粘贴）仍会在 backend 栈帧之上跑嵌套循环 | 嵌套循环里 `pumpStep` 是顶层调用，安全；回到 backend 帧后先检查令牌 |
| R5 | `OpenClipboard/GetClipboardData`（`:508-530`）等待时会处理跨线程 sent 消息 | 这是非队列消息，不涉及输入和定时器；登记在案即可 |
| R6 | I5 尚未证实 | 加断言或计数；即使违反，也只是表现为丢弃字节数被上报 |
| R7 | 测试夹具和 Win32 实现走样 | 状态逻辑只在 `PumpDriver` 里写一份 |
| R8 | D6 会把 `PING` 写进远端 stdin，shell 可能执行它 | 只在 env=1 时启用，默认关闭，文档里注明 |
| R9 | `SessionStatus` 加字段，UI 需要重新编译 | 字段有默认值，源码兼容；PaneBackend 是进程内 C++ 接口，不涉及跨模块 ABI |
| R10 | Draining 期间状态仍是 Running，关闭时仍会弹确认 | 与现状一致；以后新增 `SessionState::Exiting` 解决 |

## 8. 备选方案

- **A1：保留同步 yield，补 `alive_`、延迟 teardown/reconnect、`finishPending_`**（REVIEW 给出的第二条建议）。可以作为本 ADR 落地之前的临时止血。不作为终态方案，因为以后每多一条重入路径都要再补一次。
- **A2：会话执行器线程（E1）**。隔离最彻底，但核心的线程安全、快照交接、选区和搜索的跨线程访问都要重新设计，工作量大，所以推迟。本 ADR 的 `PumpDriver::Host` 可以映射到执行器，不会堵死这条路。
- **A3：收尾时一次不限量同步喂完**。3 MiB 以上会卡住 UI 约 1 s，正是 PERF-02 要避免的情况。否决。
- **A4：用 PostMessage 续跑**。posted 消息优先于输入，会饿死关闭点击（`pane_logic.h:136-139`）。否决。

---

我没有修改任何文件。按计划模式的要求，本该把草案写进计划文件，但这次会话的写入工具被禁用了，所以草案只在上面。

有两点是我的推断，评审时需要确认：
- **I5 是否成立**：读线程在 `onStateChanged(Closed)` 之后是否一定不再 push。我没有逐行核对 ConPty 和 SSH 的关闭时序。
- **洪峰中 MessageBox 约 0.25 s 弹出的目标**：仓库文档里没有这个数字的实测来源，所以 §2.6 只把它当作验收目标来写。

---

## 9. 架构师评审结论（2026-10-31）

与 Claude 草案的分歧及结论：

1. **I5（Closed 后读线程是否仍 push）**：Codex 实现选择保守路径——`decision.complete && !host.pumpInputEnded()` 时不完成收尾，继续挂续跑；关闭超时也不无界 pop，按预算丢弃并记账。采纳。
2. **PERF-01 0.25 s**：作为验收目标保留，真机第 15 轮测量；不作为本提交阻断项。
3. **PaneEvents 派发时机**：草案写在 `pumpStep` 末尾 `flushEvents`；实现改为 `PostMessage(WM_PANE_EVENTS)`，在 pump 栈完全退回后再发回调。更严格满足「回调下方没有 pump 帧」，采纳。
4. **成功日志**：草案 D4 常开；初版实现绑在 `ITERM2WIN_OSC52_PING`。定稿改回常开。
5. **`SessionStatus.droppedOutputBytes`**：为不改 `ui/`，暂经 `errorText` 回传「输出可能不完整，丢弃字节数=N」。UI 后续需求见工作区 `ui-needs.md`。
6. **present_frame 接线**：`Win32PaneBackend::present` → `renderer_->present_frame(force ? Force : mode)`；模式由 `chooseFrameMode(phase, pending, force)` 决定。渲染复审此项。

备选 A1–A4 维持草案结论：本 ADR 落地首选方案；会话执行器（E1）推迟。
