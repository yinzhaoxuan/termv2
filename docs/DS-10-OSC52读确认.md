# DS-10 OSC 52 读剪贴板：逐次确认（NEW-04）

日期：2026-11-04
基线：DS-16 之后
出处：`REVIEW-增补-2026-10-31.md` NEW-04（「v0.1 保持『读』只能逐次确认（会话内可选『本会话不再询问』），或者干脆只做写；写入建议在状态栏给出一次可见提示」）与 §3 DS-10；增补二 D-1；ADR-002（能力矩阵）；ADR-003 §能力与边界；`docs/TODO.md` §2.2。

凡写「决定」的地方，是本文替原设计补的选择。

## 0. 现状

- 核心：`osc52.allowRead`（settings.json 隐藏字段，默认关）打开后，`OSC 52;sel;?` 产生 `ITCORE_EV_CLIPBOARD_READ`；`itcore_encode_clipboard_reply(core, req, utf8, len, &out)` 按请求的选择参数与结尾符（BEL / ST）生成应答，`req` 必须在 `itcore_event_release` 之前使用。事件是独立的堆对象（`itcore_event_release` 就是 `delete`），不引用核心。
- 后端（`Win32PaneBackend::afterFeed` 里的 `CLIPBOARD_READ` 分支）：`osc52ReadEffective(allowRead, transport->supportsOsc52Read())` 为真就**立即**读本机剪贴板并应答，没有任何确认。
- 能力：ConPTY 路径（本地、兼容 SSH、内层兼容的 Hyper-V）里 conhost 吞掉读请求，永远收不到（ADR-002）；Direct 对读请求透明，但 D-1 起声明 `supportsOsc52Read=false`，等的就是本文。
- 写：`allowWrite` 打开后，`CLIPBOARD_WRITE` 直接写剪贴板，用户看不到任何提示。

## 1. 目标与边界

做：

1. 读请求**每次确认**（TaskDialog：「允许」「拒绝」，复选框「本会话记住此选择」）。
2. 确认界面存在之后，Direct 声明 `supportsOsc52Read=true`（Hyper-V 内层为 Direct 时随之生效）。ConPTY 不变。
3. 写入剪贴板时，每个会话（attempt）第一次在状态栏提示一次。
4. 没有接确认回调的前端（占位后端、测试）一律拒绝，不能退化成静默允许。

不做：

- 不改 `allowRead` / `allowWrite` 的默认值（仍全关），不加界面开关（D7 仍是隐藏字段）。
- 不按连接设置读写策略。
- 拒绝时不发空应答（与现状一致：程序等不到应答，自行超时）。
- 不改核心行为与 ABI；只在 `itcore.h` 的注释里写明「事件可以晚于下一次 feed 释放」这一既有事实（§3）。

## 2. 「本会话」的含义（决定）

本会话 = 当前 attempt。重连（新 attempt）后重新询问。理由：重连可能连到不同的主机（Hyper-V 换地址、改了配置），记住的授权不该延续。

## 3. 后端状态机（`app/pane_backend/pane_logic.h`，可移植，单测）

```cpp
enum class ClipboardReadDecision { AllowOnce, DenyOnce, AllowSession, DenySession };
class ClipboardReadGate {
public:
  enum class Action { Deliver, Drop, Ask, Busy };
  // canAsk = UI 接了确认回调。返回 Ask 时调用方保存事件并通知 UI，requestId 由 *id 返回。
  Action onRequest(bool canAsk, uint64_t* id);
  // 用户答复。返回 true 表示应该交付（id 必须是当前待决请求，否则 false 并忽略）。
  bool onAnswer(uint64_t id, ClipboardReadDecision);
  void reset();                         // 新 attempt：清记忆与待决请求
  bool pending() const;
};
```

规则：

- `onRequest`：已记住「允许」→ `Deliver`；已记住「拒绝」→ `Drop`；`!canAsk` → `Drop`；已有待决请求 → `Busy`（新请求丢弃，记日志）；否则 → `Ask`，分配递增的 `id`。
- `onAnswer`：`id` 不是当前待决 → `false`。`AllowSession` / `DenySession` 记住后再按 Allow / Deny 处理。返回是否交付，并清掉待决。
- 一次最多一个待决请求（决定）：程序通常串行等应答，排队没有意义，还会让用户连续看到多个对话框。

`Win32PaneBackend`：

- 成员 `ClipboardReadGate clipboardGate_`、`std::unique_ptr<itcore_event, EventReleaser> pendingClipboardRead_`。
- `CLIPBOARD_READ` 分支：策略 / 能力门不变（`osc52ReadEffective`）。通过后调 `clipboardGate_.onRequest(static_cast<bool>(ev_.onClipboardReadRequest), &id)`：
  - `Deliver`：现行逻辑（读剪贴板 → `deliverOsc52ReadReply`）。
  - `Drop` / `Busy`：记日志（`osc52 read denied (session)` / `osc52 read busy`）。
  - `Ask`：**不释放**这个事件，移入 `pendingClipboardRead_`（这条事件跳过循环末尾的 `itcore_event_release`）；`deferEvent` 通知 UI（不得在 pump 栈里弹框，ADR-001）。
- 新方法 `void answerClipboardRead(uint64_t id, ClipboardReadDecision)`（`PaneBackend` 接口，默认实现为空）：`onAnswer` 为真且核心、传输仍在 → **此刻**读剪贴板并用保存的事件 `deliverOsc52ReadReply`；然后释放事件。为假 → 只释放（若 id 匹配）。
- 新 attempt（`startSession`）与销毁：`clipboardGate_.reset()`，释放 `pendingClipboardRead_`。UI 之后的答复因 id 不匹配被忽略。
- 写提示：`CLIPBOARD_WRITE` 成功写入后，若本 attempt 还没提示过，`deferEvent` 调 `ev_.onClipboardWritten`（新回调，可空）。

