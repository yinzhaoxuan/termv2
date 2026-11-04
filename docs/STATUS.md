# iterm2-win 进度汇总

交接说明见 [HANDOFF.md](HANDOFF.md)（目录、构建、main 上已落地的提交，以及仍开放的项）。本文件是进度流水；根目录 `REVIEW*.md` 的勾选框停在 2026-11-02，晚于该日期的勾选以交接文档第 3、4 节为准。

## 2026-11-04：DS-16 快捷键展示（D5）

设计：[DS-16-快捷键展示.md](DS-16-快捷键展示.md)。

- 系统菜单「快捷键…」：只读列表，26 个可配置动作（中文名 / 当前快捷键 / 状态：默认、已修改（附默认值）、未绑定、冲突（指出对方）、无法识别、未知动作），下方列出会话树 F2 / Delete / 菜单键等固定按键；「打开配置目录」按钮。编辑仍放 v0.2+。
- 模型 `uicfg::keybindingRows` / `fixedKeys` 可移植并单测；冲突规则与 `validateSettings` 一致。ListView 的样式在代码里设置，`.rc` 保持只引 `<windows.h>`。
- macOS ctest 154/154（新增 uicfg_keybinding_* 8 项）。
- **未验证**：`keybinding_dialog.cpp`、`main.cpp`、`.rc` 等 CI `mingw`；Windows 手测见 DS-16 §4。

## 2026-11-04：DS-15 诊断包导出与脱敏（D8）

设计：[DS-15-诊断包导出.md](DS-15-诊断包导出.md)。基线 `0b72f82`。

- 系统菜单「导出诊断包…」：收集 → 脱敏 → 预览（只读，等宽）→ 用户点「保存…」后写成一个 UTF-8 文本文件（默认「文档」目录，`it2w-diag-YYYYMMDD-HHMMSS.txt`，原子写入）。
- 内容：构建与环境（配置时提交号 `IT2W_BUILD_COMMIT`、Release/Debug、MinGW、Direct 是否编译、Windows 版本、两个诊断环境开关）、配置摘要（只有计数与开关）、`pane-diag.log.2 / .1 / 当前` 各最后 5 MiB。
- 脱敏（`uicfg::redactDiagnosticText`，每条规则一个函数）：先按配置里的主机、用户、VM、路径以及 Windows 用户名、主机名、用户目录、configDir 精确替换（长的优先、边界检查、少于 3 个字符不替换），再按模式替换 `SHA256:` 指纹、GUID（含 `hv-`）、IPv4、IPv6、非默认 agent 管道、`X:\Users\<名字>`。同值同记号（`<host-1>` 等）。
- macOS ctest 146/146（新增 uicfg_diag_* 共 14 项，含 60 个已知值的 15 MiB 大包用例）。
- **未验证**：`diag_dialog.cpp`、`main.cpp`、`.rc` 未在本机编译（等 CI `mingw`）；Windows 手测见 DS-15 §7。

## 2026-11-04：D6 Ctrl+C 默认行为（ADR-004）

- 决定保留 `copyOrInterrupt` 为 Ctrl+C 默认（有选区复制并清除，否则发 0x03，与 Windows Terminal 一致），写入 [ADR-004](ADR-004-Ctrl+C默认行为.md)。退路：settings.json 里 `"copyOrInterrupt": ""`。
- 修复：`validateSettings` 把空快捷键当错误，导致按文档解绑（`copyOrInterrupt` / `pasteCtrlV` 设为 `""`）的用户之后保存 settings.json 失败（改字号、导入配色）。空字符串现为合法的「不绑定」，不参与冲突检查。
- macOS ctest 132/132。

## 2026-11-04：DS-14 连接编辑器（D1）

设计：[DS-14-连接编辑器.md](DS-14-连接编辑器.md)。基线 `d638d60`。

- **会话树右键菜单**（另有 F2 / Delete / 菜单键）：连接、编辑、删除；新建连接、新建 / 重命名 / 删除分组。connections.json 只读时除「连接」外置灰并说明原因。
- **编辑器对话框**（`.rc` 里的 DIALOGEX 模板，代码在 `ui/shell_win32/connection_dialog.cpp`）：SSH 手动地址 / Hyper-V / 本地程序三种类型，可选分组、外观 Profile（DS-9）、SSH 传输（DS-13）。Hyper-V「选择…」在后台线程列出虚拟机与网卡。校验失败时红字提示并把焦点放到对应控件，对话框不关。
- **不丢数据**：编辑器管不到的字段原样保留（环境变量、closePolicy、addressFamily、未改动的非法 transport 原文）；含换行或空字符串的参数设为只读。
- **外部修改保护**：启动时记下 connections.json 的存在 / 大小 / 修改时间，保存前比对，不一致就拒绝并提示重启（`saveConnectionsChecked`）。
- 改了外观 Profile 的连接，已打开的窗格立即换色；其他改动对之后新建 / 重连的窗格生效。
- macOS ctest **132/132**（新增 uicfg_connection_form_{roundtrip,fields,creation,groups,menus,prompts,stamps,arguments}）。
- **未验证**：`connection_dialog.cpp`、`main.cpp`、`.rc` 未在本机编译（等 CI `mingw`；`.rc` 的 UTF-8 中文是主要编译风险）；Windows 手测见 DS-14 §7（GAP-07 路径：新建 Ubuntu 连接 → 保存 → 连接 → 编辑 → 再次连接）。

## 2026-11-04：DS-13 按连接选择 SSH 传输（增补二 D-5）

设计：[DS-13-按连接选择传输.md](DS-13-按连接选择传输.md)。基线 `6db9448`。

- connections.json 的 `ssh.transport`：`"compatible"`（ConPTY + `ssh.exe`）或 `"direct"`（进程内 libssh2），Hyper-V 内层 SSH 同样适用。写明的值优先；省略时跟随环境变量 `ITERM2WIN_SSH_DIRECT`（降为全局默认）。**默认值不变**：两者都没有时仍走兼容模式。
- 加载校验：未知值或非字符串 → `ssh.transport` 错误；`"direct"` 配 KeyFile / Interactive → `ssh.auth.mode` 错误。两者都只阻止该连接启动，不让文件变 Corrupt。
- 传输偏好经 `SshLaunchDesc` → `it2w::SshLaunchOptions.transport` 带到后端，每次 attempt 由 `wantDirectSsh` 选择；构建不含 Direct 时照旧失败、不回落。两条报错文字改为指向连接字段。
- macOS ctest 124/124，CI 全绿（含 mingw）（新增 uicfg_ssh_transport_{store,validate,launch}、PANE-30）。
- **未验证**：Windows 手测见 DS-13 §6；改了 `win32_pane_backend.cpp`，需要手动触发一次 CI `mingw-direct`。

## 2026-11-04：DS-9 最小外观 Profile（D3、D4）

设计：[DS-9-外观Profile.md](DS-9-外观Profile.md)。基线 `2cc8b40`。

