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

没写 `transport` 的连接跟随**全局默认**（[ADR-006](ADR-006-SSH默认传输.md)），按下面顺序取第一个有值的：

1. 环境变量 `ITERM2WIN_SSH_DIRECT`：恰为 `1` → 直连，恰为 `0` → 兼容；`true` / `ON` 等其他值视为没设。窗格构造时读一次。
2. settings.json 的隐藏字段 `sshDefaultTransport`：`"auto"` / `"compatible"` / `"direct"`。只影响此后新建的窗格；值非法时按内置默认处理，启动时状态栏提示一次，原值保存时原样写回。
3. 内置默认：目前是兼容（ADR-006 阶段 1）。满足 ADR-006 阶段 2 的门槛后改为 `auto`。

`auto`：构建含 Direct 且认证方式是 agent → 直连；否则兼容。它是启动前的静态选择，不是失败后回落。

```powershell
$env:ITERM2WIN_SSH_DIRECT = '1'   # 或 '0'：全部退回 ssh.exe
.\it2w_shell.exe
```

连接编辑器的传输下拉框第一项显示「跟随全局默认（当前：直连 / 兼容）」，按编辑中的认证方式实时计算。每次启动 SSH 时 `pane-diag.log` 记一条 `ssh.transport direct=0/1 source=connection/env/settings/builtin`。

connections.json 只在产品启动时读入，改了 `transport` 要重启产品才生效。
显式选了 Direct（连接、环境变量或 settings 的 `direct`）而构建未包含它时明确报错。
`"transport": "direct"` 配 KeyFile / Interactive 认证会在加载校验时被拒绝，窗格不启动。
连接、主机密钥或认证失败均**不自动回落**。直连不是连接自己写明的时候，失败文字末尾会说明它从哪来（环境变量、settings 或默认）以及怎么改回 `ssh.exe`。

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
- **首次信任**（DS-12）：未知主机（含新的 Hyper-V 虚拟机）在窗格上给出「核对指纹…」，用户确认后写入 `known_hosts` 并重连；不 accept-new，密钥变化不提供信任按钮。是否把 Direct 改为默认传输见 [ADR-006](ADR-006-SSH默认传输.md)（提议中）。

## 能力与限制

- Direct 声明 `supportsOsc52Read=true`：`osc52.allowRead=true` 时，每个读请求都弹确认框（DS-10，取代 D-1 的临时关闭）。
  Hyper-V 随内层传输；本地和兼容 SSH/Hyper-V 的 ConPTY 路径收不到读请求，为 false。
  写剪贴板仍受 `osc52.allowWrite` 控制，见 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。
- PTY 使用 `xterm-256color`，网格至少 20×5；Starting 期间合并尺寸，并在 shell 启动前补齐最新尺寸。
  v0 的 PTY / window-change **像素尺寸填 0**。
- 关闭预算 **≤3 秒**，包含 owner 线程退出；DNS 与 agent 同步等待可取消。
  协议关闭最多 2700 ms，必要时由 owner 硬断 socket。没有 ssh.exe 子进程。
- 远端退出码 255 仍是 `RemoteExit(255)`，不套用 ssh.exe 的客户端错误语义。
- 字节透明不代表 tmux 控制模式或 Sixel 渲染已交付；v0 不新增 DCS / Sixel 能力旗标。
  不支持 agent/X11/端口转发、ProxyJump、主机证书或交互凭据提示；未实现 SSH/TCP keepalive。
  传输按连接选择（DS-13），首次信任见 DS-12；默认传输的去向见 [ADR-006](ADR-006-SSH默认传输.md)（提议中）。

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