`itcore.h`：在 `itcore_event_data` / `itcore_encode_clipboard_reply` 的注释里补一句：事件与核心相互独立，可以在之后的 `itcore_feed` 之后再使用和释放，但必须在 `itcore_destroy` 之前对同一核心调用 `itcore_encode_clipboard_reply`。

## 4. 接口（`ui/include/uishell/pane_backend.h`）

```cpp
enum class ClipboardReadDecision { AllowOnce, DenyOnce, AllowSession, DenySession };  // 与后端共用（后端用 uishell 的这个枚举，不另定义）
struct PaneEvents {
  …
  // 可选：远端请求读取剪贴板（仅 Direct）。UI 稍后调用 answerClipboardRead(id, …)。空 = 一律拒绝
  std::function<void(uint64_t requestId)> onClipboardReadRequest;
  // 可选：本 attempt 第一次经 OSC 52 写入了剪贴板
  std::function<void()> onClipboardWritten;
};
class PaneBackend { … virtual void answerClipboardRead(uint64_t requestId, ClipboardReadDecision) {} };
```

（§3 的 `ClipboardReadGate` 直接使用 `uishell::ClipboardReadDecision`。）

## 5. 界面（`main.cpp` + 新 `ui/shell_win32/clipboard_prompt.{h,cpp}`）

- `onClipboardReadRequest(id)`：取本机剪贴板当前文本（只用于对话框预览），生成文案，弹 `TaskDialogIndirect`：
  - 标题「剪贴板读取请求」；主文「「<连接名>」中的程序请求读取本机剪贴板」；
  - 内容：「剪贴板当前有 N 个字符。允许后，该程序会得到全部内容。」＋ 前 3 行 × 60 码点的预览（控制字符显示为 `^X`，复用 B5 的 `pastePreview` 规则）；剪贴板为空或不是文本时写「剪贴板为空」；
  - 按钮「允许」「拒绝」（默认拒绝）；复选框「本会话记住此选择」；图标警告。
  - 结果映射到四种决定，调 `backend->answerClipboardRead(id, …)`。窗格在对话框期间被关闭或重连：答复被后端忽略。
- 文案由纯函数 `uicfg::clipboardReadPrompt(connectionName, clipboardText)` 生成（返回主文与内容两段），单测。
- `onClipboardWritten`：状态栏「「<连接名>」中的程序写入了剪贴板（OSC 52）」。
- 对话框代码放 `clipboard_prompt.cpp`，接口 `std::optional<uishell::ClipboardReadDecision> runClipboardReadPrompt(HWND, const std::string& mainText, const std::string& content)`（`nullopt` = 关闭窗口，按拒绝一次处理）。

## 6. 能力与文档

- `DirectSshTransport::supportsOsc52Read()` → `true`，注释改为「读请求逐次确认（DS-10）」。Hyper-V 转发不变。
- ADR-002 能力矩阵下方、ADR-003 §能力与边界：改写为「Direct 支持 OSC 52 读，`allowRead` 打开时每次确认；ConPTY 路径仍收不到读请求」。（设计方回写。）

## 7. 测试

平台无关，进 ctest：

- `ClipboardReadGate`：四种 `onRequest` 结果；`!canAsk` 永远 Drop；待决期间 Busy；`onAnswer` 错 id / 重复答复为 false；AllowSession 后续请求直接 Deliver、DenySession 后续 Drop；`reset` 清记忆与待决、旧 id 失效。
- 延迟交付：在 `tests/app` 里用真实核心喂 `ESC]52;c;?BEL`，取出 `CLIPBOARD_READ` 事件不释放，再喂若干普通输出，然后 `deliverOsc52ReadReply` 应答以 BEL 结尾且选择参数为 `c`；ST 结尾的请求同理（证明 §3 的「事件可晚释放」）。
- `clipboardReadPrompt`：空剪贴板、短文本、超长 / 多行（截断与 `^X`）、连接名为空（本地）。

只能在 Windows 上验的：

1. `osc52.allowRead=true`，Direct 连接里 `printf '\e]52;c;?\a'`：弹出确认；允许后远端收到 base64；拒绝后没有应答；勾选记住后同一会话不再询问，重连后重新询问。
2. Neovim 配 OSC 52 provider 粘贴：确认后粘贴成功。
3. 兼容 SSH / 本地：仍然收不到请求、不弹框（ADR-002）。
4. `allowWrite=true` 时 `printf '\e]52;c;aGVsbG8=\a'`：剪贴板变为 hello，状态栏提示一次。
5. CI `mingw`、手动 `mingw-direct` 编译通过。

## 8. 改动清单

| 文件 | 改动 |
|---|---|
| `ui/include/uishell/pane_backend.h` | 决定枚举、两个回调、`answerClipboardRead` |
| `app/pane_backend/pane_logic.{h,cpp}`、`tests/app/*` | `ClipboardReadGate` 与测试；延迟交付测试 |
| `app/pane_backend/win32_pane_backend.cpp` | 门、保留事件、答复、reset、写提示 |
| `platform/windows/ssh_direct/direct_ssh_transport.h` | `supportsOsc52Read` → true |
| `ports/core/include/itcore.h` | 只改注释（§3） |
| 新 `ui/include/uicfg/…`（可放 `shell_logic.h`）＋测试 | `clipboardReadPrompt` |
| 新 `ui/shell_win32/clipboard_prompt.{h,cpp}`、`main.cpp`、`ui/CMakeLists.txt` | TaskDialog、接回调、状态栏 |
| ADR-002、ADR-003、STATUS、TODO、REVIEW 增补 NEW-04 | 回写（设计方） |
