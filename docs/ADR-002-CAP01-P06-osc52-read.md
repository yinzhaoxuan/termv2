# ADR-002：CAP-01 P06 — 经 ConPTY 的 OSC 52 读

- **状态**：Accepted（Gavin 2026-10-31 拍板）
- **证据提交**：`7a901d4`（第 15 轮真机，机器 gavin）
- **验证环境（第 16 轮 gavin 实测，2026-10-31）**：Windows build 26300.9711（26H2）；conhost.exe 文件版本 10.0.26100.9949；conpty.dll：System32 下不存在；来源：**系统自带**（`conpty_transport.cpp` 直接调 kernel32 `CreatePseudoConsole`，无 `LoadLibrary("conpty.dll")`；`rb-7a901d4` 安装包递归无 OpenConsole/conpty）
- **第 16 轮方法（对齐版，取代 PowerShell/120×30 首轮）**：独立 C# harness 调 `CreatePseudoConsole(COORD{80,24}, …, dwFlags=0)` → `hr=0x0`，与产品 `conpty_transport.cpp:113` 一致（无 `INHERIT_CURSOR`）；管道不可继承，子进程 `cmd.exe /c type <probe>`（`bInheritHandles=FALSE`，`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`），未启 `it2w_shell`。对照与第 15 轮一致：`]52;c;aGk=` 写**完整出现**（conhost 立即透传，排在其整屏重绘之前，字节不变、位置提前），`52;c;?` 读**完全消失**。结果：`/workspace/qa/realmachine-results-r16/`（`manual-r16.md`、`notes/harness-aligned.md`、`notes/aligned-hex.txt`，未入仓）。

## 决策

v0.1 **接受**「经 ConPTY 的会话不支持 OSC 52 剪贴板读取；写入仍可用」。

实际是否允许读 = `osc52.allowRead`（默认关，隐藏字段保留）**且**当前传输声明 `supportsOsc52Read`。
本地与兼容 SSH / Hyper-V 的 ConPTY 路径声明 false。Direct SSH（含以其为内层的 Hyper-V）
对读查询字节透明，但 REVIEW 增补二 **D-1** 起同样声明 `supportsOsc52Read=false`，
直到逐次确认 UI 存在；否则 `osc52.allowRead=true` 时远端可静默读走本机剪贴板。
Hyper-V 随内层 SSH 传输。能力门与策略门继续组合，不能绕过 settings。
Direct 已落地，决策与启用方式见 [ADR-003](ADR-003-ssh-direct.md) / [ssh-direct.md](ssh-direct.md)。
经 ConPTY 的会话仍**不做**逐次确认（查询到不了应用层，确认框无意义）。

## 证据摘要

- M28-A：conhost 不会自行改写剪贴板；写序列出现在输出 hex，由我们的 `allowWrite` 策略决定是否写。
- M28-B/C/D（读侧）：从未出现 `osc52 read event`；输出 hex 有写侧 `]52;`，无读查询 `52;c;?`。结论：conhost 在**输出侧吞掉** OSC 52 读查询。
- 咨询：Claude（opus, high，2026-10-31）推荐接受限制；选项「接受」与「默认关读」合并为同一项。

## 能力矩阵（OSC 52）

| 能力 | 本地 (ConPTY) | SSH (经 ConPTY) | Hyper-V (经 SSH/ConPTY) | 备注 |
|---|---|---|---|---|
| OSC 52 写剪贴板 | ✅ 受 `osc52.allowWrite` 控制，默认关 | ✅ 同左 | ✅ 同左 | conhost 透传写序列（已实测） |
| OSC 52 读剪贴板 | ❌ 不支持 | ❌ 不支持 | ❌ 不支持 | CAP-01 P06：conhost 输出侧吞掉 `52;c;?` |

Direct SSH（含以 Direct 为内层的 Hyper-V）对 OSC 52 读查询字节透明。D-1 起曾不声明读能力；
DS-10（2026-11-04）加了逐次确认后，Direct 声明 `supportsOsc52Read=true`：`osc52.allowRead=true`
时每个读请求都弹框确认（可「本会话记住」，重连后重新询问），没有确认回调的前端一律拒绝。
写入仍受 `osc52.allowWrite` 控制，每个会话第一次写入时状态栏提示。上表仅列 ConPTY 路径。

## 已知限制原文

