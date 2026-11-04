# ADR-003：Direct SSH v0 — 方案 A 为主

- **状态**：Accepted（Gavin 批准设计 §7 的五项倾向）
- **范围**：Windows/MinGW 的 SSH 与 Hyper-V 内层 SSH；本地继续使用 ConPTY。
- **状态与使用**：[STATUS](STATUS.md)、[ssh-direct.md](ssh-direct.md)。
- **S6 产品手测**：gavin 上 `it2w_shell` Direct（含 diag / 无 ssh.exe / exit / resize / vim）已于 2026-11-02 收口，细节见 [STATUS](STATUS.md)。

## 决策

产品直连以 **A（进程内 libssh2 + OpenSSL）为主**，通过 SSH PTY / window-change 管理远端尺寸。
B（纯管道 ssh.exe）仅完成控制序列透明性的对照验证；其远端尺寸为 0×0，不能作为产品传输。
不采用 A+B 混合通道。证据沿用 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。

设计 §7 的五项开放问题按 Gavin 批准的倾向定案：

1. **构建默认值**：开发期 `IT2W_SSH_DIRECT=OFF`；发布包可开启 ON，运行时仍由
   `ITERM2WIN_SSH_DIRECT=1` 控制。选中 Direct 而构建不含该功能时明确报错，不自动回落。
2. **KeyFile**：未加密 KeyFile 不进入产品 v0；v0 仅支持 agent，其他认证模式明确拒绝。
3. **主机密钥偏好**：已知类型在前，其余支持的类型在后；按实际出示类型校验，未知/变化失败关闭。
   known_hosts 只读且须预信任。
4. **POSIX/Linux 实连**：不进入 v0；Linux 运行纯逻辑测试，实连验收在 Windows 上用
   `ssh_direct_accept` 和产品手测完成。
5. **agent 选择**：仅接受以 `\\.\pipe\` 开头的 `SSH_AUTH_SOCK`，否则使用 Windows OpenSSH 默认管道；
   永不使用 Pageant，保留 `cmake/ssh_a_openssh_only.cmake` 构建补丁。

## Hyper-V 主机密钥身份（REVIEW 增补二 D-3）

Direct 的 `known_hosts` 只读，不 accept-new。查找名原来是解析出的地址（22 端口为地址本身，其它端口为 `[地址]:端口`）。Hyper-V 来宾常用 DHCP，地址一变就是 `HostKeyUnknown`。兼容模式的快照以前没有 `HostKeyAlias`，切回去确认指纹时 OpenSSH 把密钥记在新 IP 上，下次地址再变，两种模式一起失效。

Hyper-V 连接的身份改为稳定别名 `hv-<规范化 VM GUID>`（`normalizeVmId`：小写，可去掉花括号）。`HyperVTransport` 在启动内层 SSH 前写入 `SshTarget::hostKeyAlias`；快照里的 `HostName` 仍是当前地址。普通 SSH 不设别名，查找方式不变。

- **兼容模式**：`-F` 快照写出 `HostKeyAlias`，并写 `CheckHostIP no`。OpenSSH 用别名原文查找，用户确认后也按这个原文写入 `known_hosts`（`HostKeyAlias` 不再套 `put_host_port`，因此不加 `[别名]:端口`）。`CheckHostIP no` 避免再把当时的 IP 记进去：旧版 OpenSSH 默认检查 IP，新地址若曾属于别的机器，别名已经命中仍会失败。
- **直连**：`knownHostTypesFor` 与 `libssh2_knownhost_check` 使用同一查找串（`directHostKeyLookup`）。有别名时就是该别名的 ASCII 小写，不看 IP，也不加端口括号。没有别名时与原来的 `put_host_port` 相同。
- 仍然不写入 `known_hosts`，不开启 accept-new。以前按 IP 信任过的虚拟机，要在兼容模式里再确认一次；之后地址变化不用重新信任。手动回退地址同样用 VM 别名，不用那次填的主机名。

首次信任界面（展示指纹、确认后写入 `known_hosts`）仍是把 Direct 做成默认传输的前置条件。本次不实现：`ui/` 不改，Direct 继续只读预信任文件。

## 能力与边界

Direct 声明 `supportsOsc52Read=true`，但每个读请求都要用户确认（DS-10，取代 D-1 的临时关闭）；
`osc52.allowRead` 策略门（默认关）仍保留。ConPTY 收不到读请求（ADR-002）。Hyper-V 转发内层 SSH 能力，整个 attempt 模式固定。
关闭预算 ≤3 秒；v0 不增加 DCS/Sixel 旗标，像素尺寸为 0，不将序列透明性等同于渲染能力。
静态依赖许可随包及 GPLv3 对应源码要求见 [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES.md)
和 [UPSTREAM](../UPSTREAM.md#第三方静态依赖)。