- **颜色只有一个来源**：渲染器每帧从快照取核心的生效色（`itcore_snapshot_colors` → `itren::themeFromColors`），变化时整屏重画并更新光标画刷。以前渲染器用写死的主题、不读核心：OSC 4/10/11/12 改色在屏幕上不生效，OSC 10/11 查询回答的颜色也与屏幕不符。
- **settings.json `appearanceProfiles`**：前景、背景、光标、16 色、`scrollbackLines`（100–1000000）、`memoryLimitMiB`（8–1024），字段都可省略。连接按 `appearanceProfileId` 引用，先套 `appearance-default` 再逐字段覆盖；不存在或非法的 Profile 回退默认并在状态栏提示一次（不阻止启动）。未知键与非法原值原样写回。
- **默认值统一**：内置默认 = 升级前屏幕上的配色 + 10000 行 / 64 MiB（= 核心默认）。`PaneBackendConfig` 删去写死的 5000 / 32 MiB 与 `colors`，改为 `resolveAppearance` 回调；每个新核心（含重连）都下发配色。
- **导入 `.itermcolors`**：系统菜单「导入 iTerm2 配色…」「恢复默认配色」，写入 `appearance-default`，已打开窗格经 `PaneBackend::setColors` 立即换色。自写 XML plist 子集解析（拒绝二进制 plist、限 1 MiB / 8 层）；` (Dark)` 后缀回退；P3 转 sRGB，Calibrated 按 sRGB 近似；选区色等不支持的键在确认框里列出。
- macOS ctest **120/120**，CI 全绿（`6db9448`，含 mingw）（新增 uicfg_appearance_{settings,resolution,mutation}、uicfg_itermcolors_{sample,parsing,prompt}、PANE-29.appearance_colors）。
- **未验证**：`main.cpp`、`win32_pane_backend.cpp`、`renderer.cpp` 未在本机编译，等 CI `mingw`；Windows 手测见 DS-9 §7（默认外观不变、导入后实时换色、OSC 11 设置/查询/恢复、按连接引用 Profile、`scrollbackLines: 200` 生效）。

## 2026-11-04：E4/E5、NEW-02、NEW-03、DS-12 Direct 首次信任

- **E4 / E5**（`299b3c1`）：ui 默认用仓库内 nlohmann；`IT2W_REQUIRE_VECTORS`；`IT2W_SANITIZE_THREAD`；`CMakePresets.json`（dev / asan / tsan / mingw / mingw-direct）；根 README；GitHub Actions（linux / asan / tsan 测试 + mingw 交叉编译，mingw-direct 仅手动触发）。首次 CI 全绿，`986cdf7` 的 Windows 文件经 mingw 编译通过。
- **NEW-02**（`c69bc48`）：`itcore_history_rows` 改为按组增量计数，push / 淘汰只置脏首尾组，查询时惰性重排。5000 行历史再输出 10 万行：整段重建 100000 → 0 次（197.9 s → 0.19 s）；单条 10 万段软换行行：157.7 s → 1.6 s。
- **NEW-03**（`2fcaac3`）：核对确认没有地方跨 feed 缓存 `abs_row`；`itcore.h` 写明它只在同一时刻有效；新增淘汰后高亮回归测试。
- **DS-12**（设计 [DS-12-Direct首次信任.md](DS-12-Direct首次信任.md)）：Direct 遇到未知主机时窗格给出「核对指纹…」，确认框默认取消，确认后把本次出示的密钥原子写入 `configDir/ssh/known_hosts` 并重连一次。密钥变化只追加提示，不给信任按钮；DSS 可照常校验已信任的条目，但不提供信任。Direct 默认值不变。
- macOS ctest 113/113。**待验证**：Direct 代码只在 `IT2W_SSH_DIRECT=ON` 时编译，需要在 GitHub Actions 手动触发 `mingw-direct`；Windows 手测见 DS-12 §6。

## 2026-11-04：A5、A1 余量、DS-2、B1–B5（`986cdf7`）

设计：[DS-2-启动描述与B批.md](DS-2-启动描述与B批.md)。基线 `a06bbf6`。

- **A5**：加载 `connections.json` 时逐条校验（端口、超时 1–600、ID 为空或重复、分组不存在、组 ID 重复）。非整数或越界的端口/超时记成 0 交给校验报错，不再让整个文件变成 Corrupt，也不再截断成别的端口。无效连接仍显示，启动时报「连接配置无效（字段）：说明」，状态栏提示条数。
- **A1 余量**：拿到配置目录锁后，`cleanupStaleSnapshots` 删除 `ssh/snapshots` 下残留的 `ssh-<n>-<n>[-<n>].conf`（只删普通文件，不跟随符号链接，不进子目录）。
- **DS-2**：`uicfg::describeLaunch` 统一给出 Local / SSH / Hyper-V / Error 四种启动描述；主界面的 `resolveLocal`、`resolveHyperV`、`resolveSsh` 都由它派生。
- **B2 / B3**：保存的本地连接（如 PowerShell）真正拉起，传入程序、参数、工作目录（null → 用户主目录）和环境 inherit/set/unset；程序或目录不存在时同步 Failed，可重连。store 完整读写 `environment` 与 `closePolicy`（未知值原样保留，校验拒绝启动）。
- **B1**：主界面注入 `resolveHyperV`，不再把 `fallbackHost` 当手动地址直连。解析中标题为「正在解析 Hyper-V 地址…」。`SessionStatus.recovery` 以结构化字段给出「启动并连接」（VmNotRunning）或「选择地址…」（AddressAmbiguous，带 adapterId），覆盖只作用于这一次重连。普通连接不会启动 VM。
- **B4**：恢复工作区的窗格全部为「未连接」占位，点「连接」、按 Enter 或单击才启动；首次运行仍自动开本地终端。
- **B5**：粘贴文本含换行时先预览确认（最多 10 行 × 120 码点，控制字符显示为 `^X`）；确定只发送一次、不补回车，取消发送零字节。隐藏设置 `confirmMultilinePaste`（默认 true）。
- macOS 本机 ctest **107/107**（含 PANE-11；新增 uicfg_snapshot_cleanup_tests、uicfg_launch_tests、uicfg_compose_environment_tests）。
- **未验证**：`ui/shell_win32/main.cpp`、`win32_pane_backend.cpp` 没有在本机编译（无 MinGW），需要 MinGW 交叉编译后再在 Windows 手测：从连接树启动 PowerShell（空格和中文参数、显式/null cwd、环境 set/unset）、程序或目录不存在的错误；Hyper-V 运行中 / 已关机 → 启动并连接 / 多候选 → 选择地址 / 回退地址标题；恢复四个 SSH 窗格后 `ssh.exe` 进程数为 0；多行粘贴确认与取消；启动时残留快照被清掉。

## 2026-11-03：REVIEW 增补二 D-5

- ConPTY 写线程在协议 Reply 写成功后不再无条件调用 `OutputDebugStringA`。
  只在既有 OSC 52 诊断开关 `ITERM2WIN_OSC52_PING=1` 时记录 `[conpty] reply written=N`
  （与窗格 `envFlagOn` 相同：值恰好为 `1`，写线程入口读一次）。默认关闭，vim 一类查询不再刷满 DebugView。
  开关打开时这段日志仍在，便于对照 OSC 52 的 Reply 写入。未另设环境变量。

## 2026-11-03：REVIEW A4 NEED_DRAIN 零进展

