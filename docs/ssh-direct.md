# Direct SSH v0

Windows/MinGW 的可选直连传输，在进程内使用静态 libssh2 + OpenSSL，绕开 ssh.exe/ConPTY。
普通 SSH 和 Hyper-V 的内层 SSH 使用同一个传输选择。决策见 [ADR-003](ADR-003-ssh-direct.md)，
落地与验收状态见 [STATUS](STATUS.md)。

## 开启

开发构建默认 `IT2W_SSH_DIRECT=OFF`。在配置好仓库要求的 MinGW POSIX 工具链、CMake、GNU make
和 Perl 后，从仓库根目录构建：

```sh
cmake -S . -B build-mingw-direct -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DIT2W_SSH_DIRECT=ON
cmake --build build-mingw-direct --parallel 4
```

依赖固定版本与 SHA-256 见 [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES.md)。
首次构建会下载并编译源码；离线可设 `SSH_A_OPENSSL_URL` / `SSH_A_LIBSSH2_URL` 指向相同源码包，
hash 校验仍强制执行。v0 不支持 MSVC 或 POSIX 直连传输。

**按连接选择**（DS-13）：在 `connections.json` 该连接的 `ssh` 里写 `"transport": "direct"` 或 `"compatible"`。
Hyper-V 连接的内层 SSH 同样按这个字段选择。写明的值优先于环境变量。

```json
"ssh": { "user": "gavin", "auth": { "mode": "agent" }, "transport": "direct" }
```

没写 `transport` 的连接跟随**全局默认**：在 Windows PowerShell 中，于启动产品前设置环境变量：

```powershell
$env:ITERM2WIN_SSH_DIRECT = '1'
.\it2w_shell.exe
```

**仅值恰为 `1` 时开启**；`true` / `ON` 等值不生效。环境变量在窗格构造时读取一次；connections.json 只在产品启动时读入，改了 `transport` 要重启产品才生效。
两者都没有时使用兼容传输（默认值不变）。选了 Direct 而构建未包含它时明确报错。
`"transport": "direct"` 配 KeyFile / Interactive 认证会在加载校验时被拒绝，窗格不启动。
连接、主机密钥或认证失败均**不自动回落**。要改用兼容模式，把该连接的 `transport` 改为 `"compatible"`，或移除环境变量后重新启动产品。

窗格诊断（含 `ssh.direct.*`）写入 `<configDir>/logs/pane-diag.log`，约 5 MiB × 3 滚动（当前文件加 `.1`、`.2`）。
`configDir` 来自窗格配置；shell 未传入时与默认配置目录一样使用 `%APPDATA%\it2w`，该目录也不可用则不写。
每行包含 sid、attempt、phase、elapsed_ms 和 err。
日志可能含主机名、主机密钥指纹和 agent 管道路径。需要发给别人时用系统菜单「导出诊断包…」：自动脱敏并先预览（DS-15），不要直接发原始日志。

## 认证与主机信任

- 产品 v0 **仅支持 agent**：先将服务端授权的密钥用 `ssh-add` 加入 Windows OpenSSH agent。
  `KeyFile`（包括未加密私钥）、`Interactive`、password、keyboard-interactive 和 ASKPASS 均不支持。
