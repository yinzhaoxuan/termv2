# termv2 交接说明

日期：2026-11-03（2026-11-05 更新第 2、4、5、6 节）  
读者：仓库所有者，以及之后不再经过协调助手、直接改这个仓库的人。  
写入时 main 顶端：`87cdf84ab18a4fa8cf7cf7e0abfef4e46437b79c`（`fix(conpty): D-5 gate Reply ODS on OSC52 ping`）。本文只追加文档，不改行为。

termv2 是从 iTerm2 派生的 Windows 终端。本地会话走 ConPTY；SSH 默认走系统 `ssh.exe` + ConPTY（兼容模式）；可选的 Direct SSH 在进程内用 libssh2 直连，绕开 ConPTY。产品壳是 MinGW 构建的 `it2w_shell.exe`。

先读本文，再按需要打开文末索引里的原文。进度流水在 [STATUS.md](STATUS.md)。设计决策在 `docs/ADR-*.md`。与 iTerm2 的差距和 v0.1 待办在仓库根的 `REVIEW.md` 与两份 `REVIEW-增补-*.md`（不在 `docs/` 下）。v0.1 之后的能力提纲在 [ROADMAP-iTerm2-parity.md](ROADMAP-iTerm2-parity.md)。

**2026-11-05 更新（先读这段）：** 11-04 至 11-05 又落地了 DS-9（外观 Profile）、DS-13（按连接选择传输）、DS-14（连接编辑器）、ADR-004（Ctrl+C）、DS-15（诊断包导出）、DS-16（快捷键展示）、DS-10（OSC 52 读逐次确认）、NEW-05 余项、E7、E6（许可改为 GPL-3.0-or-later）、ADR-005（ABI 规则）、DS-17（内存账本）。现状看 [STATUS.md](STATUS.md) 顶部「当前状态」，剩余工作看 [TODO.md](TODO.md)，Windows 实机手测按 [WINDOWS-手测清单.md](WINDOWS-手测清单.md) 做（CI 会上传 `it2w_shell.exe`）。第 2 节环境变量、第 4、5、6 节已按 11-05 改写；第 3 节是 11-03 之前的落地记录，保留原样。

**两份增补里的勾选是当时的状态。** `REVIEW-增补-2026-11-02.md` 写于 `c1d475d..06497a0`，当时 GAP-01/14/15/16、A2–A4、A′、D-1–D-4 都还没落地。增补正文不改，文首的回写表与根目录 `REVIEW.md` §8 的勾选已经回写到 2026-11-05，以它们和 `git log` 为准。

哈希都来自 `git log` / `gh pr view` 的 merge commit，对应 main 上的那一笔提交。

---

## 1. 项目概览与目录结构

C++20，CMake ≥ 3.24（`CMakeLists.txt`）。产品名在工程里是 `iterm2_win`，可执行文件是 `it2w_shell`。配置与日志默认在 `%APPDATA%\it2w`。

| 目录 | 归属 |
|---|---|
| `ports/core/` | 终端核 **itcore**：VT 解析（`src/vt_parser.cpp`）、屏幕与 SGR（`src/screen.cpp`）、键盘/粘贴/鼠标编码（`src/encode.cpp`）、Unicode 宽度、搜索。C ABI 在 `include/`。上游对照在 `ports/core/SOURCE_MAP.md`（iTerm2 `f29420ae`）。 |
| `app/pane_backend/` | **窗格后端**。`pane_logic.*` 是平台无关逻辑（会话、滚轮、粘贴、关闭、环境变量开关）。`frame_pump.h` 是 pump / 收尾状态机。`win32_pane_backend.*` 把传输、itcore、D2D 接到 UI 线程。`pane_search.*` 是历史搜索。 |
| `platform/windows/` | **Windows 传输**。`common/` 快照、队列、SSH 会话映射、Hyper-V 解析、Direct 纯逻辑。`conpty/` ConPTY。`ssh/` 兼容模式（`ssh.exe` + 快照）。`ssh_direct/` 进程内 libssh2。`hyperv/` 解析后启动内层 SSH。`probe/` 是 `t03a_probe`、`ssh_a_probe`、`ssh_direct_accept`。 |
| `ui/` | **主界面与配置**。`shell_win32/main.cpp` 是 `it2w_shell`。`src/` 是 store、设置、工作区、校验。`include/uicfg`、`include/uishell` 是配置模型和窗格接口。单独 `cmake -S ui` 时用占位后端。 |
| `renderer/` | D2D/DWrite 渲染、布局、字形度量。静态库不链 itcore；实验窗和窗格后端再链真实核心。 |
| `app/win32_term/` | 最小 HWND 实验窗（itcore + ConPTY + D2D），不是产品壳。Linux 配置会跳过。 |
| `tests/` | 按层分：`tests/core`、`tests/app`（窗格逻辑）、`tests/platform`、`tests/renderer`，外加 `tests/golden`、`tests/vectors`。`ui/tests/` 是另一组 uicfg 单测，由 `ui/CMakeLists.txt` 登记。 |
| `docs/` | 状态、ADR、路线、身份、Direct SSH / A 探针、T03A 与 T09 规格。 |
| `cmake/` | `mingw-w64.cmake`、Direct 依赖（`SshDirectDeps.cmake`）、A 探针、许可拷贝、去掉 Pageant 的补丁。 |
| 仓库根 | `README.md`（构建入口）、`LICENSE`（GPL-3.0-or-later）、`REVIEW.md` 与两份 `REVIEW-增补-*.md`、`THIRD-PARTY-NOTICES.md`、`UPSTREAM.md`、`CMakePresets.json`、`.github/workflows/ci.yml`。 |