- `itcore_feed` 返回 `NEED_DRAIN` 且 `accepted==0` 时，pump 先 `afterFeed` 排空事件，再重喂同一段未读字节，偏移不前进。
  连续零进展重试上限是 `kNeedDrainZeroProgressRetries`（8，不含第一次 feed），Running 与 Draining 共用 `PumpBudget::finalRetries`。
  超过上限仍拒收才把这一片计入 `droppedBytes` 并跳过；会话结束时沿用「输出可能不完整，丢弃字节数=N」。
  `accepted>0` 仍只前进已接收的部分。见 [ADR-001](ADR-001-pump状态机.md) §12。单测在 PANE-08 与 PANE-28。

## 2026-11-03：REVIEW A′4（?1007 Alternate Scroll）

- `CSI ? 1007 $ p` 按真实状态回答 `CSI ? 1007 ; Pm $ y`（1 置位 / 2 复位），不再回 0。
- DECSET/DECRST `?1007` 控制备用屏滚轮是否合成上/下方向键。鼠标报告（9/1000/1002/1003；Shift 仍强制本地）优先于 1007。
- **初始复位。** xterm 的 `alternateScroll` 资源默认 false；iTerm2 的 `alternateScrollMode` 在 `commonReset` / `softReset` 里是 NO（3.6.9 的 DECRPM 也是 reset）。iTerm2 另有“自动打开备用屏滚轮”的配置项，默认同样是关。这里没有那项配置，所以模式位跟上游一样从复位开始，不再把翻译写死为开。程序用 `?1007h` 打开。DECSTR 与 RIS 回到复位。复位后备用屏滚轮走原来的视口滚动，不合成方向键。
- 单测 CORE-11、PANE-03。

## 2026-11-03：REVIEW A2 / GAP-02 收尾排空

- 已由 [ADR-001](ADR-001-pump状态机.md) 收口，本轮不改状态机。`finishSession` 只进入 Draining 并登记续跑；`itcore_finish_input` 要等传输 Closed/Failed，且队列和 carry 都空。
- 故障丢弃仍把字节数写进终态（「输出可能不完整」），不把剩余输出当成已经 EOF。
- 单测 PANE-27：10×16 KiB，跨块 UTF-8（U+4F60）和尾标记。先关闭，再只走定时器排空；全部喂入后才 EOF，标记和汉字都在。丢弃路径仍报告不完整。

## 2026-11-03：REVIEW A3 / GAP-15 启动前失败

- 兼容模式 `ssh.exe` 不存在时，`SshTransport::start` 只回调 `onFinished(SshNotFound)` 并返回 `nullopt`。窗格把 `nullopt` 当成已启动，而 `finishSession` 只认 Closed/Failed，于是一直停在 Starting。
- 现在启动前失败都同步返回 `SnapshotError`：客户端不存在（`ClientMissing`）、快照文件写失败、ConPTY 没拉起（`Launch`）。调用方在 `start` 返回前 `fail()`，状态已是 Failed，重连仍可用。直连不查找 `ssh.exe`，配置错误仍是 `Kind::Snapshot`，前缀不变（「SSH 配置无效（直连）：」）。
- 文案示例：`未找到 ssh.exe：<路径>。请在 Windows「可选功能」中安装 OpenSSH 客户端，或在连接设置中填写 ssh.exe 的完整路径。` 本地 shell 拉起失败会带上程序路径。
- Hyper-V 内层 `start` 失败同样返回错误并发 Failed；窗格若仍在 Starting 且已有 `HvAction::Fail`，在这一轮回调里发布 Failed，不再等排空定时器。没有这条 Fail 的失败（含直连在 Running 之前失败）仍走原来的 `finishSession`。`HvFailure::None` 不再编成 `None: ` 前缀。
- 这种失败没有拉起进程，ConPTY 停在 Idle。重连的关闭线程会 `close` + `waitClosed`；`close(Idle)` 现在直接标 Closed 并置位 `closedEvent_`，不再先等 5 秒再无限等。已经跑起来的关闭路径不变。
- 见 [ADR-001](ADR-001-pump状态机.md) §11。Linux 单测 PANE-26 与 `SSH-CFG-01` 的文案；HV-02 增加缺客户端 / 直连快照前缀，需 Windows 运行。

## 2026-11-03：REVIEW 增补二 D-4

- closer 上报 `timedOut`（「关闭超时，已强制结束」）后，传输仍可能停在 Closing。
  排空不再一直等 `pumpInputEnded()`：宽限 `kCloseTimeoutGraceMs`（1s）内按预算继续喂输出，
  期满丢弃尚未喂入的字节、结束 Draining，并撤掉 8 ms 续跑定时器。
  终态为 Failed，`errorText` 带超时原因（有丢弃时附带字节数）。
  未超时的关闭仍等到传输 Closed/Failed 再收尾。见 [ADR-001](ADR-001-pump状态机.md) §10。单测 PANE-25。

## 2026-11-03：REVIEW 增补二 D-3（HostKeyAlias 管道）

- Hyper-V 的主机密钥身份改为 `hv-<规范化 VM GUID>`，不再只用解析出的 IP。
  DHCP 换地址后原先两种模式都会 `HostKeyUnknown`：Direct 按 IP 查找只读的 `known_hosts`，兼容模式快照又不写 `HostKeyAlias`，重新信任只记住新 IP。
- `HyperVTransport` 在启动内层 SSH 前写入 `SshTarget::hostKeyAlias`（手动回退地址同样用 VM 别名）。
  兼容模式 `-F` 快照包含 `HostKeyAlias`，`HostName` 仍是当前地址，并写 `CheckHostIP no`。
  Direct 的 `knownHostTypesFor` 与 `libssh2_knownhost_check` 使用同一查找串：有别名时为别名原文（不加端口），否则维持 `[host]:port`。
- 不改 `ui/`，不开启 accept-new。首次信任界面（展示指纹并确认后写入）仍是 Direct 转为默认的前置条件，留在本变更之外。
  已按 IP 信任的虚拟机需在兼容模式再确认一次。见 [ADR-003](ADR-003-ssh-direct.md)、[ssh-direct.md](ssh-direct.md)。

## 2026-11-03：REVIEW A′ GAP-14 / GAP-16

- GAP-14：SGR 38/48/58 的冒号形式在子参数不少于 5 个时跳过色彩空间（`38:2::R:G:B` 与 `38:2:0:R:G:B` 都是红，不再读成绿或丢掉）。分号形式的 58 与 38 一样消费 `;5;N` 和 `;2;R;G;B`；分量被截断时吃掉已有数字，不再漏成 SGR 2/0。59 清下划线色。`4:0..5`、21、53/55 写入样式位与上划线位；D2D 按这些位画双线、波浪、点线、虚线和上划线。`ITCORE_ABI_VERSION` 仍是 0（`flags` 里原本空着的位，结构没变）。
- GAP-16：DA1/DA2 统一为 VT220（`CSI ? 62 ; 22 c`、`CSI > 1 ; 0 ; 0 c`），不再宣告 VT520 / xterm 2500。XTVERSION 是 `termv2 0.1`。DA3 回固定单元号，不抬高级别。DECRQSS（`m` / `r` / `"q` / `SP q`）和 XTGETTCAP 未知项都有应答，不再沉默。选择记在 [identity-14.3.md](identity-14.3.md)。
- Linux：CORE-22、CORE-23。实机 `vim -u NONE` 的 `v:termresponse` 还没跑。DECSLRM 没做。?1007 见文首 A′4。