- 只有 `SSH_AUTH_SOCK` 以 `\\.\pipe\` 开头时才采用；否则使用默认
  `\\.\pipe\openssh-ssh-agent`。**永不使用 Pageant**，构建保留移除后端条目的补丁。
- 应用的 `configDir/ssh/known_hosts` 中没有的主机：窗格给出「核对指纹…」，用户确认后写入并重连（DS-12）。
  不 accept-new；密钥变化不提供信任按钮。
- 握手偏好为**已知类型在前，其余支持的类型在后**；握手后按服务器实际出示的类型核对密钥。
  支持普通和哈希条目；`@cert-authority` / `@revoked` 标记报配置错误。
  未知类型/主机报 `HostKeyUnknown`，密钥变化报 `HostKeyChanged`，均失败关闭；按受信渠道核实后再更新信任。
- **Hyper-V 地址会变（D-3）**。来宾 DHCP 换地址后，若主机密钥按解析出的 IP 查找，下一次就是 `HostKeyUnknown`；兼容模式快照以前也不写 `HostKeyAlias`，重新信任只是把新 IP 记下来，下次再变仍然失败。
  Hyper-V 现在用稳定别名 `hv-<规范化 VM GUID>`（小写，花括号会去掉）。两种模式都按这个别名核对，不按当时的地址。
  兼容模式的 `-F` 快照包含 `HostKeyAlias`，`HostName` 仍是当前地址，并设 `CheckHostIP no`，避免 OpenSSH 再把瞬时 IP 写进 `known_hosts`。OpenSSH 按别名原文查找和保存，不附加端口。
  直连用同一字符串调用 `libssh2_knownhost_check`（别名不做 `[别名]:端口`）。普通 SSH 不设别名，仍按主机名或 `[主机]:端口` 查找。
  已经按 IP 信任过的虚拟机需要在兼容模式里再确认一次，之后换地址不用重做。手动回退地址也用这台 VM 的别名。
- **首次信任界面仍未做**，并且仍是 Direct 改为默认传输的前置条件：展示指纹、用户确认、再写入 `known_hosts`。本次不改 `ui/`，Direct 继续只读、不 accept-new。新虚拟机要先用兼容模式确认一次指纹。

## 能力与限制

- Direct 暂声明 `supportsOsc52Read=false`（REVIEW 增补二 D-1：逐次读确认 UI 之前不开放）。
  通道对 OSC 52 读查询透明，但应用层不应答，即使 `osc52.allowRead=true`。
  Hyper-V 随内层传输；本地和兼容 SSH/Hyper-V 的 ConPTY 路径同样为 false。
  写剪贴板仍受 `osc52.allowWrite` 控制，见 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。
- PTY 使用 `xterm-256color`，网格至少 20×5；Starting 期间合并尺寸，并在 shell 启动前补齐最新尺寸。
  v0 的 PTY / window-change **像素尺寸填 0**。
- 关闭预算 **≤3 秒**，包含 owner 线程退出；DNS 与 agent 同步等待可取消。
  协议关闭最多 2700 ms，必要时由 owner 硬断 socket。没有 ssh.exe 子进程。
- 远端退出码 255 仍是 `RemoteExit(255)`，不套用 ssh.exe 的客户端错误语义。
- 字节透明不代表 tmux 控制模式或 Sixel 渲染已交付；v0 不新增 DCS / Sixel 能力旗标。
  不支持 agent/X11/端口转发、ProxyJump、主机证书或交互凭据提示；未实现 SSH/TCP keepalive。
  模式选择是环境变量全局开关，尚无按连接选择或首次信任 UI。

## 验收与随包文件

产品 API 验收载体是 `ssh_direct_accept.exe`；配置时另加
`-DIT2W_BUILD_SSH_DIRECT_ACCEPT=ON`（必须同时开启 Direct）。在 Windows 上用预信任文件和
已加载密钥的 agent 运行；使用 `--help` 查看 `--host`、`--user`、`--known-hosts` 等必填参数。
`--test all` 覆盖认证、首条 stty/Starting 尺寸、79 字节、resize、modes 和洪水/认证中关闭；
主机密钥类型/未知/变化、exit-255、dns-cancel 和 gate 是独立用例，需要相应测试条件。
完整环境开关、进程/快照与兼容模式对照仍靠产品手测，不能由 `gate` 子测试替代。
gavin 上 Direct 产品手测（含 `pane-diag.log`、无 ssh.exe、exit/resize/vim）已于 2026-11-02 完成，见 [STATUS](STATUS.md)。

Linux/macOS 只运行现有纯逻辑测试，不能替代 Windows 实连验收；v0 不增加 POSIX/Linux live harness。
独立 [A 探针](ssh-a-probe.md) 保留作依赖基线，不替代产品 API 验收。

全量构建默认运行 `it2w_third_party_notices`；若只构建了 `it2w_shell` 目标，打包前须再运行
`cmake --build build-mingw-direct --target it2w_third_party_notices`。
分发时携带 exe 旁 `licenses/` 中的三份上游原文与 `THIRD-PARTY-NOTICES.md`；
独立探针保留 `ssh-a-licenses/`。对应源码交付见 [UPSTREAM](../UPSTREAM.md#第三方静态依赖)。
