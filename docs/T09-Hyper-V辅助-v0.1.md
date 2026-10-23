# T09 Hyper-V 辅助：范围与方案 v0.1

更新时间：2026-10-23（Asia/Shanghai）。依据：《iTerm2-Windows移植设计-v0.3》第 7 节（Hyper-V 连接步骤）、第 13.2 节（`endpoint.mode=hyperv` 配置）、第 16 节（解析与失败处理）、第 17 节（错误类别）、验收 VM-01、任务表 T09（依赖 T08）。仓库内此前没有单独的 T09 规格，本文即规格。

## 1. 文档给定的范围

- Hyper-V 是 **地址解析辅助**，终端通道仍是 SSH（T07 `SshTransport`）。文档的连接方式是"按 VM ID 查状态与网卡地址 → 选地址 → 系统 ssh 连接"，**不是** PowerShell Direct、`hvc.exe` 或 `vmconnect`。这三者不在 T09 范围：PowerShell Direct 需要来宾凭据与管理员权限，与"普通 SSH 不依赖 Hyper-V 权限""认证提示由 OpenSSH 显示在终端"冲突；`vmconnect` 是图形控制台，不是字节流传输。
- 只支持本机宿主机（`hostScope=local`）。按 VM ID（GUID）查找，不用名称通配；显示名只用于展示。
- 查询前检查模块与权限；后台查询返回结构化结果；固定脚本，VM ID 通过绑定方式传入，不拼进命令代码。
- 地址筛选默认排除未指定、回环、无作用域的链路本地地址；不笼统排除私有网段。IPv4 优先是配置策略。
- 计时：轮询 2 s，单次查询超时 5 s，等待启动与地址总预算 60 s；认证交互不计入。
- 启动是显式管理操作，只发一次；取消等待不关 VM；普通连接失败不启动、重启或关闭 VM。
- 多地址有歧义时不盲选第一项；地址缺失时允许手动地址回退；SSH 失败不自动尝试其他地址。
- 地址变化仍校验主机密钥，不关闭指纹校验（沿用 T07 快照的 known_hosts 策略）。

## 2. 选定方案

### 2.1 查询通道：powershell.exe + 固定脚本 + 环境变量传参
- 执行 `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand <固定脚本>`。脚本文本是编译期常量，`-EncodedCommand` 只是为了避开命令行引号规则。
- VM ID 先在本进程按 GUID 格式严格校验（`8-4-4-4-12` 十六进制），再经子进程环境变量 `IT2W_VMID` 传入，脚本用 `[guid]$env:IT2W_VMID` 读取并 `Get-VM -Id`。用户内容从不进入命令代码。
- 输出：单行 JSON（`ConvertTo-Json -Compress`），字段 `{v:1, ok, err, msg, vms:[{id,name,state,adapters:[{id,name,mac,ips:[...]}]}]}`。`err` 取 `ModuleMissing` / `AccessDenied` / `NotFound` / `Failed`。
- 枚举（UI 选择 VM 用）与查询共用同一脚本：`IT2W_VMID` 为空时列出全部 VM。
- 启动：同一通道的第二段固定脚本 `Start-VM -VM (Get-VM -Id ...)`，只在策略允许时调用一次。
- 进程在 Job（KILL_ON_JOB_CLOSE）里运行，超时 5 s 或取消即终止。

为何不用 WMI/`root\virtualization\v2` COM：C++ 侧代码量大、mingw 下 WMI 头支持不全，而文档本身以 `Get-VMNetworkAdapter` 为依据。WMI 可在实机验证后作为替换实现，接口不变。

### 2.2 权限与错误映射
| 情况 | 结构化结果 | 错误类别（第 17 节） | 窗格状态 / 文本 |
|---|---|---|---|
| 未安装 Hyper-V 模块（`Get-Command Get-VM` 失败） | `ModuleMissing` | `VmAccessDenied`（能力缺失） | Failed："Hyper-V 管理模块不可用…可改用手动地址" |
| 不在 Hyper-V Administrators 组 / 未提权（`PermissionDenied`、`AuthorizationManager`、访问被拒） | `AccessDenied` | `VmAccessDenied` | Failed："没有查询 Hyper-V 的权限…" |
| VM ID 不存在 | `NotFound` | `VmNotFound` | Failed："虚拟机已移除或标识变化"，不按同名回退 |
| powershell.exe 不存在 / 启动失败 | 本地 | `ExecutableNotFound` / `ProcessStartFailed` | Failed |
| 单次查询超时 5 s / 输出不可解析 | 本地 | `VmAddressUnavailable`（重试计入预算） | 继续轮询，预算用尽后 Failed |
| 未运行且策略不允许启动 | — | — | Failed："虚拟机未运行（状态 X）"，需 UI 提供"启动并连接" |
| 运行但无地址，超出 60 s | — | `VmAddressUnavailable` | Failed："等待地址超时，请检查来宾集成服务，或改用手动地址" |
| 多个候选且无记住的网卡 | — | — | Failed："有多个候选地址"，列出 网卡/地址，需 UI 选择器 |
| 解析不可用但配置了 `fallbackHost` | — | — | 用手动地址连接，并在诊断记录 `hyperv.fallback` |

不提权：应用始终以普通用户运行；权限不足只影响 Hyper-V 辅助，普通 SSH 与手动地址照常可用。

### 2.3 平台无关逻辑（Linux 单测 HV-01）
`platform/windows/common/hyperv_resolve.*`：GUID 校验；查询 JSON 解析；地址筛选（IPv4 `0.0.0.0`、`127/8`、`169.254/16`；IPv6 `::`、`::1`、无 `%scope` 的 `fe80::/10`）；按 `adapterId` 与 `addressFamily`（`ipv4`/`ipv6`/`any`）选择；`HyperVResolver` 状态机（输入每次查询结果与时间，输出 Query/Start/Wait/Connect/Fail 决策），负责 2 s/5 s/60 s 计时、启动只发一次、回退与歧义判定；错误文本生成。

### 2.4 Windows 集成
- `platform/windows/hyperv/hyperv_query.*`：`runHyperVQuery(vmId, timeout, cancelEvent)`、`runHyperVStart(...)`、`listHyperVVms(...)`。
- 传输：`HyperVTransport` 实现后端的 `Transport` 抽象。`start` 立即返回，后台线程跑解析状态机（状态 `Starting`，窗格标题显示"正在解析 Hyper-V 地址"）；得到地址后把 `hostName` 填入 `SshLaunchOptions` 再启动内部 `SshTransport`，之后所有调用转发给它。解析期间输入与粘贴丢弃（返回 `Closed` 以外的 Ok，不排队），`close` 设置取消事件并等待线程。
- `PaneBackendConfig::resolveHyperV(connectionId, *error)` 返回 `HyperVLaunch{policy, ssh}`；后端 resolve 路径先问 `resolveHyperV`，非空走 Hyper-V，否则走原有 `resolveSsh`。`policy.startIfOff`（默认 false）由 UI 在用户点击"启动并连接"后置 true 重新创建窗格。

## 3. 不做 / 待 UI
- "启动并连接"按钮、多地址选择器、"等待地址"提示：头文件没有交互回调，首版以 Failed 文本表达，记入 STATUS 接口问题。
- 远程宿主机、HostKeyAlias 稳定映射（待决项）、PowerShell Direct、`hvc.exe`、`vmconnect`。
- 实机验证（VM-01：关机、无地址、多地址、改名、无权限、取消）须在装有 Hyper-V 的 Win11 上进行；Linux 只验证纯逻辑与 mingw 编译。