## 2026-11-03：REVIEW 增补二 D-2 / GAP-01

- pane-diag 按 DS-11 滚动：`pane-diag.log` 约 5 MiB，另保留 `.1` 与 `.2`（共 3 个文件）。进程内只保持一个写句柄，滚动或更换目录时才重新打开。
- 每行字段：`v`（格式版本）、`sid`、`attempt`、`phase`、`elapsed_ms`、`err`（没有错误码时为 `-`）、`bytes`、`msg`。
- 目录是 `<configDir>/logs`。`PaneBackendConfig::configDir` 非空时用它。shell 尚未传入该字段（本轮不改 `ui/`），因此使用与 shell 相同的默认配置目录 `%APPDATA%\it2w`：先 `SHGetFolderPath(CSIDL_APPDATA)`，失败再读 `APPDATA`。两者都失败则不写。
- 每个窗格分配进程内单调 SessionId（从 1 起）。重连保持同一个 sid，attempt 加一。兼容模式快照名为 `ssh-<pid>-<sid>-<aid>.conf`，避免多窗格互撞，也避免上次崩溃留下的 `ssh-1-1.conf` 挡住新会话。启动时按目录锁清理过期快照仍在 `ui/`，本轮未做。
- 诊断行可能含主机名、主机密钥指纹和 agent 管道路径，导出前需自行脱敏。
- Linux 单测 PANE-24 覆盖滚动、字段和 SessionId。Windows 落盘路径未在本环境运行。

## 2026-11-03：REVIEW 增补二 D-1

- Direct SSH `DirectSshTransport::supportsOsc52Read()` 改为 `false`，与 ConPTY 相同。
  逐次读确认 UI 做出之前，即使 `osc52.allowRead=true` 也不应答 OSC 52 读。
  Hyper-V 转发内层能力，因此走 Direct 时一并关闭。
  见 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)、[ADR-003](ADR-003-ssh-direct.md)、[ssh-direct.md](ssh-direct.md)。

## 2026-11-02：Direct SSH S6 产品手测收口（Asia/Shanghai）

- 基线：`fde8e99`（窗格诊断含 `ssh.direct.*` 追加写入 `%APPDATA%\it2w\logs\pane-diag.log`）。
- `ssh_direct_accept.exe`：此前 gavin PASS（dns-cancel / server-only-ecdsa 等需条件用例 SKIP）。
- `it2w_shell` Direct 手测（gavin，`ITERM2WIN_SSH_DIRECT=1` → hc-lab）：
  - round1（lab 关机）：`pane-diag.log` 写有 `ssh.direct.*` PASS；无 `ssh.exe` PASS；connect timeout；exit / resize / vim SKIP。
  - round2（UbuntuDev 已开）：resize PASS（`tput`/`stty` 104×34→71×24→115×40，diag `window-change`）；vim PASS；exit PASS（退出码 0，`protocol close`）；diag 复核 hostkey ed25519、agent `\\.\pipe\openssh-ssh-agent`、pty/shell/running；CLEAN_OK。
- 证据留在本机 QA 目录，未入仓。Direct SSH v0 产品手测（含诊断落盘）收口。

## 2026-11-01：Direct SSH v0 / S7

- S0–S5 已落地：共享固定依赖、传输接口/纯逻辑、进程内 DirectSshTransport、双开关与产品 API 验收载体。
  产品 v0 仅 agent、known_hosts 只读预信任，失败不回落；决策见 [ADR-003](ADR-003-ssh-direct.md)。
- S6：`ssh_direct_accept.exe` 验收用例此前 PASS；`it2w_shell` 产品手测见上方 **2026-11-02** 收口。
- S7：本次变更补齐 `it2w_third_party_notices` 默认构建目标、exe 旁 `licenses/`、
  [第三方声明](../THIRD-PARTY-NOTICES.md) / 对应源码说明和 [使用文档](ssh-direct.md)。
- Direct 的 OSC 52 读当时与 `osc52.allowRead` 策略门组合；2026-11-03 D-1 起改为 `supportsOsc52Read=false`（见文首）。ConPTY 路径不支持读，证据保持见 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。


## 2026-10-31：NEW-01 / NEW-05 / OSC52-05

- 实现 [ADR-001](ADR-001-pump状态机.md)：pump 不再嵌套派发消息或使用重入守卫；
  Running → Draining → Finished 通过一次性续跑定时器按预算推进。
  finish_input 只在队列/carry 空且读线程已结束后执行；回调经独立消息与 attempt 令牌隔离。
- final 拒收与传输队列丢弃合计进入后端终态数值字段、现有 errorText 与 OutputDebugString 日志；
  不修改 `ui/`，接口后续需求登记在 `（架构师工作区 ui-needs：SessionStatus 后续加 droppedOutputBytes）`。
- OSC52 增加读入口、跳过、拒绝、成功（常开）及 Reply 写入诊断；唯一诊断开关
  `ITERM2WIN_OSC52_PING=1`，默认关闭。启用时同一 Reply 路径发送三段诊断后再发送真实应答，
  原始输出 tap 对 `]52;` 记录至多 64 字节 hex。
- macOS（Codex）ASan/UBSan：**93/93**（402 s）；box 复核：ASan **114/114**（372 s，含 PANE-11 洪水关闭 370 s）、
  Linux **114/114**、TSan **113/113**（跳过 PANE-11）、MinGW 交叉编译 `it2w_shell.exe` 成功。无 sanitizer 报错。
- 未运行：Windows 真机（第 15 轮计划见 `/workspace/qa/r15-plan-draft.md`）；
  live SSH 排除；CORE-01/02 外部向量缺失按既有 CMake 行为跳过。


更新时间：2026-10-23（Asia/Shanghai）

本文件跟踪从核心/传输到最小 Win32 窗的提交进度，以及**尚未在真实 Windows 机器上验证**的项。

## 最近提交（摘要）

| 提交 | 内容 |
|------|------|
| `9d7f716` | T05 屏幕模型 Grid/Cell/Scrollback；CSI 默认参数与 SGR；快照 ABI；CORE-03/04/05、MEM-01 子集 |
| `aab3c7e` | T06+T05 评审：`encode_*`、D2D/DWrite 原型、宽字孤儿与 `wrap_pending` 组合、快照 `generation`、ConPTY `failStart` 清 reader |
| `66f10dc` | P0 第 2 批：DA2、CSI t 窗口查询与标题栈、OSC 1 图标标题、OSC 4/10/11/12/104/110–112 配置色 ABI；CORE-20/21 |
| `c8e9264` | T06 集成：`app/win32_term/` 最小 WIN32 实验窗串联 itcore + ConPTY + `itrender_d2d`；`docs/STATUS.md` |

## 模块状态

