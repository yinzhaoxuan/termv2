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

在 Windows PowerShell 中，于启动产品前设置环境变量：

```powershell
$env:ITERM2WIN_SSH_DIRECT = '1'
.\it2w_shell.exe
```

**仅值恰为 `1` 时开启**；`true` / `ON` 等值不生效。开关在窗格构造时读取一次，重连沿用原模式。
未设置时继续使用兼容传输；选了 Direct 而构建未包含它时明确报错。
连接、主机密钥或认证失败均**不自动回落**。要改用兼容模式，先移除环境变量再重新启动产品。

## 认证与主机信任

- 产品 v0 **仅支持 agent**：先将服务端授权的密钥用 `ssh-add` 加入 Windows OpenSSH agent。
  `KeyFile`（包括未加密私钥）、`Interactive`、password、keyboard-interactive 和 ASKPASS 均不支持。
- 只有 `SSH_AUTH_SOCK` 以 `\\.\pipe\` 开头时才采用；否则使用默认
  `\\.\pipe\openssh-ssh-agent`。**永不使用 Pageant**，构建保留移除后端条目的补丁。
- 应用的 `configDir/ssh/known_hosts` 必须已预信任目标；可先在兼容模式中核对指纹并完成信任。
  Direct **只读**该文件，不写入、不 accept-new、不弹出首次信任提示。
- 握手偏好为**已知类型在前，其余支持的类型在后**；握手后按服务器实际出示的类型核对密钥。
  支持普通和哈希条目；`@cert-authority` / `@revoked` 标记报配置错误。
  未知类型/主机报 `HostKeyUnknown`，密钥变化报 `HostKeyChanged`，均失败关闭；按受信渠道核实后再更新信任。

## 能力与限制

- Direct 声明 `supportsOsc52Read=true`；实际读剪贴板仍须 `osc52.allowRead=true`（默认关闭）。
  Hyper-V 随内层传输；本地和兼容 SSH/Hyper-V 的 ConPTY 路径仍为 false。
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

Linux/macOS 只运行现有纯逻辑测试，不能替代 Windows 实连验收；v0 不增加 POSIX/Linux live harness。
独立 [A 探针](ssh-a-probe.md) 保留作依赖基线，不替代产品 API 验收。

全量构建默认运行 `it2w_third_party_notices`；若只构建了 `it2w_shell` 目标，打包前须再运行
`cmake --build build-mingw-direct --target it2w_third_party_notices`。
分发时携带 exe 旁 `licenses/` 中的三份上游原文与 `THIRD-PARTY-NOTICES.md`；
独立探针保留 `ssh-a-licenses/`。对应源码交付见 [UPSTREAM](../UPSTREAM.md#第三方静态依赖)。
