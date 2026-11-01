# ADR-003：Direct SSH v0 — 方案 A 为主

- **状态**：Accepted（Gavin 批准设计 §7 的五项倾向）
- **范围**：Windows/MinGW 的 SSH 与 Hyper-V 内层 SSH；本地继续使用 ConPTY。
- **状态与使用**：[STATUS](STATUS.md)、[ssh-direct.md](ssh-direct.md)。

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

## 能力与边界

Direct 声明 `supportsOsc52Read=true`，与默认关闭的 `osc52.allowRead` 策略门组合；
ConPTY 路径仍声明 false。Hyper-V 转发内层 SSH 能力，整个 attempt 模式固定。
关闭预算 ≤3 秒；v0 不增加 DCS/Sixel 旗标，像素尺寸为 0，不将序列透明性等同于渲染能力。
静态依赖许可随包及 GPLv3 对应源码要求见 [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES.md)
和 [UPSTREAM](../UPSTREAM.md#第三方静态依赖)。