### 已落地（可在 Linux 单测 / mingw 交叉编译门闸）
- **CORE-06/07/08 Unicode 宽度 / OSC 8 / 快照池引用**（2026-10-23）
  - `ports/core/tools/gen_width_table.py` 从 UCD 16.0.0（可 `--ucd-dir` 离线）生成 `src/ucd_width_table.inc`；`ucd::width` 覆盖 EAW W/F、组合符、Default_Ignorable、Extended_Pictographic、emoji 修饰、VS16（仅主屏）、RI 对、ZWJ 序列；对齐 upstream iTermCharacterWidth.c / StringToScreenChars。
  - OSC 8：URL+id 入 grapheme pool（`\0H` 前缀），cell 携带 hyperlink id；新 ABI `itcore_snapshot_hyperlink`；URL 上限 2097152，空 URL 结束链接，SGR 0 不结束。
  - 快照不再复制 pool：`PoolRef{shared_ptr<PoolStore>, limit, epoch}`，分块追加、release/acquire 发布；clear 换新 store 并 ++epoch。
  - 测试：`itcore_test_unicode`（width/osc8/pool，含并发读线程）；Linux ctest 57/57；mingw 全量构建通过（Windows 上未实际运行）。

- **itcore（ports/core）**
  - VT 解析、屏幕模型、SGR/网格、scrollback 预算
  - `itcore_encode_text/paste/key/mouse`（DECCKM、应用键区、1000/1002/1003+1006、bracketed paste ≤32KiB）
  - 快照 ABI：`revision` / `generation` / `mouse_mode` / grapheme
  - 宽字孤儿修复、`wrap_pending` 组合回写末非 CONTINUATION
- **平台层（T03A）**
  - `it2w_platform_common`：SSH 快照过滤、命令行/环境块、输入调度与输出水位（Linux 单测）
  - `ConPtyTransport`：start / enqueue / tryPop / resize(generation) / close；`failStart` 等待后 `CloseHandle(reader_)`
  - `t03a_probe`：最小读写驱动（仅 WIN32）
- **渲染（T06 原型）**
  - `GridMetrics`（DPI 取整、最小 20×5）
  - `ISwapChainHost` + `D2DRenderer`（脏行、caret_rect、预编辑覆盖、`resize_surface`）
  - 静态库不链 itcore；测试用 mock，实验窗链真实 itcore
- **app/win32_term（本提交）**
  - 单 HWND：键盘/`WM_CHAR` + IMM32 IME 提交 → encode → `enqueueUserInput`
  - 输出 `tryPopOutput` → `itcore_feed` → 定时/事件驱动快照 → `present_frame`
  - resize：`GridMetrics::grid_size` → **先** `itcore_resize(gen)` **再** `transport.resize(gen)` → `resize_surface`
  - 关闭：`close(User)` + `waitClosed` → 排空 → `finish_input` / `destroy`
  - CMake：`if(WIN32)`；Linux 配置跳过；尽量 mingw 交叉编译

### 有意未动

- **`ui/`**：本轮不覆盖、不改布局与既有代码（连接/工作区等 UI 路径保持原状）。

## 本轮（渲染评审 + T07）

- **渲染评审 5 点**（`renderer/d2d`）：颜色与脏行复用渲染的 `renderer/layout/cell_style.h`（`resolve_cell` / `DirtyRows`），持久画布只重画脏行、覆盖层（光标/预编辑）每帧画在后缓冲；粗体/斜体（4 个 TextFormat 变体）+ 下划线（`underline_color`）；设备丢失 → `recover_device()`；generation 不一致 → `frame_policy.h::LayoutKeeper` 保留旧布局、只画交集、其余填背景。单测 `renderer_frame_policy`。
- **SSH 快照 A4 修复**：Interactive 增加 `IdentityFile none` / `IdentitiesOnly yes` / `IdentityAgent none`；三种模式都输出 `CertificateFile none` / `ControlMaster no` / `ControlPath none`。
- **T07**：`platform/windows/common/ssh_session.*`（`ssh -G` 解析与生效值校验、255/远端退出码与错误映射、输出尾部去转义）；`platform/windows/ssh/ssh_transport.*`（落盘快照 → ConPTY 中 `ssh.exe -F <快照> <alias>` → `onFinished(SshExitInfo)`）。测试：`T07.ssh_map.unit`、`SSH-CFG-01.effective_G`、`T07.ssh_live`（本机 sshd 127.0.0.1:22422：未知主机 / 主机密钥变化 / 拒绝连接 / 错误密钥 / 交互对仅公钥服务器）。授权密钥、密码（22423）、带口令密钥用例在 `/home/it2wcli/lab` 对 box 用户不可读时自动 SKIP。
- **mingw 全量构建**：交叉编译时 uicfg 使用 `third_party/nlohmann_json/include`（独立路径，不再引入 `/usr/include`），工具链加 `CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY`；`cmake --build build-mingw` 全目标通过。
- 已知限制：ssh 退出瞬间仍在输出队列中、未被 UI 取走的错误文本不计入尾部，可能映射为 Unknown；后续改为 Transport 内部旁路复制。

## T08 PaneBackend（uishell/pane_backend.h 实现，架构师）

- `app/pane_backend/pane_logic.*`（平台无关，Linux 单测 PANE-01）：bell 1 次/秒限流、标题 100ms 节流、最小网格 20×5、resize generation 追踪、UI 派发队列（空→非空唤醒）、`SessionCloser`（每会话关闭线程）、`DragSelect`（拖选手势）。粘贴清洗已移入核心。
- `app/pane_backend/win32_pane_backend.*`：`it2app::makeWin32PaneBackendFactory(PaneBackendConfig)`；空 connectionId→ConPtyTransport（%ComSpec%），否则经 `config.resolveSsh`（仅创建时调用一次）得 SshLaunchOptions→SshTransport。attach 建子窗 + HwndSwapChainHost + D2DRenderer；setBounds 先 itcore_resize(gen) 后 transport.resize(gen)；传输回调只捕获共享队列并投递，PaneEvents 仅在 UI 线程（WM_APP+0x41 / 50ms 定时器）派发。
- 传输改动：`ConPtyTransport::beginPaste(n, bracketed)` 透传（原先恒为 bracketed）；新增 `activeProcessCount()`（Job 基本记账 ActiveProcesses，加锁防 job_ 关闭竞态）；SshTransport 对应转发。