数据流：UI 选连接 → `PaneBackend` 选传输（空 connectionId 走本地 ConPTY；否则 SSH 或 Hyper-V）→ 字节进 `itcore_feed` → 快照交给 D2D。pump 在 UI 线程，状态是 Running → Draining → Finished（[ADR-001](ADR-001-pump状态机.md)）。

---

## 2. 如何构建与测试

工具链事实以根 `CMakeLists.txt`、`docs/ssh-direct.md`、`docs/ssh-a-probe.md`、`docs/STATUS.md`「构建提示」为准。

### Linux ctest

原生 Linux 不编 WIN32 目标（ConPTY、`it2w_shell`、Direct 传输）。能跑 itcore、窗格纯逻辑、SSH 快照 / Direct 逻辑、Hyper-V 策略、渲染纯逻辑。

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

原生配置时 `ui/CMakeLists.txt` 会 `find_package(nlohmann_json 3.11 REQUIRED)`。交叉编译才会自动把 `UICFG_NLOHMANN_INCLUDE_DIR` 指到 `third_party/nlohmann_json/include`（根 `CMakeLists.txt` 在 `add_subdirectory(ui)` 之前）。干净机器上若没有系统 nlohmann，原生 `cmake` 会在 ui 处失败。这就是 REVIEW GAP-11 / E4，尚未收口。临时做法是装 nlohmann 3.11，或自己传入 `-DUICFG_NLOHMANN_INCLUDE_DIR=<含 nlohmann/json.hpp 的目录>`。

CORE-01/02 向量默认路径是 `/workspace/qa/vectors/core01_02.json`（`ITCORE_VECTORS`）。文件不在时 CMake 只告警并跳过，不让配置失败。仓库内另有 `tests/vectors/extra.json`，总会登记。

本机有 `ssh` 时会加 `SSH-CFG-01.effective_G` 和 `T07.ssh_live`（超时 400s，串行）。实连凭据目录是 `IT2W_SSH_LAB`，默认 `/workspace/qa/ssh-lab`；目录不可读的用例会 SKIP。

ASan/UBSan：`-DIT2W_SANITIZE=ON`（要 Clang 或 GCC；MSVC 会直接报错）。测试目标不定义 `NDEBUG`，assert 在 RelWithDebInfo 里仍然有效。

### MinGW 交叉编译

真机包默认 `RelWithDebInfo`。工具链文件是 `cmake/mingw-w64.cmake`。

```bash
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-mingw -j
```

产物包括 `it2w_shell.exe`。实验窗是 `win32_term`，默认拉起 `C:\Windows\System32\cmd.exe`（[HISTORY-2026-10](HISTORY-2026-10.md)「构建提示」）。交叉编译配置阶段会跳过多数 `add_test`（`if(NOT CMAKE_CROSSCOMPILING)`），所以 MinGW 树不是跑 Linux ctest 的地方。

### Direct SSH 开关

开发构建默认 **`IT2W_SSH_DIRECT=OFF`**。只在 Windows/MinGW 上能编进产品。MSVC 和 POSIX 没有这条传输（`ssh-direct.md`、ADR-003）。

```bash
cmake -S . -B build-mingw-direct -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DIT2W_SSH_DIRECT=ON
cmake --build build-mingw-direct --parallel 4
```

首次构建会下载并编译固定版本的 OpenSSL 与 libssh2，SHA-256 强制校验。离线把 `SSH_A_OPENSSL_URL` / `SSH_A_LIBSSH2_URL` 指到相同源码包。依赖说明在 `THIRD-PARTY-NOTICES.md`。

产品 API 验收程序还要 `-DIT2W_BUILD_SSH_DIRECT_ACCEPT=ON`。它要求已经 `IT2W_SSH_DIRECT=ON`，并且是 WIN32 + MinGW；不会反过来把 Direct 打开。全量构建默认带上 `it2w_third_party_notices`；若只编了 `it2w_shell`，打包前再编这个目标，把 `licenses/` 放到 exe 旁。

独立 A 探针不编 UI 和产品传输：

```bash
cmake -S . -B build-mingw/ssh-a -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DIT2W_SSH_A_PROBE_ONLY=ON
cmake --build build-mingw/ssh-a --target ssh_a_probe --parallel 4
```

Linux 上只跑探针的可移植自测：`-DIT2W_SSH_A_PROBE_ONLY=ON -DIT2W_SSH_A_PROBE_SELFTEST=ON`（`docs/ssh-a-probe.md`）。

### 运行时环境变量

`envFlagOn`（`app/pane_backend/pane_logic.cpp`）只认**值恰好是 `1`**。`true`、`ON`、`01` 都是关。`ITERM2WIN_SSH_DIRECT` 改用 `sshDirectEnv`：恰为 `1` 或 `0` 才算设了，其他值视为没设。都在窗格构造时读一次，重连沿用。