> **CAP-01 P06：经 ConPTY 的会话不支持 OSC 52 剪贴板读取**
>
> termv2 的本地、兼容 SSH 和以内层兼容 SSH 连接的 Hyper-V 会话经过 Windows ConPTY。实测表明，conhost 会在输出侧丢弃 OSC 52 读取请求（`ESC ] 52 ; c ; ?`），请求到不了 termv2，所以 termv2 无法应答。OSC 52 写入不受影响，仍由 `osc52.allowWrite` 控制（默认关闭）。Direct SSH 的能力与策略门另见 ADR-003。
>
> **影响**：依赖 OSC 52 读取剪贴板的程序（例如配置了 OSC 52 剪贴板 provider 的 Neovim、vim 插件，以及部分 tmux 配置）在执行“粘贴”时会等不到应答，可能超时或报错。termv2 收不到这类请求，所以不会弹出任何提示。
>
> **替代做法**：请使用终端粘贴（Ctrl+Shift+V、右键粘贴），或把程序的剪贴板配置成“复制走 OSC 52、粘贴走终端粘贴”。
>
> **安全说明**：OSC 52 读取会让终端内运行的程序拿到本机剪贴板内容。即使在支持读取的终端里，这项能力通常也是默认关闭或每次都要确认。

## 第 16 轮序列探针（gavin，`rb-7a901d4`）

| ID | 序列 | 判定 |
|---|---|---|
| S1 | OSC 1337 SetMark | 完整透传 |
| S2 | OSC 133;A | 完整透传 |
| S3 | OSC 8 超链接 | **被改写**（conhost 自加 `id=`，BEL→ST；URL/文本保留） |
| S4 | DCS `ESC P1000p`（tmux -CC 入口） | **完全消失**；无 ST 时还吞后续整段流 |
| S4' | `ESC P1000p ESC\` | DCS 本身消失，前后标记在 |
| S5 | Sixel DCS | **完全消失** |

**结论：条件 1 已触发 →「直连 SSH 须重评」。** 真实 tmux -CC 入口在 `ESC P1000p` 后不发 ST，经 ConPTY 做不了 tmux 控制模式集成。S1 透传说明 harness 能抓到 OSC；第 15 轮已证 OSC 52 写透传、读查询被吞。渲染侧：OSC 8 解析需接受 ST 与 conhost 生成的 `id=`；Sixel 经 ConPTY 不可用。5e（输入侧注入）未做。

## 何时重评直连 SSH

满足任一条件即重新评估绕开 conhost 的直连通道（范围：仅 SSH；Hyper-V 随 SSH 受益；本地不做）：

1. ~~真机 hex 探针证明 ConPTY 也吞掉 **OSC 1337** 或 **tmux -CC（DCS）**~~ → **已触发（第 16 轮：S4 DCS 被吞；S1 OSC 1337 透传）**。
2. 用户反馈「远程 nvim/tmux 粘贴」进入优先问题。
3. 路线图本就要做 SSH 层深度集成，可一并摊成本。

### 第 18 轮直连探针结论（2026-10-31，Windows → Linux）

方案对照（Gavin 已批「B 起步」）：B = 纯管道 `ssh.exe -tt`；A = 进程内 libssh + `window-change`。

| 项 | 结果 | 含义 |
|---|---|---|
| R18-1 控制序列 | **PASS**（79 B 逐字节） | B 路径 DCS（含无 ST）/ Sixel / OSC 52 读+写均透明；tmux -CC 与 `52;c;?` 在此路径可行 |
| R18-2 TTY 尺寸 | **FAIL**（`stty size` = `0 0`） | B 无法初始化或中途改远端尺寸 → **产品直连硬需求方案 A** |
| R18-3 交互提示 | 管道无 ask/password UI | 默认路径预信任 known_hosts（v0.3 不做 accept-new）；密码与首次主机密钥 ask 需 ASKPASS（实现期再定） |


**A 探针（2026-11-01，Gavin 的 Windows lab）**：独立 `ssh_a_probe`（进程内 libssh2 + OpenSSL 静态 MinGW）`--test all` 六项 PASS（agent auth、79B、stty、33×window-change、close-flood、close-auth CancelSynchronousIo）。产品直连以方案 A 为主；产物不入仓。
**当前立场：** Direct SSH v0 已按 A 为主落地；B 保留为控制序列透明性的对照证据，不采用 A+B 混合。
决策见 [ADR-003](ADR-003-ssh-direct.md)，使用方式见 [ssh-direct.md](ssh-direct.md)。
第 18 轮证据见 `/workspace/qa/realmachine-results-r18/manual-r18.md`（未入仓）。

## 实现要点

- `Transport::supportsOsc52Read()` 默认 `false`；LocalT 仍为 false，SshT / HyperVT 转发内层能力。
  ConPTY 为 false；Direct 自 DS-10 起为 true，读请求经 `ClipboardReadGate` 逐次确认（D-1 时曾临时为 false）；每个 attempt 的能力固定。
- `ITCORE_EV_CLIPBOARD_READ` 路径：`osc52ReadEffective(allowRead, supportsOsc52Read())` 为假则不交付；策略关记 `read denied`，策略开但传输不支持记 `read unsupported (transport)`。
- 单元测试：`osc52ReadEffective` 四象限。

## 非目标（本 ADR）

- 不随包附带 OpenConsole/conpty NuGet（可选后续实验）。
- 不验证输入侧注入应答形状（5e）——单独探针项。
- 不改 `ui/`。