### T08 评审修复（R1–R4，对齐 UI 头 783435d）
1. **关闭不阻塞 UI**：`requestClose` 把关闭交给本会话的关闭线程后立即返回。该线程调 `transport.close(User)`，由传输层 closer 执行 T03A §1.4 的 9 步和 3 s 强杀，再 `waitClosed(5 s)`。结果 post 到 UI 队列，UI 线程 `finishSession`（排空输出、finish_input、最终快照）后发 `onStatus(Exited)`，超时发 `onStatus(Failed, "关闭超时，已强制结束")`。传输若在 `waitClosed` 超时后仍停在 Closing，D-4 再给 1s 排空宽限，期满结束 Draining 并发布这条 Failed（见文首）。超时后线程继续等传输真正结束，并在该线程上析构。析构函数先 close 队列，再把传输交给关闭线程（若尚未开始），只释放自己的引用，**不 join、不等待**。多窗格各自一条线程，并行关闭。单测 PANE-01：立即返回、只经 drain 送达、8 个会话并行（≈单个耗时）、超时路径、关闭中销毁不回调。
2. **粘贴**：直接 `itcore_encode_paste`，由核心按 DECSET 2004 加标记并清洗 NUL 与内嵌起止标记（7 位 / C1，循环删除），已删除 `bracketedFromProbe`/`buildPaste`/`sanitizePasteBody`。核心返回 -2 或传输返回 TooLarge 时调 `onPasteRejected(bytes, 32768)`。单测 ENC-02。
3. **`itcore_mode_query(core, mode)`** → `ITCORE_MODE_NOT_RECOGNIZED/SET/RESET`（0/1/2，与 DECRPM Pm 一致），仅 DEC 私有模式，覆盖 1 4（DECSCLM，仅记录） 7 9 25 47 1000 1002 1003 1006 1007（Alternate Scroll，初始复位） 1047 1049 2004。`CSI ? Ps $ p` → `CSI ? Ps ; Pm $ y`。单测 CORE-11。ANSI 模式另用 `itcore_ansi_mode_query(core, mode)`（4 IRM、20 LNM），保持原函数签名以稳定 C ABI；DEC ?4 与 ANSI 4 互不影响，CORE-13 覆盖。
4. **选区在核心**：`itcore_selection_start/extend/clear/active/text`。端点存为 (逻辑段 id, 字符序号)，跟随滚动进历史、历史按宽度重排和 resize。起点被历史裁掉时夹到最早一行；切主/备用屏、RIS、ED 3 时清除。复制规则：软换行拼接，硬换行出 `\n`，行尾空白去掉，宽字符只出一次，WIDE 换行补的空格不算内容。快照里选中格带 `ITCORE_CELL_SELECTED` 并翻转 INVERSE，选区变化会 touch 全部行，触发脏行重绘。后端：本地模式（无鼠标报告或按住 Shift）下，移到另一格才开始拖选；单击清除选区并检查链接。`selectedText()`/`clearSelection()` 已实现；选区有/无变化（含被输出清掉）时发 `onSelectionChanged`。单测 CORE-12。
- 剩余 UI 接口需求：见下“接口问题”。

### T08 第二轮（对齐 UI 头 a44c631）
1. **选区按行取 revision**：start/extend/clear 以及被输出清掉（切屏、RIS、ED 3）时，`sel_touch(old)` 把新、旧选区覆盖到的网格行都从 Screen 全局计数器取新 revision，不再 touch 全部行。全局计数至少推进一次，所以可见历史行（快照里 revision 取当前计数）也会重画。CORE-12 增加 `DirtyRows` 拖选用例：起点、扩展、回缩、清除、ED 3 清除、视口位于历史中，每一步只有受影响的行变脏。
2. **关闭线程生命周期**：
   - 新增 `SessionShared`，包含队列、pumpPending、传输对象和关闭结果，是窗格与后台之间唯一共享的状态。
   - 关闭线程只持有 `shared_ptr<SessionShared>`，结果写进共享状态并 post 一个空任务唤醒 UI；`drainUi()` 领取结果后调 `onCloseDone`。
   - 传输回调只捕获 `weak_ptr`，投递 `postTo<Win32PaneBackend>` 任务（`drain(this)` 时才得到窗格引用），不捕获 this。
   - wake 只捕获 HWND，在队列锁内调用，close 时清空。
   - 析构顺序：`ev_ = {}` → 关闭队列 → 交出传输。
   - `SessionCloser::shutdownAll(3s)` 使用故意泄漏的全局登记表（活跃数 + 条件变量）。超时返回 false，线程保持 detach，由 Job `KILL_ON_JOB_CLOSE` 兜底。对外入口是 `it2app::shutdownPaneBackends(ms)`。
   - 单测 PANE-02：8 个窗格创建后立即销毁，循环 100 次，期间有并发投递；验证 shutdownAll 后活跃线程为 0，以及卡住线程时的超时路径。ASan/UBSan、TSan 下全部 65 项通过。
3. **视口**：
   - `scrollViewport(lines)` 截断到 [0, 历史行数]；`scrollToBottom()`；`sendInput`/`paste` 先回到底部。
   - 上翻时历史增长会同步增大偏移，视图停在原处。
   - 快照用 `first_row = -offset` 取历史行，光标移出视口时不可见。
   - `onViewportChanged(offset, history)` 在偏移或历史行数变化时触发。
   - 新 ABI `itcore_history_rows`。
   - `onRedrawRequested` 相关代码已删除。
4. **鼠标**：
   - 滚轮每格 3 行。鼠标模式下转发 WHEEL_UP/DOWN（Shift 强制本地）；备用屏且 ?1007 置位、无鼠标模式时发上/下方向键；否则滚动视口。
   - 双击选词：词字符为字母数字、非 ASCII 以及 `/-+\~_.`（上游默认），其余标点单独成词。
   - 三击选逻辑行（跨软换行）。双击/三击之后的拖动按同一单位扩展，新 ABI 为 `itcore_selection_start_unit`。
   - 拖出上下边缘时，每 50ms tick 自动滚动 1 行。
   - `pane_logic` 中的 `Viewport`、`ClickCounter`、`routeWheel` 由 PANE-03 覆盖。
5. **DECRQM ANSI**：`CSI Ps $ p`（不带 ?）与私有模式分开处理，回复 `CSI Ps;Pm$y`。4（IRM）和 20（LNM）回 1 或 2，其余回 0；ANSI 25 与 `?25` 互不相干。新实现 LNM（LF/VT/FF 兼回车）。单测 CORE-13 覆盖 2004、1049、25 的开和关，`?9999`→0、`9999`→0，以及 4 和 20；LF/VT/FF 在 LNM 开和关时的列位置、Enter/Alt+Enter 编码均有对照。Viewport::scroll 用 int64 截断，PANE-03 覆盖 INT_MIN/INT_MAX。

### T09 Hyper-V 辅助（实现）
- `platform/windows/common/hyperv_resolve.*`（平台无关）：VM ID 规范化（仅 GUID）、查询 JSON 解析（v=1，标量/数组、BOM）、地址筛选（排除 0.0.0.0、127/8、169.254/16、::、::1、无 scope 的 fe80::/10）、候选选择、`HyperVResolver` 状态机与 `runHyperVResolution`（轮询 2 s、单次查询 5 s、预算 60 s；只在策略允许时启动一次；多候选不盲选；VmNotFound 不回退，AccessDenied/模块缺失/启动失败可回退手动地址）。
- `platform/windows/hyperv/`：固定 PowerShell 脚本（-EncodedCommand，VM ID 走环境变量）+ Job/超时/取消；`HyperVTransport` 后台解析后启动内层 SshTransport，并写入 `HostKeyAlias=hv-<vmId>`（D-3；首次信任界面仍未做）。
- 后端：`PaneBackendConfig::resolveHyperV`，失败文本为 `<类别>: <说明>` 写进 errorText；连接后标题为 “Hyper-V <地址>” 或 “手动地址 <地址>”。
- 测试 HV-01.resolve_policy。实机（VM-01）未跑。