| 变量 | 作用 |
|---|---|
| `ITERM2WIN_SSH_DIRECT` | 没写 `ssh.transport` 的连接的**全局覆盖**：`1` → 直连，`0` → 兼容，其他值或未设 → 交给 settings.json `sshDefaultTransport`，再交给内置默认（目前兼容）。写了 `"direct"` / `"compatible"` 的连接以连接为准（DS-13、ADR-006）。选中 Direct 而构建没编进它就明确报错（「此构建未包含直连 SSH。」加改法），**不回落**到 `ssh.exe`。 |
| `ITERM2WIN_OSC52_PING` | `1` 时打开 OSC 52 诊断：Reply 路径先发三段诊断再发真实应答；原始输出里 `]52;` 至多记 64 字节 hex。D-5 之后，ConPTY 写线程也只在这个开关下打 `OutputDebugStringA`（`[conpty] reply written=N`）。默认关。没有单独的 ODS 变量。 |
| `SSH_AUTH_SOCK` | Direct 只接受以 `\\.\pipe\` 开头的值，否则用 `\\.\pipe\openssh-ssh-agent`。不用 Pageant。 |
| `APPDATA` | 诊断目录。shell 未把 `configDir` 传进窗格时，用 `SHGetFolderPath(CSIDL_APPDATA)`，失败再读 `APPDATA`，得到 `%APPDATA%\it2w\logs\pane-diag.log`。两者都失败则不写。 |
| `IT2W_SSH_LAB` | 仅 Linux 的 T07 实连测试凭据目录。 |
| `ITCORE_VECTORS` | CORE-01/02 向量 JSON 路径。 |

PowerShell：

```powershell
$env:ITERM2WIN_SSH_DIRECT = '1'
.\it2w_shell.exe
```

诊断日志约 5 MiB × 3（`pane-diag.log`、`.1`、`.2`）。行里有主机名、主机密钥指纹和 agent 管道路径；要发给别人时用系统菜单「导出诊断包…」，自动脱敏并先预览（DS-15）。

---

## 3. 已实现清单

下面按模块列已经在 main 上的行为。提交是该模块对应的落地提交，不是全部历史。

### 3.1 Direct SSH v0（S0–S7）

决策：[ADR-003](ADR-003-ssh-direct.md)。用法：[ssh-direct.md](ssh-direct.md)。状态：[STATUS.md](STATUS.md) 2026-11-01 / 2026-11-02。

产品 v0 的边界（ADR-003 五项，Gavin 已批）：

- 方案 A：进程内 libssh2 + OpenSSL，用 SSH PTY / `window-change` 改远端尺寸。方案 B（纯管道 `ssh.exe`）只证明序列透明，远端 `stty size` 为 `0 0`，不做产品传输，也不做 A+B 混合。
- 只支持 agent。KeyFile、Interactive、password、keyboard-interactive、ASKPASS 都拒绝。
- `known_hosts` 只读，必须预先信任。不写入、不 accept-new、没有首次信任提示。未知主机 `HostKeyUnknown`，密钥变化 `HostKeyChanged`，失败即关闭。
- 握手偏好：已知类型在前，其余支持类型在后；按服务器实际出示的类型核对。
- 失败不回落 ConPTY。没有 ssh.exe 子进程。关闭预算 ≤ 3 秒。PTY 是 `xterm-256color`，网格至少 20×5，v0 像素尺寸填 0。
- 远端退出码 255 仍是 `RemoteExit(255)`，不套用 ssh.exe 的客户端错误语义。
- 字节透明不等于已经做了 tmux -CC 或 Sixel。没有 agent/X11/端口转发、ProxyJump、主机证书、keepalive。

| 阶段 | main 提交 | 内容 |
|---|---|---|
| A 探针（S0 之前的依赖基线） | `9c0a59b74052ecf63394ed6d687030986487b8f5` | `ssh_a_probe`（libssh2 静态）+ MinGW OpenSSL `FIND_ROOT`。gavin 上 `--test all` 六项 PASS（STATUS CAP-01）。探针不替代产品验收。 |
| S0 + S1 | `75f3c83fa14d7735d15055879c0caae392b6faa6` | 共享直连依赖 + `SshSessionTransport` 接口。零行为变化。 |
| S2 | `8bec5e97cca5f15fe2a8b0ceb5bce1f6330cf587` | 平台无关纯逻辑，未接线。Linux 测 `SSH-DIRECT-01.logic`。 |
| S3 | `27171243870fe5513ede33eb33f7b472ecc4ef27` | `DirectSshTransport`。当时还没接运行时开关。 |
| S4 | `ea4a0de0aeb2ea10581011f8102a2be9466b5c37` | `ITERM2WIN_SSH_DIRECT` 选择 Direct，失败不回落。 |
| S5 | `7a11351efd4d9ca70b1e86103b34ad123cf2d42e` | 产品验收载体 `ssh_direct_accept.exe` 与 StallingAgent。 |
| S7 | `0089edfc76ea539b840b8f2166c6a40061fbf48c` | `it2w_third_party_notices`、exe 旁 `licenses/`、第三方声明与使用文档。 |
| 诊断落盘（S6 基线） | `fde8e9932f34a28dcbf0b1d4b727aa76ce225b08` | `ssh.direct.*` 追加写入 `%APPDATA%\it2w\logs\pane-diag.log`。 |
| S6 收口（文档） | `06497a05b52d4095f2b2fdfaf4b83a9f08907df8` | 记录 gavin 的 `it2w_shell` 手测。代码手测本身不在 git 里。 |

S6 手测（STATUS 2026-11-02，证据在本机 QA 目录，**未入仓**）：

- `ssh_direct_accept.exe`：gavin PASS（dns-cancel、server-only-ecdsa 等有条件用例 SKIP）。
- `it2w_shell`，`ITERM2WIN_SSH_DIRECT=1`，目标 hc-lab。round1（lab 关机）：diag 有 `ssh.direct.*`，无 `ssh.exe`，连接超时；exit / resize / vim SKIP。round2（UbuntuDev 开着）：resize 104×34 → 71×24 → 115×40，vim PASS，exit 0，`protocol close`，hostkey ed25519，agent `\\.\pipe\openssh-ssh-agent`。CLEAN_OK。

普通 SSH 和 Hyper-V 的内层 SSH 共用这个传输选择。

### 3.2 CAP-01 与 OSC 52

`89854826744037ab0ca0d8f3fba1a5ccae11b9d5`：接受「经 ConPTY 的会话不支持 OSC 52 读」，写入能力门和 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。写仍可用，由 `osc52.allowWrite` 控制，默认关。

真机结论记在 ADR-002 / STATUS，原始 hex **未入仓**：

- 第 16 轮（gavin，Windows build 26300.9711，conhost 10.0.26100.9949，系统 ConPTY）：OSC 1337、OSC 133 透传；OSC 8 被改写（加 `id=`，BEL→ST）；DCS（tmux -CC）和 Sixel 被吞。由此重评直连。
- 第 17 轮：增量路径上 OSC 133;A 与提示符同位。整屏重绘未测。core 仍无 OSC 133 状态机（属 T04 之后的 Shell Integration）。
- 第 18 轮：方案 B 控制序列 79 字节逐字节 PASS（R18-1）；尺寸 `0 0` FAIL（R18-2）→ 产品必须方案 A。管道上看不到主机密钥 ask / 密码提示（R18-3）。

2026-11-03 起 Direct 也声明 `supportsOsc52Read=false`（见 D-1）。通道对读查询透明，应用层不应答。

### 3.3 窗格 pump（ADR-001，早于下面的 PR）

`7a901d4e22e9538e73a548fd8709366a2558418a`：NEW-01 / NEW-05 / OSC52-05。pump 不再嵌套 `DispatchMessage`。收尾由一次性定时器按预算推进。丢弃字节写入终态 `errorText`（「输出可能不完整，丢弃字节数=N」）。`ui/` 未加 `droppedOutputBytes` 字段。STATUS 记 macOS ASan 93/93、box ASan 114/114、Linux 114/114、TSan 113/113（跳过 PANE-11）。Windows 洪峰实测当时没跑。

### 3.4 2026-11-03：REVIEW 增补二与 A / A′ 批（PR #1–#10）

这些 PR 都已 MERGED，merge commit 就是 main 上的提交。

| PR | main 提交 | 事项 |
|---|---|---|
| [#1](https://github.com/yinzhaoxuan/termv2/pull/1) | `43850d0ac86c79cd92efa5317efbf5f1e00b68e6` | **D-1**：`DirectSshTransport::supportsOsc52Read()` 返回 false。Hyper-V 转发内层能力，走 Direct 时一并关闭。即使 `osc52.allowRead=true` 也不应答。 |
| [#2](https://github.com/yinzhaoxuan/termv2/pull/2) | `48e0a06f54f54835eaf6025116874831e43e9cbc` | **D-2 / GAP-01**：pane-diag 5 MiB × 3 滚动；行字段 `v/sid/attempt/phase/elapsed_ms/err/bytes/msg`；目录 `<configDir>/logs`。进程内单调 SessionId（从 1，重连不变）。快照名 `ssh-<pid>-<sid>-<aid>.conf`。单测 PANE-24。 |
| [#3](https://github.com/yinzhaoxuan/termv2/pull/3) | `7cc613d7aebf86943b90a6feeeab857d4688b325` | **GAP-14 / GAP-16**：SGR 38/48/58 子参数、下划线样式与上划线；DA1/DA2 改为 VT220；XTVERSION `termv2 0.1`；DA3 固定单元号；DECRQSS 与 XTGETTCAP 有应答。选择写在 [identity-14.3.md](identity-14.3.md)。单测 CORE-22、CORE-23。`ITCORE_ABI_VERSION` 仍是 0。 |
| [#4](https://github.com/yinzhaoxuan/termv2/pull/4) | `615dd383b4387e3f6d439265a3ade6b3d1c84954` | **D-3**：Hyper-V 主机密钥身份 `hv-<规范化 VM GUID>`。兼容模式快照写 `HostKeyAlias` 和 `CheckHostIP no`。Direct 用同一查找串，别名不加端口。不改 `ui/`，不开启 accept-new。 |
| [#5](https://github.com/yinzhaoxuan/termv2/pull/5) | `6a59765a685803fd2bbb884a1f1afe3c8789f9f1` | **D-4**：closer 报 `timedOut` 后，宽限 `kCloseTimeoutGraceMs`（1s）继续排空，期满结束 Draining，终态 Failed。未超时的关闭仍等到 Closed/Failed。单测 PANE-25。ADR-001 §10。 |
| [#6](https://github.com/yinzhaoxuan/termv2/pull/6) | `0356230f7a08d50bf6d04a9cdee6adaa099dda39` | **GAP-15 / A3**：启动前失败同步返回 `SnapshotError`（缺 `ssh.exe`、快照写失败、ConPTY 没拉起）。调用方在 `start` 返回前 `fail()`，状态已是 Failed，可以重连。Idle 上 `close` 直接 Closed，不再空等 5 秒。单测 PANE-26；HV-02 文案要在 Windows 上跑。ADR-001 §11。 |
| [#7](https://github.com/yinzhaoxuan/termv2/pull/7) | `b5e9ec1f8067e9d7aeb7be1a62b00a0895d9c4c1` | **GAP-02 / A2**：收尾排空已由 ADR-001 收口，这笔记的是单测 PANE-27（10×16 KiB，跨块 UTF-8，先关闭再只走定时器，全部喂入后才 EOF）。 |
| [#8](https://github.com/yinzhaoxuan/termv2/pull/8) | `a740590b32e3b0d73db10bc1e86c3ca419ac6c35` | **A4**：`NEED_DRAIN` 且 `accepted==0` 时先排空事件再重喂同一段。零进展上限 `kNeedDrainZeroProgressRetries`（8，不含第一次）。超过上限才计入 `droppedBytes`。单测 PANE-08、PANE-28。ADR-001 §12。 |
| [#9](https://github.com/yinzhaoxuan/termv2/pull/9) | `c84cec5c5c79d1f8d709753d60252cce791e6479` | **A′4**：`CSI ? 1007 $ p` 按真实状态回答（1 置位 / 2 复位）。初始复位，与 xterm `alternateScroll` 默认 false、iTerm2 `alternateScrollMode` 初始 NO 一致。DECSET/DECRST 可改；鼠标报告优先于 1007。单测 CORE-11、PANE-03。 |
| [#10](https://github.com/yinzhaoxuan/termv2/pull/10) | `87cdf84ab18a4fa8cf7cf7e0abfef4e46437b79c` | **D-5 的 ODS 部分**：ConPTY 写线程只在 `ITERM2WIN_OSC52_PING=1` 时记录 Reply 的 `OutputDebugStringA`。 |

路线图文档本身：`ef7f383c586588fa27b975b47f55734515ea97ef`（F-0..F-5 提纲）。增补二评审原文：`bcd7e9a19af73afcab0a0abb52453afbde7097a3`。

### 3.5 同一次落地里已经有、但容易被旧 REVIEW 当成未做的部分

- **A′1–A′3、A′5** 随 PR #3：CORE-22 / CORE-23 已登记。
- **A1 的 SessionId 与快照文件名**随 PR #2。启动时凭目录锁清理过期快照仍在 `ui/`，没做（`platform/windows/common/ssh_snapshot.h` 注释写明「这里不删残留」；STATUS D-2 同句）。
- **D8 的滚动日志**随 D-2 做了落盘和 5 MiB × 3。诊断包导出、脱敏工具没有。
- **搜索（REVIEW D2）**在更早的提交里就有纯文本搜索（SRCH-01..03）。正则留到 v0.2。Windows 上的大历史性能没测（ROADMAP §2.6）。

更早的 T05–T09（屏幕、编码、ConPTY、D2D、SSH 快照、PaneBackend、Hyper-V 后台）在 [HISTORY-2026-10](HISTORY-2026-10.md)「模块状态」里（10 月快照，部分已过时），这里不逐条重抄。Hyper-V **后台**在 `platform/windows/hyperv/`，主界面没有注入，见第 4 节 B1。

---

## 4. 未完成 / 计划

优先级按当前代码缺口，不按过期勾选框的原始顺序。每一条都标了出处。出处日期早于 2026-11-03 的，以「现在仍在」这句话为准，我对照过 main `87cdf84`。

### 4.1 先做：A5 端口越界校验（在 `ui/`）

出处：`REVIEW.md` §6「配置加载校验」、§8 A5。`REVIEW-增补-2026-10-31.md` §1 A5。`REVIEW-增补-2026-11-02.md` §0.3 与 §2 表仍写「未动」。2026-11-03 的 PR 没有改 `ui/`。

原文（REVIEW §6）：加载后直接当成功，只有保存时才 `validate`。非法端口在主 UI 里被强转成 `uint16_t`，例如 65558 会变成 22，悄悄连到另一个端口。应在加载时和启动前统一检查端口、超时、唯一 ID 和引用。

现状：`ui/shell_win32/main.cpp` 仍是 `o.target.port = (uint16_t)c->endpoint->port;`。逻辑在 `ui/src/store.cpp`、`ui/src/validate.cpp`。验收就是：越界端口报错，而不是截断后去连。

### 4.2 REVIEW B 类（主界面使用路径，P1）

出处：`REVIEW.md` §4 GAP-03..06、§7 批次 B、§8 B1–B5。两份增补都标成未动。`ui/shell_win32/main.cpp` 在 `87cdf84` 上仍是下面这几句。

| 项 | 原文要求 | 现在 |
|---|---|---|
| **B1** GAP-03 | 主 UI 注入 `resolveHyperV`；解析进度、多地址选择、「启动并连接」、可见的手动回退。普通连接不能暗中启动 VM。 | `makeFactory` 只赋 `resolveSsh`。Hyper-V 无备用地址时错误文本仍是「Hyper-V 地址解析尚未接入（T09），且未设置备用地址」。后台 `HyperVTransport` 和 D-3 别名已经在，缺的是注入和 UI。 |
| **B2** GAP-04 | local 路由贯通 executable、arguments、cwd、环境 inherit/set/unset。`cwd=null` 按设计映射到用户主目录。 | 只有空 connectionId 走本地，默认 ComSpec/cmd。保存的 PowerShell 配置双击走 `resolveSsh`，会得到「不是 SSH 连接」。`--command` 只作用于第一个空 ID 本地窗格（增补 10-31）。 |
| **B3** GAP-04 | store 完整保存和加载 `local.environment` 与 `closePolicy`。 | 增补 10-31：`ui/src/store.cpp` 加载不读 environment，保存写死 inherit/set/unset 和 closePolicy。 |
| **B4** GAP-05 | 工作区恢复为未连接占位，点击后才连接。设计 v0.3 §13.3，和 iTerm2 Arrangement 自动启动不同，以设计为准。 | 恢复标签后对引用有效的窗格立即 `startSession`。 |
| **B5** GAP-06 | 多行粘贴预览和确认；确认后只提交一次，不补回车；取消时发送零字节。设计要点另见增补 10-31 DS-6（触发条件、bracketed paste 时是否仍确认、「不再询问」的作用域）。 | 剪贴板读出后直接 `paste`。32 KiB 上限和 bracketed paste 编码已经有，不能代替确认。 |

B 批退出标准（REVIEW §7）：从主程序走通本地 PowerShell、手动 SSH、Hyper-V 发现并连接三条路径。

相关设计项还没写完：DS-2 统一启动描述（`resolve(connId) → variant<Local, Ssh, HyperV, Error>`）、DS-5 未连接占位态。出处：`REVIEW-增补-2026-10-31.md` §3。

### 4.3 其他项（2026-11-05 状态）

逐项勾选以根目录 `REVIEW.md` §8 为准（已回写到 2026-11-05），剩余工作与推荐顺序以 [TODO.md](TODO.md) 为准。概括：

- **已落地**：A 批全部（含 A5、A1 目录锁清理）、A′、B1–B5、D1 连接编辑器、D3 / D4 外观 Profile、D5 快捷键展示（只读）、D6（ADR-004）、D8 诊断包、E2 / NEW-02、E3 内存账本、E4 / E5（CI、预设、README）、E6 许可、E7 文档整理、NEW-03、NEW-04（DS-10）、NEW-05、DS-8 的 ABI 规则（ADR-005）、SSH 默认传输阶段 1（ADR-006）。设计文档都在 `docs/DS-*.md` / `docs/ADR-*.md`。
- **仍开放**：C1–C6 全部实机验证（[WINDOWS-手测清单.md](WINDOWS-手测清单.md)）；D2 搜索的正则与大历史耗时；D7 OSC 52 开关仍是 settings.json 隐藏字段、没有界面；E1 执行器 / UI 线程模型 ADR（需实测）；E8 GlyphCache 与连字；DS-7 的 TERM / terminfo 对齐（需实机）；E6 余项（MinGW 运行时许可文本打包）；D-5 的 QA 证据入仓。

### 4.4 D-5 余项

出处：`REVIEW-增补-2026-11-02.md` §1 D-5 三条。ODS 那条已由 PR #10 落地。剩下两条原文是：

- Direct 只能用 MinGW 构建，MSVC 产物里没有它。按连接选择传输已由 DS-13 完成（`ssh.transport`，环境变量降为全局默认）。
- CAP-01 第 16–18 轮和 S6 手测的证据都「留在本机 QA 目录，未入仓」。C6 要求结果绑定提交号和环境。建议至少把结果表（不含原始输出）入仓。

路径在 ADR-002 / STATUS 里写过、但不在 git 中：`/workspace/qa/realmachine-results-r16/`、`/workspace/qa/realmachine-results-r18/manual-r18.md`，以及 S6 的本机 QA 目录。A 探针产物同样不入仓。

### 4.5 Direct 改成默认传输之前

已由 [ADR-006](ADR-006-SSH默认传输.md) 决定，分两阶段。阶段 1 已落地：`auto`（agent → 直连）、settings.json `sshDefaultTransport`、环境变量 `1` / `0`、失败文字说明来源、发布包用 `mingw-direct`、CI 每次编 Direct。内置默认 `kBuiltinSshDefault` 仍是兼容。阶段 2 把它改成 `Auto`，前提是 ADR-006 §决策 4 的四道门槛都过了（编译、手测、一个工作日的实际使用、退回路径）。在那之前不要改 `kBuiltinSshDefault`。

### 4.6 Windows 实机待验

代码已在 main，下面这些还没有实机记录。完整的、按优先级排好的手测清单见 [WINDOWS-手测清单.md](WINDOWS-手测清单.md)，本表只留 11-03 之前就开着的几项。

| 待验 | 出处 | 期望 |
|---|---|---|
| DebugView 里的 ODS | STATUS 2026-11-03 D-5；ADR-001 §5 | 默认不刷 `[conpty] reply written=`。只有 `ITERM2WIN_OSC52_PING=1` 才有。vim 一类频繁查询不应再刷满 DebugView。开关打开时，OSC 52 Reply 的 `written=` 仍在。 |
| `vim -u NONE` 的 `v:termresponse` | STATUS GAP-16；`identity-14.3.md`；REVIEW GAP-16 验收 | `:echo v:termresponse`，并看 `:set t_RV?` 和启动时间，确认没有探测超时。DA 已改成 VT220 / 补丁号 0，就是为了让 Vim 保留下划线真彩色、不发尚未实现的 modifyOtherKeys。 |
| 波浪下划线等样式 | ROADMAP §2.1（状态 `[~]`）；STATUS GAP-14 | D2D 按样式位画双线、波浪、点线、虚线、上划线。Linux CORE-22 过了。实机还没画过波浪线。 |
| 洪峰 | ADR-001 §6 落地顺序第 5 步、§7 R1/R2、§9 第 2 条；增补二 §2「NEW-01 待 Windows 洪峰实测」 | PERF-01：洪峰中 MessageBox 约 0.25 s 弹出（验收目标，第 15 轮测，不作为已测项）。`cat 50MB; exit` 时打开链接确认框，尾部完整且终态 Exited。`yes` 洪峰中 Ctrl+Shift+W 确认关闭 100 次，不崩溃。结果写进 STATUS。 |
| pane-diag 的 Windows 路径 | STATUS D-2 | PANE-24 在 Linux 覆盖了滚动和字段。`%APPDATA%\it2w\logs` 没在写文档的环境里跑过。S6 手测证明当时有 `ssh.direct.*` 落盘，那是滚动字段改版之前的基线 `fde8e99`。 |
| 其余未实机清单 | [HISTORY-2026-10](HISTORY-2026-10.md)「未实机项」「T08 未实机项」「T09」 | ConPTY 全路径、IME、DPI、设备丢失、鼠标命中、窗体粘贴、Job 进程计数、关闭顺序、VM-01。Linux ctest 和 MinGW 交叉编译不能代替这些。 |

第 15 轮计划写在 STATUS 里的路径是 `/workspace/qa/r15-plan-draft.md`，不在仓库中。

### 4.7 ROADMAP F-0..F-5（v0.1 之前只做设计）

出处：[ROADMAP-iTerm2-parity.md](ROADMAP-iTerm2-parity.md) §1 与 §4。基线当时是 `bcd7e9a`。原文：「v0.1 收尾（REVIEW 的 A/A′/B/C 批）完成之前，本文的项目只做设计，不写实现。」

| 编号 | 主题 | 状态（路线图原文） |
|---|---|---|
| F-0 | 传输能力矩阵：把 `supportsOsc52Read` 推广成能力声明；不可用时 UI 要说明；重连改传输时能力怎么变 | `[~]` 有部分代码 |
| F-1 | Profile：配色、字体、光标、回滚、按键、启动命令、动态 Profile、`.itermcolors`、自动切换 | `[ ]` |
| F-2 | 行元数据：提示符/命令标记、时间戳、注释、图片引用；和搜索坐标的关系（NEW-03） | `[ ]` |
| F-3 | 图形存储与渲染：内联图片、Sixel、背景图的线程、内存和锚点 | `[ ]` |
| F-4 | 命令与动作系统：可绑定 Action；菜单、快捷键、触发器、脚本共用入口 | `[~]` 只有 settings.json 快捷键表 |
| F-5 | 会话持久与多窗口、标签拖出、Arrangement；与 GAP-05「恢复时不自动连接」一致 | `[ ]` |

路线图 §4 把实现排到 v0.2 以后：v0.2 基础是 F-0/F-1/F-4（Profile、配色导入、按键、Kitty 键盘、正则搜索）；v0.2 特色是 F-2（Shell Integration）；v0.3 是 F-3/F-5（图片与 Sixel、多窗口、热键窗口、触发器）；v0.4 才是 tmux -CC 和脚本 API。依赖 DCS 或 Sixel 的功能只能放在 Direct 上，本地 cmd/PowerShell 必须明确提示不支持（路线图 §0.1）。

REVIEW §8.F1–F13 是同一批能力的另一份清单（Shell Integration、通知、Kitty 键盘、tmux、WSL 等），同样标成 v0.2+ 各自立项。路线图说新差距追加到路线图正文，不另开清单。

---

## 5. 已知风险与设计决策索引

读 ADR 时以文末补充节为准。ADR-001 §1 的调用链是落地前的现状，用来解释为什么要改；现行规则在 §2 和 §10–§12。

### ADR-001 pump / 收尾状态机

| 节 | 内容 |
|---|---|
| §2 | 去掉嵌套派发；Running → Draining → Finished；各状态下 close / reconnect / search / resize；渲染与 UI 生命周期；保住洪峰时输入优先；收尾不丢尾部；丢弃字节进终态。 |
| §5 | OSC 52 诊断 D1–D7。D5 写线程在 2026-11-03 改为只在 `ITERM2WIN_OSC52_PING=1` 时输出。成功日志定稿是常开（§9.4），和 D5 的 ODS 不是同一条。 |
| §7 | 风险 R1–R10：定时器粒度约 15.6 ms、排空吞吐、QS_PAINT 饿死、模态框嵌套、剪贴板等待、I5 未证实、D6 会把 `PING` 写进远端 stdin（仅 env=1）、Draining 期间状态仍显示 Running 所以关闭仍会弹确认。 |
| §8 | 否决的备选：临时 `alive_` 止血、会话执行器（E1，推迟）、收尾一次喂完、`PostMessage` 续跑。 |
| §9 | 架构师定稿：Closed 后仍可能有 push，所以没结束就继续排空；0.25 s 只是验收目标；回调必须等 pump 栈退出。 |
| §10 | D-4 关闭超时后再给 1s。 |
| §11 | GAP-15：启动前失败不进 Draining，同步 `fail()`。 |
| §12 | A4：`accepted==0` 重喂，上限 8 次。 |

文首还写了两点推断：I5（Closed 之后读线程是否还会 push）没有逐行核对；0.25 s 在仓库文档里没有实测来源。

### ADR-002 OSC 52 读

| 节 | 内容 |
|---|---|
| 决策 | ConPTY 路径不支持读。Direct 曾从 D-1 起不声明读能力；DS-10 加了逐次确认后改为声明支持，每个读请求都要用户确认。`osc52.allowRead`（默认关）与 `supportsOsc52Read` 必须同时为真才会询问。 |
| 能力矩阵 | 写：三种 ConPTY 路径都受 `allowWrite` 控制，默认关。读：ConPTY 上 conhost 在输出侧吞掉 `52;c;?`。 |
| 第 16 轮 / 何时重评 | DCS 与 Sixel 被吞，直连重评已经触发。本地不做直连。 |
| 第 18 轮 | B 透明但没有尺寸；A 探针六项 PASS；产品已按 A 落地。 |
| 非目标 | 不捆 OpenConsole；不验证输入侧注入应答；不改 `ui/`。 |

安全原文在「已知限制」：OSC 52 读会把本机剪贴板交给终端里的程序。ConPTY 上请求到不了应用，所以不会弹提示，程序可能一直等到超时。

### ADR-003 Direct SSH

| 节 | 内容 |
|---|---|
| 决策 | 方案 A；构建默认 OFF，发布包用 `mingw-direct`；运行时按连接 > 环境变量 > settings.json > 内置默认选择（DS-13、ADR-006）；v0 仅 agent；未知主机经首次信任界面写入 known_hosts（DS-12），不 accept-new；不做 POSIX 实连；agent 只用 OpenSSH 管道。 |
| Hyper-V 主机密钥身份 | `hv-<规范化 VM GUID>`；兼容模式 `HostKeyAlias` + `CheckHostIP no`；Direct 查找串与 OpenSSH 一致，别名不加端口。 |
| 能力与边界 | `supportsOsc52Read=true`，读请求逐次确认（DS-10）；关闭 ≤3s；不增加 DCS/Sixel 旗标；像素尺寸 0；序列透明不等于渲染能力。对应源码见 `UPSTREAM.md`。 |

### ADR-006 SSH 默认传输

| 节 | 内容 |
|---|---|
| 决策 | 没写传输的连接：环境变量（`1` / `0`）> settings.json `sshDefaultTransport` > 内置默认；`auto` = 构建含 Direct 且 agent 认证时直连。静态选择，失败不回落。 |
| 阶段 | 阶段 1（已落地）内置默认仍兼容；阶段 2 改成 `auto`，门槛见 §决策 4。 |
| 可见性 | 直连非连接写明时失败文字说明来源；编辑器「跟随全局默认（当前：…）」；诊断 `ssh.transport`。 |

### ADR-004 Ctrl+C / ADR-005 ABI 规则

- ADR-004：Ctrl+C 默认「有选区复制并清除，否则发 0x03」，与 Windows Terminal 一致；settings.json 里 `"copyOrInterrupt": ""` 退回纯中断。
- ADR-005：`ITCORE_ABI_VERSION` 只在不兼容变化时加 1（当前 0）；`ABI-02.layout` 用编译期断言锁住公开结构体布局与枚举值，改动它们时测试会失败，提交信息要写明兼容性。

### 使用时会碰到的行为

- 选了 Direct 又连不上时，不会自动改走 `ssh.exe`。要兼容模式就在编辑器里把该连接的传输改成「兼容」，或设 `ITERM2WIN_SSH_DIRECT=0` / settings.json `"sshDefaultTransport": "compatible"` 后重启（`ssh-direct.md`）。直连不是连接写明的时候，失败文字会说明它从哪来。
- OSC 52 读只在 Direct 上能收到，`osc52.allowRead=true` 时每次弹框确认；ConPTY 路径收不到请求，远端 nvim / vim 若靠 OSC 52 读粘贴会超时，请用终端自己的粘贴（Ctrl+Shift+V 或右键）。
- `ITERM2WIN_OSC52_PING=1` 会把诊断序列写进远端 stdin（ADR-001 R8）。默认关着。
- 诊断日志含主机名、指纹、agent 路径；对外发送用「导出诊断包…」（DS-15）。
- 输出被丢弃时，`SessionStatus.droppedOutputBytes` 有值，状态栏显示「输出不完整」；`errorText` 里的文字仍保留。
- 内存：单会话超过 128 MiB 或合计超过 512 MiB 时自动压低回滚历史；合计接近 512 MiB 时拒绝新建窗格（DS-17）。
- TERM：本地会话继承父进程环境，SSH 取决于 Windows OpenSSH 默认值。代码里没有统一设置 `TERM`。Direct 的 PTY 类型是 `xterm-256color`。DA 身份、TERM、terminfo 三者还没在实机上对齐（REVIEW GAP-10，DS-7）。

---

## 6. 接手建议

1. **先做 Windows 实机验证。** 11-04 以来的功能都还没在 Windows 上跑过，`mingw-direct` 也一次没编译。按 [WINDOWS-手测清单.md](WINDOWS-手测清单.md) 从 P0 做起，结果写进 STATUS，绑定提交号与 Windows 版本（诊断包里有）。
2. **不要把 Direct 改成默认**，直到上一步的 Direct 相关项通过，并按 TODO §2.1 写了 ADR。
3. **剩下的开发项**见 TODO §2.2：E1、E8、DS-7 需要实机；E6 余项依赖构建机。F-0..F-5 只写设计（ROADMAP）。
4. **工作方式**：先在 `docs/DS-*.md` 写设计，再实现；平台无关的逻辑放 `ui/src`（uicfg）、`app/pane_backend/pane_logic.*`、`renderer/layout/` 并单测，Win32 文件只做薄胶水；CI 的 mingw 是唯一的 Windows 编译检查。改完回写 STATUS 顶部与 TODO，提交信息写清行为、测试结果与「未验证」项；动了 itcore 公开头文件要按 ADR-005 写明兼容性。

构建自检：Linux 上 `ctest`（注意 nlohmann 和 CORE-01/02 向量）；要给 Windows 用的包用 MinGW 交叉编译；要手测 Direct 用 `mingw-direct` 预设（或 CI `mingw-direct` 任务的产物，每次推送都生成），在连接里写 `"transport": "direct"` 或运行时设 `ITERM2WIN_SSH_DIRECT=1`。
