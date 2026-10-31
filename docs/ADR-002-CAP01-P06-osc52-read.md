# ADR-002：CAP-01 P06 — 经 ConPTY 的 OSC 52 读

- **状态**：Accepted（Gavin 2026-10-31 拍板）
- **证据提交**：`7a901d4`（第 15 轮真机，机器 gavin）
- **验证环境（占位，探针补齐）**：Windows build ＿＿＿；conhost.exe 文件版本 ＿＿＿；conpty.dll（若存在）＿＿＿；来源：系统自带 / 随附 OpenConsole ＿＿＿

## 决策

v0.1 **接受**「经 ConPTY 的会话不支持 OSC 52 剪贴板读取；写入仍可用」。

实际是否允许读 = `osc52.allowRead`（默认关，隐藏字段保留）**且**当前传输声明 `supportsOsc52Read`。本地 / SSH / Hyper-V 均经 ConPTY，一律声明不支持。这是**能力门**，不是强制忽略 settings：将来若有直连传输可声明支持后再与策略组合。v0.1 **不做**逐次确认（查询到不了应用层，确认框无意义）。

## 证据摘要

- M28-A：conhost 不会自行改写剪贴板；写序列出现在输出 hex，由我们的 `allowWrite` 策略决定是否写。
- M28-B/C/D（读侧）：从未出现 `osc52 read event`；输出 hex 有写侧 `]52;`，无读查询 `52;c;?`。结论：conhost 在**输出侧吞掉** OSC 52 读查询。
- 咨询：Claude（opus, high，2026-10-31）推荐接受限制；选项「接受」与「默认关读」合并为同一项。

## 能力矩阵（OSC 52）

| 能力 | 本地 (ConPTY) | SSH (经 ConPTY) | Hyper-V (经 SSH/ConPTY) | 备注 |
|---|---|---|---|---|
| OSC 52 写剪贴板 | ✅ 受 `osc52.allowWrite` 控制，默认关 | ✅ 同左 | ✅ 同左 | conhost 透传写序列（已实测） |
| OSC 52 读剪贴板 | ❌ 不支持 | ❌ 不支持 | ❌ 不支持 | CAP-01 P06：conhost 输出侧吞掉 `52;c;?` |

## 已知限制原文

> **CAP-01 P06：经 ConPTY 的会话不支持 OSC 52 剪贴板读取**
>
> termv2 的本地、SSH 和 Hyper-V 会话都经过 Windows ConPTY。实测表明，conhost 会在输出侧丢弃 OSC 52 读取请求（`ESC ] 52 ; c ; ?`），请求到不了 termv2，所以 termv2 无法应答。OSC 52 写入不受影响，仍由 `osc52.allowWrite` 控制（默认关闭）。
>
> **影响**：依赖 OSC 52 读取剪贴板的程序（例如配置了 OSC 52 剪贴板 provider 的 Neovim、vim 插件，以及部分 tmux 配置）在执行“粘贴”时会等不到应答，可能超时或报错。termv2 收不到这类请求，所以不会弹出任何提示。
>
> **替代做法**：请使用终端粘贴（Ctrl+Shift+V、右键粘贴），或把程序的剪贴板配置成“复制走 OSC 52、粘贴走终端粘贴”。
>
> **安全说明**：OSC 52 读取会让终端内运行的程序拿到本机剪贴板内容。即使在支持读取的终端里，这项能力通常也是默认关闭或每次都要确认。

## 何时重评直连 SSH

满足任一条件即重新评估绕开 conhost 的直连通道（范围：仅 SSH；Hyper-V 随 SSH 受益；本地不做）：

1. 真机 hex 探针证明 ConPTY 也吞掉 **OSC 1337** 或 **tmux -CC（DCS）**（亦测 OSC 133、OSC 8、Sixel 作对照）。
2. 用户反馈「远程 nvim/tmux 粘贴」进入优先问题。
3. 路线图本就要做 SSH 层深度集成，可一并摊成本。

## 实现要点

- `Transport::supportsOsc52Read()` 默认 `false`（LocalT / SshT / HyperVT 均为 ConPTY）。
- `ITCORE_EV_CLIPBOARD_READ` 路径：`osc52ReadEffective(allowRead, supportsOsc52Read())` 为假则不交付；策略关记 `read denied`，策略开但传输不支持记 `read unsupported (transport)`。
- 单元测试：`osc52ReadEffective` 四象限。

## 非目标（本 ADR）

- 不随包附带 OpenConsole/conpty NuGet（可选后续实验）。
- 不验证输入侧注入应答形状（5e）——单独探针项。
- 不改 `ui/`。