### PaneBackend 新接口（对齐 UI 头 5397adb / 00f1574）
- `sendKey(vk, mods)`：`planKey`（PANE-04）决策——VK_PROCESSKEY 忽略；Ctrl+Alt 且 ToUnicode 有可打印字符（AltGr）按文本走 sendInput；其余交 `itcore_encode_key`（DECCKM/DECKPAM/xterm 修饰键，Alt+字符 ESC 前缀、Ctrl+字母控制字符由核心按 base 字符生成）。
- `reconnect()`：旧 attempt 用 `itcore_export_text` 归档逻辑文本（§15.4：每 Pane 一个、≤8 MiB、尾部保留、不切断 UTF-8，CORE-19），旧传输交给旧共享状态上的关闭线程（旧队列先 close），新 core + 进程，attempt +1（传入 AttemptId）。`previousAttemptId()` / `previousAttemptText()` 返回归档。
- 门禁：mingw 全量构建含 it2w_shell（以 HEAD 的 ui/ 验证；工作区里 UI 未提交的 main.cpp 当前编不过，非本提交范围）。

### P0 序列（第 1 批：屏幕本地 + 快照 ABI）
- SCS `ESC ( / ESC )` + SO/SI，DEC 特殊图形映射；DECSC/DECRC/?1048 一并保存字符集（CORE-14）。
- 制表位：HTS、TBC 0/3、CHT `CSI I`、CBT `CSI Z`，resize 扩展默认每 8 列，RIS 复位（CORE-15）。
- SGR 2/5/6/8/9 与 22/25/28/29：`ITCORE_CELL_DIM/BLINK/HIDDEN/STRIKE` 占 bit 6–9，与既有 bit 0–5（含 SELECTED=5）无冲突，沿用渲染临时位；`#define ITCORE_HAS_SGR_EXT_FLAGS`（CORE-16）。
- ?5 DECSCNM：`itcore_mode_query(5)` 可查，切换推进全部行 revision，`itcore_snapshot_reverse_screen`；DECSCUSR Ps 0–6 → `itcore_snapshot_cursor_style`；?1004 / ?2026 可置位可查（CORE-17）。
- DECALN `ESC # 8`、DECSTR `CSI ! p`、DEC 私有 DSR `CSI ? 6/15/25/26/53/55 n`（原先解析后不执行）（CORE-18）。

### P0 序列（第 2 批：应答 + 标题 + 配置色）
- DA2 `CSI > c` → `\e[>64;2500;0c`（上游 VT100Output.m，emulation level 500）。
- CSI t：18t 恒应答 `8;rows;cols`；14t/16t 仅在后端调用 `itcore_set_cell_pixel_size` 后应答；22/23t 窗口/图标标题栈（各上限 10，23t 重发标题事件）；21t/20t 回报标题有注入风险，不实现；8t 等改窗口尺寸忽略（CORE-20）。
- OSC 0/1/2：新事件 `ITCORE_EV_ICON_TITLE = 6`（OSC 0 同时发 TITLE 与 ICON_TITLE；ABI-01 已更新）。
- 配置色：`itcore_colors` + `itcore_default_colors`（xterm 调色板）/`itcore_set_colors`/`itcore_snapshot_colors`；OSC 4、10/11/12 设置与 `?` 查询（应答 `rgb:rrrr/gggg/bbbb`，统一以 ST 结尾），OSC 104、110/111/112 恢复配置；变更全行失效 + RENDER_INVALIDATED；RIS 恢复配置色并清标题栈（CORE-21）。
- 第 3 批（已完成）：?1004 焦点事件（FocusReporter，CSI I/O）；?2026 暂停快照（SyncGate，超时 ITREN_SYNC_TIMEOUT_MS = 150 ms 兜底出帧，DECSTR/RIS 关闭后立即出帧）；D2D 接 cursor_style（DECSCUSR 形状/闪烁/失焦空心框/块光标反色）、reverse_screen（DECSCNM 整屏重画）、SGR 5/9（text_visible / strike_rect）、fit_glyph（advance 缓存，只缩不放）、ime_layout（预编辑逐格排布、子句下划线、候选窗锚点）、selection_hit（拖选宽字符吸附、按越界距离自动滚动）。
- 渲染评审修复 1–6（已完成）：SGR 5 按行跟踪（`BlinkRows`，相位翻转只标脏含闪烁格的行，闪烁全部消失后 `wants_animation()` 为 false）；`caret_rect_px` 用 `composition_anchor`（组字起点，不随组字右移）；预编辑宽度与簇切分走核心 `itcore_text_clusters`（新 C ABI，与 itcore_feed 同规则）；下划线随闪烁隐藏、位置粗细按度量与 DPI（`underline_rect`）；光标在宽字符尾格先吸附首格再判宽（`cursor_cell`）；拖选端点（含自动滚动）按 `SelHit::boundary` 换算，右半算进选区（`selection_hit_autoscroll` / `drag_endpoint_col`）。pump()/present() 的喂数据与出帧闸门抽成 `app/pane_backend/frame_pump.h`（`pump_core` / `frame_due`），SYNC-02 端到端单测走同一段代码。回归：BLINK-01/02、IME-01/02、CUR-01、SEL-01、SYNC-02(e2e)。
- ?2026 出帧规则（6f34a78 评审修复）：present 前比较 `itcore_sync_end_count`（?2026l / DECSTR / RIS 各计一次），变化即出帧并重新计时——覆盖 tmux/neovim 同一批内“关→内容→开”；超时（150 ms）后只是每 150 ms 出一帧，**不清**核心的同步状态；改窗口尺寸绕过 SyncGate 强制出帧。
- 未接：GlyphCache 图集与 plan_runs 多 face 回退——当前走 DrawText（DWrite 自带回退），需改为 glyph run 路径后再接，留第 4 批。
- 提案（第 4 批）：运行时改字体/字号。`GridMetrics::from_font(实测 advance/行高)` 取代硬编码 7.2×10 → on_config 时：重建 DWrite 格式并清 advance 缓存 → 重算 GridMetrics → `itcore_set_cell_pixel_size` → grid_size 得新列行 → ResizeTracker 出 generation → `itcore_resize(gen)` → `transport.resize(gen)` → 渲染按 expected generation 保持旧布局直到快照对齐。DPI 变化走同一路径。

### 接口问题（未改头文件，待 UI 定夺）
8. T09：UI 无“启动并连接”按钮、多地址选择器、“正在解析/等待 VM 地址”状态；目前分别以 Failed 文本（VmNotRunning / AddressAmbiguous 含候选列表）和 Starting 状态表达。
1. “UI 翻译按键”与子窗焦点冲突：子窗把 WM_KEY*/WM_CHAR SendMessage 给父窗，IME 留在子窗处理。
2. **UI 须在退出时调用 `it2app::shutdownPaneBackends(3000)`**：在所有窗格销毁之后、WinMain 返回之前调用。头文件里没有对应的跨平台入口，建议在 `PaneBackendFactory` 旁边加一个 shutdown 钩子。
3. Shift+PgUp 等滚动快捷键由 UI 调 `scrollViewport`。后端不处理键盘滚动，因为按键会转发给父窗。
4. SSH 下 hasForegroundJob 无法看到远端，Running 期间恒为 true（仅供参考）。
5. attach 在会话启动失败时仍返回 true（状态 Failed）。
6. 词分隔符目前写死在核心，尚无配置接口。LNM 下回车键已发 CR LF：编码器 VK_RETURN 看 lnm，后端 sendInput 对恰为 "\r"/"\x1b\r" 的输入按 itcore_ansi_mode_query(20) 补 LF（UI 自行翻译按键，只认精确的回车输入，属已知限制）。
7. `itcore_history_rows` 每次都会重排历史（O(历史)），只在有输出时调用。大历史下的性能留作后续缓存项。

### T08 未实机项
子窗交换链/DPI、IME 候选窗定位、焦点与按键转发、Job 进程计数（conhost 是否在 Job 内影响 >1 判定）、关闭顺序与多窗格并行关闭耗时、拖选与选区高亮、链接单击、SSH 实连；滚轮（精密触摸板小 delta 累加）、双击/三击时序、边缘自动滚动手感、视口偏移下的光标/IME 位置。

## CAP-01 P06（已定）

经 ConPTY 的会话**不支持 OSC 52 读**；写正常。能力门与措辞见 [ADR-002](ADR-002-CAP01-P06-osc52-read.md)。

第 16 轮真机（gavin，build 26300.9711 / conhost 10.0.26100.9949，系统 ConPTY）：S1 OSC 1337 / S2 OSC 133 透传；S3 OSC 8 被改写（加 `id=`、BEL→ST）；**S4 DCS（tmux -CC）与 S5 Sixel 被吞** → **直连 SSH 须重评**（已触发）。

第 17 轮（对齐 harness，`cmd.exe /k prompt $e]133;A$e\$P$G`，5 回车 + `dir`）：增量绘制下 7 个 `]133;A` **均同位**（紧跟提示符文字，未提前）；标记与提示符文字偶发分两次 ReadFile（约 14–19 ms），字节流连续。**未测** resize/`cls`/滚出底边整屏重绘。core 尚无 OSC 133（属 T04）；`?9001` win32-input-mode 忽略、`?1004` 焦点已支持。

第 18 轮（Windows → Linux SSH，OpenSSH_for_Windows 9.5p2，纯管道无 ConPTY；产物 `/workspace/qa/realmachine-results-r18/`，未入仓）：
- **R18-1 PASS**：DCS（含无 ST）、Sixel、OSC 52 查询/写入共 79 字节与远端 `printf` **逐字节一致**；无 conhost 改写、无 `?9001` 注入。相对第 16 轮 ConPTY 吞 DCS/Sixel/`52;c;?`，方案 B 对控制序列**透明**。
- **R18-2 FAIL（尺寸）**：远端 `stty size` 恒 **`0 0`**；`~` 转义无 resize；`COLUMNS`/`LINES` 与真控制台 `mode con` 再重定向均无效 → **方案 B 无法初始化/中途改远端 TTY 尺寸，产品直连须方案 A（进程内 libssh `window-change`）**。
- **R18-3**（gavin，不变）：首次主机密钥 ask 与 password 提示不到管道 → 默认路径预信任 known_hosts（v0.3 不做 accept-new）+ 公钥；密码与首次 ask 需 `SSH_ASKPASS`（实现期再定）。
- **Windows A 探针**（进程内 libssh2 1.11.1 + OpenSSL 3.5.9 静态 MinGW）在 **gavin** 上 `--test all` 六项 PASS（agent auth、79B 序列、首条 stty、33×window-change、close-flood、close-auth CancelSynchronousIo）；产物不入仓。立场：产品直连以 A 为主。

## 未实机项（须 Win11 + 真 ConPTY / D3D 验证）

以下在 Linux 上只能做到**编译门闸**或纯逻辑单测，行为以规格为准，**不能**用交叉编译结果代替实机：

1. **ConPTY 全路径**：`CreatePseudoConsole` / Job 归属 / 管道读写 / `ResizePseudoConsole` / 关闭时序与超时 fault（见 `docs/T03A-Windows规格-v0.1.md`）。
2. **win32_term 端到端**：cmd.exe 启停、键盘与中文 IME（IMM32 提交吞 `WM_CHAR`）、帧延迟与脏行 Present、Per-Monitor V2 DPI 变化后行列收敛。
3. **D3D/D2D 设备丢失与重建**：`present_frame` 遇 `DEVICE_REMOVED/RESET/RECREATE_TARGET` 置位，下一帧 `recover_device()` 整链重建并经 `host->create_swap_chain` 新建交换链（已实现，未实机触发验证）。
7. **渲染评审 5 点的视觉效果**：画布脏行重绘、粗体/斜体/下划线、generation 不一致保留旧布局——逻辑已单测，像素结果须实机截图。
8. **T07 SshTransport**：Windows 自带 OpenSSH 在 ConPTY 中的启动、主机密钥/密码/口令提示交互、快照路径（盘符/UNC/中文）与退出码映射须实机；Linux 侧仅以 OpenSSH 10.0 验证映射与 `-G` 生效值。
4. **鼠标报告**：encode_mouse 已单测；窗体命中 → 模式开启时上报尚未接到 win32_term。
5. **粘贴事务**：`beginPaste` / bracketed / 32KiB 上限在传输与 encode 侧有测；窗体 Ctrl+V / 剪贴板未接。
6. **WinUI / 正式 UI 壳**：当前仅为 HWND 实验窗，非正式产品窗。

## 构建提示

```bash
# Linux 本机（跳过 WIN32 目标，跑 itcore / 布局单测）
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# mingw-w64 交叉（产出 t03a_probe、win32_term、itrender_d2d 等）
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-mingw -j   # 全量通过
```

实机：将 `win32_term.exe` 与依赖（若非 `-static`）拷到 Win11，直接运行；默认启动 `C:\Windows\System32\cmd.exe`。
- VS16 按屏幕（itcore_text_clusters 取 core 当前屏，alt 屏 VS16 不加宽）+ SEL-01 drag_end 纯函数（空选区状态、按下格内回拖不误判反向）+ BLINK-03 present_frame 路径测试 + cell_style 注释位置修正；Linux 85/85 ×3（plain/ASan/TSan），mingw 含 it2w_shell 通过。
- PANE-06 setFont/cellSize：FontSpec + resolveFontChange（空字体名保留、6–72pt 夹取、NaN 保留）、renderer set_font/font_metrics（DWrite designUnits）、经 UiDispatchQueue 异步 applyFont→applyBounds→publishStatus，未定尺寸时 cellSize={0,0}；修 mingw create_formats 声明；Linux 86/86 ×3，mingw 含 it2w_shell 通过。
- BUG-01 行高热修：5f5c2b6 的 pane/win32_term 写死 7.2×10 DIP 而字体按 12pt=16 DIP 建 → 按格裁剪截字、fit_glyph 压窄。现：首次创建即按 DirectWrite 实测（失败打日志）；TextFormat 字号乘 DPI，SetLineSpacing(UNIFORM, 格高, round(ascent×scale))，DPI/格高变化时重建；win32_term 去掉写死值；text_clusters 加 kMaxClusterCps=32。Linux 86/86 ×3，mingw 通过。
