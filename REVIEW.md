# termv2 实现与设计差距评审

日期：2026-10-28（修订版，取代 2026-10-26 初版）  
对照设计：《iTerm2 Windows 移植设计 v0.3》（2026-10-23，会话中形成的设计文档，未包含在仓库中；相关要求在下文逐项说明）  
对照上游：[gnachman/iTerm2 @ f29420a](https://github.com/gnachman/iTerm2/tree/f29420ae4d3e0f986ee24ea286885d32af58175a)（即 UPSTREAM.md 固定的提交）  
仓库：[yinzhaoxuan/termv2](https://github.com/yinzhaoxuan/termv2)  
审查基线：main `57618bb`。该提交只新增了 REVIEW.md，实现代码与 `cd71b7ee1f1da122b0be869be24b79f078d62b94` 完全相同，因此下文的代码链接仍固定到 `cd71b7e`，行号有效。

## 0. 本次修订说明

相对 2026-10-26 初版，本次：

1. **补充实测证据**：在 Linux（Ubuntu 24.04，GCC，CMake）上对基线做了干净配置、构建和 CTest，并用核心 C ABI 写了小探针，直接验证解析和应答行为（见 §1）。
2. **更正初版中的不准确表述**：
   - GAP-01：重连时 attempt **已经**递增（[win32_pane_backend.cpp:733](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L733)），问题只在 SessionId 固定为 1。另外初版低估了崩溃残留文件的影响：残留一个 `ssh-1-1.conf`，此后**所有**新 SSH 窗格的首次连接都会失败，见修订后的 GAP-01。
   - GAP-11：初版说缺少 nlohmann_json「不必然造成构建失败」，这不对。`find_package(... REQUIRED)` 会让干净的本机配置**直接失败**，已复现，见 GAP-11。
   - §5「缺少 ssh.exe」：初版写的是「应验证」。按调用链可以确定窗格会**永久停在 Starting**，因此升为缺陷 GAP-15。
   - GAP-09：核心默认值本来就是 10000 行 / 64 MiB（[core.cpp:326](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L326)），和设计一致。偏离来自后端配置的覆盖值，用户也无法配置。
3. **新增与上游 iTerm2 的功能差距分析**（§3）：初版只对照设计文档，没有回答「离 iTerm2 还差多少」。
4. **新增缺陷**：GAP-14（SGR 解析会误清属性、丢颜色）、GAP-15（缺 ssh.exe 时卡在 Starting）、GAP-16（DA 能力宣告与实现不符）。
5. **新增合并待办清单**（§8），给每项标注批次、优先级和验收方式。

## 1. 结论与证据边界

当前应定位为「终端核心与 Win32 集成已有较多实现，尚未完成 Windows 日常使用验收」的阶段。核心解析、屏幕、输入编码、D2D 渲染、ConPTY、SSH 配置隔离、工作区模型和 Hyper-V 后台模块都已有代码，不需要重做。下一阶段应集中修复集成问题、接通用户操作，并补齐实机证据。

以 iTerm2 为参照，本项目目前覆盖的是 **「VT100/xterm 基础仿真 + 多标签分屏 + SSH 连接管理」** 这一子集。iTerm2 的标志性能力，包括 tmux 集成、Shell Integration（OSC 133/1337）、内联图片、触发器、Profile 体系、查找、即时回放和 Python API，都还没有开始（§3）。这与设计 v0.1 的范围一致，不算缺陷，但对外描述时不应称为「iTerm2 移植已基本完成」。

单凭目录数量或单测数量，给不出可信的完成百分比。尤其不能因为 Hyper-V 库已存在，就认为主界面支持 Hyper-V；也不能因为本地终端模型已存在，就认为保存的 PowerShell 配置能启动。

**本次实际执行的验证**（Linux，基线 `57618bb`）：

| 项目 | 结果 |
|---|---|
| 干净配置 `cmake -S . -B build` | **失败**：`find_package(nlohmann_json)` 找不到（GAP-11） |
| 加 `-DUICFG_NLOHMANN_INCLUDE_DIR=third_party/nlohmann_json/include` 后构建 | 通过 |
| CTest | **71/71 通过**，配置阶段警告「未找到测试向量，跳过 CORE-01/02」。STATUS 自述 86/86，差额 15 项。缺失的 CORE-01/02 向量是已知原因之一，其余未逐项核对 |
| `T07.ssh_live` | 显示通过，但本机没有 22422 端口的测试 sshd，应属于内部 SKIP，不算 SSH 实连证据 |
| SGR / 应答探针（链接 `libitcore`） | 发现 GAP-14 与 GAP-16 所列行为 |

**未执行**：没有在 Windows 上编译或运行（本机没有 MSVC/mingw），没有 ConPTY、D2D、IME 或 Hyper-V 的实机验证，没有连接用户的 VM。仓库 STATUS 中的 ASan/TSan 和 mingw 通过记录属于项目自述，这次没有复跑。树中没有 `.github/workflows`，GitHub Actions 也没有运行记录。

下文的用语约定：「已实现」指代码可定位；「代码缺陷」指能从当前调用链推导出、或已用探针复现的问题；「待验收」指仅凭静态代码无法判定的事项。

## 2. 设计覆盖矩阵

| 设计部分 | 已看到的实现 | 仍有差距 | 判定 |
|---|---|---|---|
| T01 源码派生与来源 | UPSTREAM 固定 SHA，SOURCE_MAP 对照到上游函数，部分派生文件带 SPDX | 许可表述冲突；无根 LICENSE；`itcore.h` 等派生文件缺 SPDX；无上游文件哈希清单 | 部分完成 |
| T03/T04 核心构建与解析 | C++20/C ABI、VT 状态机、分块、上限、事件/EOF 测试 | CORE-01/02 向量依赖仓库外绝对路径；DCS 只解析不执行 | 有实现，复现不完整 |
| T05 屏幕与输入 | 主/备用屏、SGR、Unicode/字素池、历史重排、选区、键盘/鼠标/粘贴编码 | SGR 子参数缺陷（GAP-14）；历史搜索未接；资源账本缺失；EOF 集成有问题 | 主要能力已有代码 |
| T03A/T07 传输与 SSH | ConPTY/Job、读写队列、关闭线程、独立 -F 快照、三种认证策略 | 会话 ID/快照碰撞；缺 ssh.exe 时卡死；Windows SSH 无实测 | 有实现，不能验收通过 |
| T06 渲染与输入法 | D2D/DWrite、字体/DPI、IME、脏行、设备恢复代码 | 真机视觉、IME、混合 DPI、设备丢失和延迟待验证；下划线样式无数据来源 | 有实现，待实机 |
| T08 工作区 | 会话树、标签、双向分屏、复制粘贴、重连归档、布局持久化 | 连接编辑器、历史搜索、主题/配色/回滚设置界面未接；恢复行为违背设计 | 部分可用路径 |
| 本地 PowerShell | LocalSettings 与 LaunchSpec 类型已定义，空 ID 能启动本地 shell | 保存的 local 配置无法路由；参数、cwd、环境未贯通 | 集成缺口 |
| T09 Hyper-V | 查询、地址筛选、轮询/超时、后台传输与纯逻辑测试 | 主 UI 没有注入 resolveHyperV；缺 VM/地址选择与启动操作 | 后台有实现，主流程未接 |
| 配置可靠性 | schema/revision、临时替换、备份、高版本只读、目录锁 | local.environment 不保真；加载时不校验 | 部分完成 |
| 生命周期/性能/诊断 | 单窗格异步关闭、pump 时间预算、部分内存限额、stderr 诊断 | 解析仍在 UI 线程；全局内存预算、日志导出和性能基线未闭环 | 与详细设计有偏离 |

## 3. 与上游 iTerm2 的功能差距

本节回答「与 iTerm2 相比还差什么」。「范围」列给出建议：**v0.1** 表示日常试用需要；**v0.2+** 表示属于 iTerm2 特色，可在 v0.1 之后排期；**不做** 表示与 macOS 绑定或价值低。上游引用均指向固定提交 `f29420a`。

### 3.1 终端仿真（ports/core 对照 `sources/VT100/`）

| 能力 | iTerm2 上游 | termv2 现状 | 范围 |
|---|---|---|---|
| 基本光标/编辑/滚动 CSI（CUP/ED/EL/IL/DL/ICH/DCH/ECH/SU/SD/REP/HPR/VPR/CHT/CBT 等） | 有 | 有（[screen.cpp:542-577](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L542)） | — |
| DECSTBM、DECOM、DECAWM、IRM、LNM、制表位、SCS/DEC 图形、DECALN、DECSTR、RIS | 有 | 有 | — |
| SGR 基本属性、16/256/真彩（分号形式） | 有 | 有 | — |
| SGR 子参数：`38:2::r:g:b`、`38:2:cs:r:g:b`、下划线样式 `4:0..5`、`21` 双下划线、`58/59` 下划线颜色、`53/55` 上划线 | 有（[VT100GraphicRendition.m:38](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100GraphicRendition.m#L38)、[:305](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100GraphicRendition.m#L305)） | **缺失或错误**，见 GAP-14 | **v0.1** |
| XTPUSHSGR/XTPOPSGR、XTREPORTSGR、XTPUSHCOLORS | 有 | 无 | v0.2+ |
| 鼠标 9/1000/1002/1003/1006 | 有 | 有 | — |
| 鼠标 1005（UTF-8）、1015（urxvt）、1016（SGR 像素）、1007（备用屏滚轮转方向键） | 有 | 无（DECRQM 回 0）。1007 的行为在后端以写死规则实现，不能由程序开关 | v0.2+（1007 建议 v0.1 做成可查询） |
| DECSLRM / DECLRMM（?69 左右边距） | 有 | 无 | v0.2+，但与 GAP-16 的 DA 宣告相关 |
| 矩形操作 DECCRA/DECFRA/DECERA/DECSERA/DECCARA/DECRARA、选择性擦除 DECSCA/DECSED/DECSEL | 有 | 无 | v0.2+ |
| DECDHL/DECDWL 双宽双高行 | 有 | 无 | 不做/低 |
| DCS：DECRQSS、XTGETTCAP（`DCS + q`）、DECRSPS | 有 | **解析后丢弃**，无应答（已用探针确认） | **v0.1**（DECRQSS m/r、XTGETTCAP TN/Co/RGB 是 vim/neovim/tmux 能力探测常用项） |
| XTVERSION（`CSI > q`）、DA3 | 有 | 无应答 | v0.1（XTVERSION 低成本） |
| DA1 / DA2 | 按 emulation level 生成（[VT100Output.m:956](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100Output.m#L956)、[:1115](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100Output.m#L1115)） | DA1 `?1;2c` 写死，DA2 写死 VT500 级 `>64;2500;0c`，两者自相矛盾，见 GAP-16 | **v0.1** |
| CSI t 窗口操作 | 完整（含 8t/9t/11t/13t 等，受设置控制） | 14/16/18/22/23t；其余忽略 | 基本够用 |
| ?2026 同步输出、?1004 焦点、?2004 括号粘贴 | 有 | 有 | — |
| ?2027 字素簇模式、?2031 主题变化通知、?2048 带内尺寸报告 | 有 | 无 | v0.2+ |
| Kitty 键盘协议（`CSI > u` / `CSI ? u` push/pop/query） | 有（VT100CSI_PUSH/POP/QUERY_KEY_REPORTING_MODE） | 无 | v0.2+ |
| modifyOtherKeys（`CSI > 4 ; n m`） | 有（SET/RESET_MODIFIERS） | 无 | v0.2+ |
| OSC 0/1/2 标题、4/10/11/12/104/110-112 颜色、8 超链接、52 剪贴板 | 有 | 有（52 默认关闭） | — |
| OSC 7 当前目录 | 有 | 无 | v0.2+（新标签继承目录依赖它） |
| OSC 9 / OSC 777 通知 | 有 | 无 | v0.2+（可映射 Windows Toast） |
| OSC 133 / FinalTerm 提示符标记 | 有（XTERMCC_FINAL_TERM、PromptStateMachine.swift） | 无 | v0.2+（Shell Integration 基础） |
| OSC 1337 私有扩展（SetMark、CurrentDir、RemoteHost、SetUserVar、File 内联图片、SetBadgeFormat、Copy、ReportCellSize 等约 50 个键） | 有 | 无 | v0.2+（按子命令挑选） |
| Sixel / iTerm2 内联图片 / Kitty 图形 | Sixel 与 iTerm 图片有 | 无 | v0.2+ |
| tmux 控制模式（`tmux -CC`，DCS 1000p） | 有，是 iTerm2 核心卖点 | 无 | v0.2+（工作量大，需独立设计） |
| 8 位 C1 控制 | 可选支持 | 固定关闭（SOURCE_MAP 已说明） | 不做 |
| Unicode 宽度表 | 可选 Unicode 版本 | UCD 16.0.0 生成表 | — |

### 3.2 应用层功能（对照 iTerm2 用户可见功能）

| 能力 | iTerm2 | termv2 现状 | 范围 |
|---|---|---|---|
| 多标签、水平/垂直分屏、焦点切换 | 有 | 有 | — |
| 复制粘贴、双击选词、三击选行、拖选自动滚动 | 有 | 有 | — |
| 多行粘贴确认 / 粘贴历史 / 粘贴特殊处理 | 有 | 无确认（GAP-06），无历史 | 确认 v0.1，其余 v0.2+ |
| 回滚区查找（含正则） | 有 | 快捷键已定义，功能未实现（[main.cpp:541](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L541)） | **v0.1** |
| Profile（配色、字体、回滚行数、按键、命令） | 核心概念 | `Connection.appearanceProfileId` 只存储不使用；`PaneBackendConfig.colors` 主 UI 没有传；回滚行数不可配置 | **v0.1** 做配色和回滚；完整 Profile 放 v0.2+ |
| 配色方案导入（.itermcolors） | 有 | 无 | v0.2+（适合作为 Profile 的第一个导入格式） |
| 连接/书签管理 | Profiles 窗口 | 只有会话树，没有新建/编辑/删除界面（GAP-07） | **v0.1** |
| 快捷键配置界面 | 有 | 只能手改 settings.json | v0.1 只读展示，编辑放 v0.2+ |
| Hotkey 下拉窗口 | 有 | 无 | v0.2+（Win32 可用 RegisterHotKey 实现） |
| 广播输入到多个窗格 | 有 | 无 | v0.2+ |
| Shell Integration：命令标记、跳转到上/下一条提示符、命令历史、最近目录 | 有 | 无（依赖 OSC 133/1337） | v0.2+ |
| 触发器（正则匹配输出后执行动作） | 有 | 无 | v0.2+ |
| 智能选择、语义历史（点击路径打开） | 有 | 只有 OSC 8 链接点击 + 确认 | v0.2+ |
| 即时回放（DVR） | 有 | 无（SOURCE_MAP 明确未移植） | 不做/远期 |
| 会话日志记录到文件 | 有 | 无 | v0.2+ |
| 状态栏、徽标（badge）、标签状态/活动指示 | 有 | 只有基础状态栏 | v0.2+ |
| 窗口布局保存/恢复（Arrangements） | 有 | 工作区持久化有，恢复时自动连接违背设计（GAP-05） | v0.1 修行为 |
| 背景图片、透明度、模糊 | 有 | 无 | v0.2+（Win11 可用 Mica/Acrylic） |
| 连字（ligature） | 有 | 走 DrawText 逐格绘制，不支持；GlyphCache 图集未接（STATUS） | v0.2+ |
| 密码管理器 | 有（Keychain） | 无 | v0.2+（Windows Credential Manager） |
| Python API / 脚本 | 有 | 无 | 不做/远期 |
| 自动更新、崩溃报告 | 有（Sparkle） | 无 | 分发前 |
| Windows 特有：本地 PowerShell/cmd、WSL、Hyper-V 发现 | 不适用 | PowerShell 路由断（GAP-04）、Hyper-V 未接（GAP-03）、WSL 无配置类型 | PowerShell 和 Hyper-V 属于 v0.1；WSL 放 v0.2+ |

## 4. 优先修复的代码与集成问题

优先级：P1 = 阻塞主要使用路径或可能丢失输出，应在下一次试用前处理；P2 = v0.1 日常试用验收前补齐。编号用于后续追踪，不代表已在 GitHub 创建 Issue。

### GAP-01 · P1：所有 SSH 窗格共用 SessionId 1，快照文件互相冲突

**证据：** [win32_pane_backend.cpp:330](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L330)、[:360](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L360)、[:382](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L382) 这三处启动（本地、Hyper-V、SSH）都传入 `SessionId{1}`。每个后端的 attempt 从 1 开始，重连时递增（[:733](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L733)）。[ssh_transport.cpp:46](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/platform/windows/ssh/ssh_transport.cpp#L46) 按 `ssh-<sid>-<aid>.conf` 命名，用 CREATE_NEW 创建，失败即返回「无法创建快照文件」。主 UI 给所有连接提供同一个 snapshots 目录（[main.cpp:865](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L865)）。

**触发与影响：**
- 第一个 SSH 窗格还没结束时，再打开任何 SSH 窗格（另一个连接、同一连接或分屏），都会尝试创建 `ssh-1-1.conf`，第二个必然失败。这直接阻塞设计中「同一配置打开多个独立 Session」的要求。
- 程序崩溃或被强杀后，`ssh-1-1.conf` 会残留。由于每个新窗格的首次 attempt 都是 1，此后**所有** SSH 首次连接都会失败，直到用户手动删除该文件。初版评审低估了这一点。
- Hyper-V 路径最终也走 SshTransport，问题相同。

**建议：** 在进程内为每个后端分配单调递增的 SessionId，文件名再加上进程 ID 或启动时间戳，保证跨进程重启也唯一。保持 CREATE_NEW，不要用覆盖现有文件来绕过。启动时清理 snapshots 目录中不属于当前实例的过期文件（凭目录锁判断）。

**验收：** 同配置和不同配置各开多个 SSH 窗格，交叉关闭和重连，快照互不覆盖、互不误删。预置一个 `ssh-1-1.conf` 残留后，仍能新建会话。

### GAP-02 · P1：结束会话时可能提前 EOF，丢失队列尾部输出

**证据：** [win32_pane_backend.cpp:413](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L413) 的 finishSession 只调用一次 pump，随后立即 `itcore_finish_input`。[frame_pump.h:14](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/frame_pump.h#L14) 给每轮 pump 设置 64 KiB / 8 ms 预算，预算用尽返回 FedMore；[win32_pane_backend.cpp:521](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L521) 只是安排定时续跑。[core.cpp:338](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L338) 在 finished 之后拒绝 feed，返回 FATAL。续跑的 pump 收到 Fatal 后，会再对已经关闭的传输调用一次 `close(Fault)`（[:526](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L526)）。

**触发与影响：** 收到传输 Closed/Failed 时，如果输出队列里还剩超过一轮预算的数据，第一轮之后就被标记 EOF，后续定时器取到的剩余字节无法再解析。错误报告或命令的最后几行可能丢失，UTF-8 尾部也可能被过早判为残缺。

**建议：** 把「传输已结束」和「核心已消费完尾部」拆成两个状态。进入排空状态后，允许按预算多轮续跑；只有在传输最终关闭、队列确认清空之后，才执行 finish_input、生成最终快照、发布 Exited/Failed。故障丢弃路径要单独报告输出不完整。

**验收：** 模拟队列中有 10 × 16 KiB 数据，末尾放唯一标记和跨块 UTF-8；先通知传输关闭，再处理 UI 队列，确认全部消费后才 EOF。现有 [test_pane_logic.cpp:439](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/tests/app/test_pane_logic.cpp#L439) 只覆盖多轮 pump，没有覆盖 finishSession 与 EOF 的组合。

### GAP-03 · P1：Hyper-V 后台已写，正式主界面没有接入

**证据：** 后端有 resolveHyperV 与 HyperVTransport 分支（[win32_pane_backend.cpp:331](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L331)），但 [main.cpp:876](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L876) 的 makeFactory 只给 resolveSsh 赋了值。[main.cpp:851](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L851) 遇到 Hyper-V 配置时只取 fallbackHost，没有的话直接返回「Hyper-V 地址解析尚未接入」。

**影响：** 即使 VM ID 配置正确、VM 正在运行，从主界面连接也不会查询 Hyper-V。配置了备用地址时会直接连该地址，跳过设计中的解析、候选选择和回退判断，用户也看不出走的是回退路径。

**建议：** 复用现有后台模块，补上 Connection → HyperVLaunch 的映射并注入工厂。增加解析进度显示、多地址选择、明确的「启动并连接」操作，以及可见的手动回退。普通连接操作不能暗中启动 VM。

**验收：** 真机覆盖以下情形：VM 运行中、已关机、不存在、权限不足、模块缺失、地址变化、多个候选、用户取消。记录每次实际走的是自动解析还是手动回退。

### GAP-04 · P1：保存的本地 PowerShell 配置无法执行

**证据：** [model.h:36](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/include/uicfg/model.h#L36) 定义了 LocalSettings，但后端只在 connectionId 为空时走 LocalT，其余情况最终都请求 resolveSsh（[win32_pane_backend.cpp:309](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L309)）。主 UI 的 resolveSsh 明确拒绝非 SSH 配置（「不是 SSH 连接」）。空 ID 时默认用 ComSpec/cmd，不读取已保存 local 配置的 arguments 和 workingDirectory。

此外，[store.cpp:66](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L66) 加载 local 时不读 environment；[store.cpp:97](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L97) 保存时固定写出 `inherit=true, set={}, unset=[]`，closePolicy 也是写死的，设计中的环境覆盖无法保留。

**影响：** 空白的「本地终端」能启动，但保存一项 PowerShell 连接后双击会失败。手工写入的环境配置不会生效，下次保存时还会被重置。

**建议：** 统一解析成区分 Local / SSH / Hyper-V 的启动描述，或增加 resolveLocal。完整传递 executable、arguments、cwd 和 inherit/set/unset，落实路径与环境校验。按设计把 cwd=null 映射为用户主目录。

**验收：** 从连接树启动 PowerShell，验证带空格和中文的参数、显式 cwd、null cwd、环境变量的 unset/set。保存后重新加载，各字段逐一保持不变。程序不存在或目录不存在时给出明确错误。

### GAP-05 · P1：恢复工作区时自动启动所有有效连接

**证据：** [main.cpp:927](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L927) 恢复标签后逐个 createPane；[main.cpp:383](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L383) 对引用有效的窗格立即调用 startSession。

**与设计的差距：** v0.3 §13.3 要求恢复为未连接状态，由用户点击后再连接。当前只有引用缺失的窗格不会启动。iTerm2 的 Arrangement 恢复会直接启动，但本项目设计有意选择了不同行为，应以设计为准。

**建议与验收：** 把布局构建和会话启动拆开，恢复时只建占位视图。保存一个含四个 SSH 窗格的工作区后重启，应启动零个 ssh.exe；点击一个窗格，才新建一次连接。首次安装时是否自动打开本地终端，可作为独立规则保留。

### GAP-06 · P1：多行粘贴没有预览确认

**证据：** [main.cpp:481](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L481) 从剪贴板读取后直接调用 backend->paste；[win32_pane_backend.cpp:638](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L638) 只处理状态、大小和编码。没有看到设计 §12.4 要求的多行预览和确认路径。iTerm2 也提供多行粘贴前的警告。

**影响：** 32 KiB 上限和 bracketed paste 编码已经实现，但不能代替用户确认。远端没有启用 bracketed paste 时，多行文本可能被直接执行。

**建议与验收：** 发送前对规范化后的多行内容显示预览；确认后只提交一次原事务，不额外补回车；取消时发送零字节。保留现有的 NUL 和内嵌标记清洗以及大小上限。

### GAP-14 · P1（新增）：SGR 子参数处理会丢颜色，甚至误清全部属性

**证据：** [screen.cpp:975](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L975) 只处理 38/48；分号形式的展开循环（[:991](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L991)）同样只认 38/48。`4` 不看子参数（[:952](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L952)）。上游参照见 [VT100GraphicRendition.m:38](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100GraphicRendition.m#L38)（下划线样式）和 [:305](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100GraphicRendition.m#L305)（颜色子参数）。

**探针复现**（基线构建的 libitcore，对首格读 fg/flags）：

| 输入 | 期望（上游行为） | 实际 |
|---|---|---|
| `CSI 38;2;255;0;0 m` | fg = 红 | 正确 |
| `CSI 38:2:255:0:0 m` | fg = 红 | 正确 |
| `CSI 38:2::255:0:0 m`（ITU 标准形式，色彩空间留空） | fg = 红 | **忽略，fg 仍为默认** |
| `CSI 38:2:0:255:0:0 m`（显式色彩空间 0） | fg = 红 | **错读为绿（0,255,0）** |
| `CSI 4;58;2;0;255;0 m` | 下划线 + 绿色下划线色 | **flags = 0**：58 不消费后续参数，`2` 被当作 SGR 2（暗淡），末尾 `0` 被当作 SGR 0，**清掉前面所有属性** |
| `CSI 4:0 m` | 关闭下划线 | **仍有下划线** |
| `CSI 4:3 m` / `CSI 21 m` | 波浪线 / 双下划线 | 只有普通下划线 / 忽略 |
| `CSI 53 m` | 上划线 | 忽略 |

**根因：** SOURCE_MAP 记录的「空子参数保留为 null」这一差异，修正了上游的位置错位问题，但 38/48 的取值仍按「没有色彩空间」的下标读，于是 ITU 形式反而读错。58/59 完全没有实现，导致分号形式的参数泄漏给后续 SGR。

**影响：** 任何发送分号形式 `58;2;R;G;B` 或 `58;5;N` 的程序都会触发属性被清零。例如 Vim 的 `t_8u` 常见取值就是分号形式 `\e[58;2;%lu;%lu;%lum`，开启 `termguicolors` 后，拼写检查或诊断一出现带颜色的下划线就会触发（需在 C3 中实测确认）。Neovim、Helix 等编辑器用 `4:3` 加 `58:2::r:g:b` 表示 LSP 诊断，现在显示为普通下划线、无颜色。`itcore_cell.underline_color` 字段和渲染器的 `underline_color` 都已存在，只是核心从不写入它。

**建议：** 按上游语义重写 38/48/58 的取值：冒号形式中子参数 ≥ 5 个时跳过色彩空间；分号形式中 58 与 38 一样消费 `;5;N` 和 `;2;R;G;B`。实现 59、`4:0..5`、21、53/55，在 `itcore_cell` 中增加下划线样式位，并让 D2D 绘制波浪线、双线、点线和虚线。

**验收：** 新增 CORE-22，把上表全部写成用例，同时覆盖 `48:5:N`、`38;5;N` 与其他属性组合，以及被截断的参数（`38;2;1`）不得越界读取或泄漏参数。

### GAP-15 · P1（由初版「回归边界」升级）：缺少 ssh.exe 时窗格永久停在 Starting

**证据：** [ssh_transport.cpp:38](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/platform/windows/ssh/ssh_transport.cpp#L38) 发现客户端不存在时，只回调 `onFinished(SshNotFound)` 并返回 `nullopt`，不发 `onStateChanged`。后端把 `nullopt` 当作启动成功（[win32_pane_backend.cpp:382](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L382)），而 onFinished 只保存了 `sshExit_`（[:373](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L373)）。finishSession 只由 Closed/Failed 状态触发，所以状态永远不会离开 Starting。

**影响：** Windows「可选功能」中没有安装 OpenSSH 客户端的机器很常见（家庭版、精简镜像）。用户看到的是一个永远在「连接中」的窗格，没有任何错误提示。

**建议与验收：** 所有启动前失败都统一走 `fail()`，或者补发 `Failed` 状态。把 `sshExe` 指向不存在的路径来启动，窗格应在一帧内显示「未找到 ssh.exe」并给出安装指引；重连按钮可用。

## 5. v0.1 交付仍需补齐的部分

### GAP-07 · P2：会话管理与设置的模型领先于用户界面

主 UI 已有会话树筛选、双击连接、多标签、分屏、复制粘贴和字号调整，不是纯占位外壳。顶层 CMake 构建会链接真实的 PaneBackend，只有单独构建 ui 子项目时才启用 stub（[ui/CMakeLists.txt:29](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/CMakeLists.txt#L29)）。

但当前主程序入口和消息分派中，找不到连接新建/编辑/删除、分组管理、VM 选择，以及字体、主题、快捷键的设置界面。uicfg 的模型和单测不能替代这些入口。历史搜索明确没有接：[main.cpp:541](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L541) 注释写着 searchHistory 未实现，但 settings 中已为它分配了 `Ctrl+Shift+F`，按下后会原样交给终端。

另外，[makeFactory](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L876) 只传了字体、字号和 wordChars：
- `PaneBackendConfig.colors` 从未设置，配色只能用 xterm 默认调色板；
- `scrollbackLines` / `memoryLimitBytes` 不能配置；
- `Connection.appearanceProfileId` 只存储，从不读取；
- OSC 52 没有任何开关入口（默认关闭是对的，但用户无法开启）。

**建议：** 先补一个最小编辑器，能完成「新建 Ubuntu 连接 → 保存 → 连接 → 编辑 → 再次连接」。再补当前 Session 的历史搜索（结果跳转、软换行映射、取消和限额），以及配色方案（可以先支持导入 .itermcolors）和回滚行数设置。发布前提供经过校验的示例配置和首次运行说明。

还有一处明确的交互偏离：设计默认 Ctrl+C 始终发送中断，当前默认的 copyOrInterrupt 在有选区时执行复制（[settings.cpp:23](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/settings.cpp#L23)、[main.cpp:531](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L531)）。这和 Windows Terminal 的默认行为一致，有合理性，但应作为明确的产品决策登记到设计里，或者恢复为设计默认。

### GAP-08 · P2：核心仍在 UI 线程运行，预算不等于线程隔离

[win32_pane_backend.cpp:142](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L142) 明确写着 pump 在 UI 线程执行；feed、事件处理、快照和 present 也都在这条路径上。64 KiB / 8 ms 预算和低优先级定时器是已落实的响应性改进，但设计 §6/§15 要求每个会话由自己的执行器串行修改核心，UI 只消费快照。

当前的预算检查以块为边界，无法中断一次耗时的 feed；之后的快照、重排和绘制也不全在这 8 ms 之内。多个窗格的输出仍在争用同一条 UI 线程。另外，[STATUS「接口问题 7」](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/docs/STATUS.md)提到 `itcore_history_rows` 每次调用都要重排整个历史，复杂度 O(历史)，而它在每次有输出时都会被调用。历史一大，这项开销会直接落在 UI 线程上。

**建议：** 把它登记为临时架构偏离。先在真实 Windows 上测量：四个窗格持续输出时的键盘延迟、缩放和关闭。然后按设计迁移到会话执行器，或者根据测量结果写一份正式 ADR，决定保留哪种模型。不能仅凭 PUMP-01 单测就宣称延迟达标。

### GAP-09 · P2：只有局部限额，没有完整的会话/应用内存账本

历史、字素池、输入/输出队列、粘贴和旧 attempt 文本都已有局部限制。[screen.cpp:583](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L583) 把 memory_limit 交给历史，并另外分配字素池预算；[core.cpp:583](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L583) 的快照分配不经过全局额度申请。在已审阅的启动、核心、快照和主 UI 路径中，没有看到设计中的统一账本（每 Session 128 MiB、整个应用 512 MiB）及受控拒绝。

核心默认值是 10000 行 / 64 MiB（[core.cpp:326](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L326)），与设计一致。但后端配置把它覆盖成了 5000 行 / 32 MiB（[win32_pane_backend.h:27](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.h#L27)），用户无法调整（GAP-07）。这个初始值可以保留，但要在设计中登记并统一，也不能把它称为 Session 实际存活内存的上限。

**建议与验收：** 按真实存活对象记账，覆盖旧快照仍持有的池、历史、队列和归档；新增压力下的淘汰策略和拒绝新建策略。在多个会话接近限额、100 次重连、快照滞留等场景下，验证没有虚扣、无限增长或悬空引用。图形显存与进程 Private Bytes 分开统计。

### GAP-10 · P1 验收门槛：Windows 通路、TERM 与端到端行为没有闭环证据

[T03A-Windows规格-v0.1.md:6](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/docs/T03A-Windows规格-v0.1.md#L6) 明确说全部条目都没有实机验证；[STATUS.md:137](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/docs/STATUS.md#L137) 列出了 ConPTY、D3D、Windows SSH、IME 等待测项。代码里有探针，不等于已经有 CAP-01 要求的记录：ConPTY 前后的字节对照、TERM/terminfo 以及应答归属。

设计把 T03A 定为冻结 T05 和传输职责之前的门槛，实际代码已经推进到 T09，对应的实验却仍缺证据。应把它视为需要尽快补上的架构验证，而不是留到最后的形式测试。代码中没有任何地方显式设置 `TERM`：本地会话继承父进程环境，SSH 会话取决于 Windows OpenSSH 的默认值。远端实际拿到的 TERM 值、它对应的 terminfo 能力和 GAP-16 的 DA 宣告三者必须一致，这一点需要在 CAP-01 中实测记录。

**最小实机包：** 固定 Windows、Ubuntu、OpenSSH、字体、DPI 和 GPU 的版本，验证以下场景：本地 PowerShell、Ubuntu SSH、vim/top/tmux（含开启 `termguicolors` 和拼写检查的 vim，覆盖 GAP-14）、中文 IME、鼠标、32 KiB 粘贴、备用屏、尺寸同步、断网、连续输出中退出、100 次关闭循环。保存 CAP-01 能力矩阵、尾部字节记录、进程和句柄趋势，以及输入延迟分位数。

### GAP-11 · P2：干净环境无法直接配置，测试复现不完整（更正初版）

**构建依赖：** [ui/CMakeLists.txt:7](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/CMakeLists.txt#L7) 在非交叉编译时执行 `find_package(nlohmann_json 3.11 REQUIRED)`，而根 CMakeLists 只在交叉编译时才把 `UICFG_NLOHMANN_INCLUDE_DIR` 指向仓库内的头文件（[CMakeLists.txt:65](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/CMakeLists.txt#L65)）。本次在没有安装 nlohmann-json3-dev 的 Ubuntu 24.04 上，`cmake -S . -B build` **配置失败**。Windows 原生 MSVC 构建同样会失败，除非另外通过 vcpkg 安装。仓库里明明已经附带了这份头文件。

**测试数据：** [CMakeLists.txt:41](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/CMakeLists.txt#L41) 的 CORE-01/02 输入默认指向 `/workspace/qa/vectors/core01_02.json`，文件不存在时只给 WARNING 并跳过。仓库里有 golden 输出 `tests/golden/core01_02.tokens`，却没有对应的输入。干净检出得到「71/71 通过」，实际少跑了基础向量，与 STATUS 的 86/86 无法对上。SOURCE_MAP 中的「本地 `/workspace/iterm2-upstream`」也是仓库外路径。

**建议：**
- 根 CMakeLists 无条件使用 `third_party/nlohmann_json`，可选 `find_package` 作为覆盖。
- 把 `core01_02.json` 纳入 `tests/vectors/`；必需测试缺数据时应 FATAL_ERROR，不能静默减项。
- 提供 `CMakePresets.json`：linux-debug、linux-asan、windows-msvc、mingw-cross。
- 添加根 README：依赖、主程序目标（`it2w_shell`）、运行方式和产物位置。
- 接入 GitHub Actions：Linux 核心测试（plain/ASan/TSan）+ Windows MSVC 编译 + 可自动化的 ConPTY 生命周期测试。
- 每份测试报告写明提交 SHA，以及注册、执行和跳过的用例数。

### GAP-12 · P2，分发前解决：来源与许可材料需要统一

优点是已经固定上游 `f29420ae4d3e0f986ee24ea286885d32af58175a`，SOURCE_MAP 还记录了函数级差异，不能把本项目简单判定为没有来源记录的重写。

问题如下：
- [UPSTREAM.md:4](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/UPSTREAM.md#L4) 写的是「GPL v2 或更新版本（整体按 GPL v3）」，而 [SOURCE_MAP.md:4](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/SOURCE_MAP.md#L4) 和 11 个带 SPDX 的文件写的是 `GPL-2.0-only`。固定上游的 [COPYING](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/COPYING) 说明是 v2 或更新版本，且上游整体因依赖关系实际适用 v3。这几处表述不能同时作为清晰的分发说明。
- SPDX 覆盖不完整：`ports/core/include/itcore.h` 的注释明确引用了上游，却没有 SPDX 头；`renderer/`、`app/`、`platform/` 中只有 `renderer/d2d/renderer.cpp` 有 SPDX 头。需要逐个判断哪些文件属于派生作品，哪些是原创。
- 项目根目录没有 LICENSE/COPYING，也没有所选上游文件的逐项哈希清单；只有 nlohmann_json 自带 LICENSE。没有看到单独的依赖闭包审计产物。

**建议：** 根据实际移植的文件和实际引入的依赖，核对来源及适用许可，统一 SPDX、根目录文档和分发说明，补齐许可文本、原作者与修改声明、上游路径和文件哈希。不要直接拿上游全量应用的依赖结论，去推断本项目实际依赖组合的最终许可。这是材料与一致性审查，不构成法律意见。

### GAP-13 · P2：诊断输出尚未达到设计要求的可支持性

后端 onDiagnostic 直接 `fprintf(stderr)`，并且忽略回调传入的 session/attempt（[win32_pane_backend.cpp:306](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L306)）。主窗口是 WIN32 GUI 子系统程序，stderr 在正常启动下不可见，所以这些诊断实际上全部丢失。另外，修复 GAP-01 之前，session ID 恒为 1，即使记录下来也无法区分是哪个会话。目前还没有看到设计要求的 5 MiB × 3 滚动日志、诊断包预览和脱敏导出，以及完整的性能记录入口。

**建议：** 先实现元数据日志，记录版本、唯一的 session/attempt、状态、阶段、耗时和错误码。默认不把原始终端输出写入日志。日志要和用户可见的错误关联起来，使 Windows 试用中的问题能定位到具体会话。

### GAP-16 · P2（新增）：DA 能力宣告与实际实现不一致

**证据：** [screen.cpp:1265](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L1265) 的 DA2 写死为 `\e[>64;2500;0c`，即宣称 VT500 级、xterm 补丁号 2500；[:1267](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L1267) 的 DA1 却是 `\e[?1;2c`（VT100 加 AVO）。上游的这两个值都随 emulation level 一起生成（[VT100Output.m:1115](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/sources/VT100/VT100Output.m#L1115)），而它的 500 级还配套实现了 DECSLRM、矩形操作、DECRQSS 等。探针确认，termv2 对 DECRQSS、XTGETTCAP、XTVERSION、DA3 都不应答，DECRQM 对 ?69 返回 0。

**影响：** 有些程序会根据 DA2 的版本号推断能力。例如 Vim 会根据 DA2 报告的 xterm 版本号推断兼容能力，并决定后续发出哪些探测请求。宣称 VT500 和 xterm 2500，会让这些程序以为左右边距、矩形操作等能力可用。探测请求得不到应答时，有的程序会等到超时，表现为启动变慢。

**建议：** DA1 和 DA2 取同一级别。在 DECSLRM 等功能补齐之前，DA2 宣告较低的级别（例如上游 VT100 级对应的 `>0;…`，或 VT220 级的 `>1;…`），补丁号用一个能真实反映能力的值，并登记到设计 §14.3。补上 XTVERSION（返回 `termv2 <版本>`），以及 DECRQSS 的 `m`、`r`、`" q`、`SP q`；XTGETTCAP 对未知项回 `DCS 0 + r ST`，不能沉默。

**验收：** 新增 CORE-23，覆盖 DA1/DA2/DA3/XTVERSION/DECRQSS/XTGETTCAP 的应答格式。实机运行 `vim -u NONE` 并执行 `:echo v:termresponse`，检查 `:set t_RV?` 的结果和启动时间，确认没有探测超时。

## 6. 另外需要纳入回归的边界

这些不替代前述主线，适合在修改对应模块时一并处理。

- **NeedDrain 零进展：** [frame_pump.h:42](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/frame_pump.h#L42) 在 accepted=0 时把当前块的剩余字节全部计为已消费。按核心的实现（[core.cpp:343](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L343)），只有进入 feed 时事件队列已接近容量才会出现 accepted=0。由于 afterFeed 每次都会排空事件，正常路径很难触发，但一旦触发就是静默丢数据。NEED_DRAIN 的契约应当是排空事件后重试同一段字节，并设一个有界的无进展次数上限。不能为了避免忙循环而跳过数据。
- **配置加载校验：** [store.cpp:109](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L109) 解析后直接返回 Ok，只有保存时才调用 validate。非法端口在主 UI 中被直接强转为 `uint16_t`（[main.cpp:859](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L859)），例如 65558 会变成 22，悄悄连到另一个有效端口。应在加载时和启动前统一检查端口、超时、唯一 ID 和引用。
- **Ctrl+Shift+F 吞键：** searchHistory 已有默认快捷键但没有实现，当前返回 false 交给终端，会向远端发送 Ctrl+F 类的编码。实现之前，应在设置默认值中留空，或明确提示「未实现」。
- **文档陈旧：** STATUS 中仍把「退出 shutdown 钩子」「SSH 错误尾部旁路」「固定字体度量」列为待处理或旧状态，但当前源码已包含相应修复。SOURCE_MAP 的 T05 表仍写着「无图片/鼠标扩展」「OSC 8 写入超链接池未移植」「`encode_*` 仍 -100」，与后文及 STATUS 中「已完成」的记录冲突。应改成单一的当前状态，把变更历史放到单独的位置，避免重复实现。

## 7. 推荐推进顺序与验收产物

| 批次 | 工作 | 本批退出标准 |
|---|---|---|
| A：修复会话基础 | GAP-01 ID/快照隔离；GAP-02 EOF 排空；GAP-15 启动失败统一路径；零进展回归 | 两个 SSH 窗格并存，重连互不干扰；大量尾部输出完整；任何启动失败都不卡在 Starting |
| A′：仿真正确性 | GAP-14 SGR 子参数；GAP-16 DA/DECRQSS/XTVERSION；GAP-11 中的向量与依赖 | CORE-22/23 通过；干净检出可直接配置，并执行完整的 CORE-01/02 |
| B：打通主要使用场景 | GAP-03 Hyper-V 注入；GAP-04 本地 PowerShell 路由；GAP-05/06 恢复与粘贴行为 | 从主程序走通本地 PowerShell、手动 SSH、Hyper-V 发现并连接三条路径 |
| C：同步开展 Windows 验证 | T03A/CAP-01、SSH-CFG-01、WIN-01/02/03、VM-01、DPI/IME、TERM 实测 | 固定提交的结果表、能力矩阵、日志/截图与失败项；通路不满足要求时先修架构 |
| D：补齐日常操作 | 连接编辑、分组、历史搜索、配色/回滚设置、快捷键展示；诊断日志 | 不手改 JSON 就能完成最常用的配置；搜索和错误恢复可操作 |
| E：资源与交付 | 会话执行器或正式 ADR、全局内存账本、CI、来源与许可材料 | 性能和内存实测达标；干净环境能构建并执行全部必需测试 |
| F：iTerm2 特色（v0.2+） | 见 §8.F | 按单项立项，各自有设计与验收 |

A/A′/B 的最小可测路径一具备，就应立即开始 C，不要等 D/E 完成再验证。v0.1 不扩大到 SFTP、图片协议、tmux 集成、复杂 shell integration，也不更换 UI 框架。

## 8. 待完成项清单

状态说明：`[ ]` 未开始，`[~]` 有部分代码但未闭环。每项都要能对应到上文 GAP 或 §3。

### A. 会话基础（P1）
- [ ] A1 每个后端分配唯一 SessionId；快照文件名跨进程唯一；启动时清理过期快照（GAP-01）
- [ ] A2 finishSession 改为多轮排空，队列清空后才 finish_input；新增尾部完整性测试（GAP-02）
- [ ] A3 ssh.exe 缺失及所有启动前失败统一走 Failed，并给出可操作的错误提示（GAP-15）
- [ ] A4 NEED_DRAIN 且 accepted=0 时重试，不丢字节，并设无进展上限（§6）
- [ ] A5 加载时校验配置，端口越界报错而不是截断（§6）

### A′. 仿真正确性（P1/P2）
- [ ] A′1 SGR 38/48 冒号形式支持色彩空间；实现 58/59 下划线颜色（GAP-14）
- [ ] A′2 下划线样式 4:0..5、21；上划线 53/55；`itcore_cell` 增加样式位，D2D 绘制波浪线、双线、点线、虚线（GAP-14）
- [ ] A′3 DA1/DA2 级别统一，宣告不超过实际能力；XTVERSION；DECRQSS（m、r、SP q、" q）；XTGETTCAP（GAP-16）
- [ ] A′4 DECRQM 覆盖 ?1007，并让备用屏滚轮转方向键的行为可由程序关闭（§3.1）
- [ ] A′5 新增 CORE-22（SGR）和 CORE-23（应答）测试

### B. 主要使用场景（P1）
- [ ] B1 主 UI 注入 resolveHyperV；显示解析进度、多地址选择、「启动并连接」、可见的回退（GAP-03）
- [ ] B2 local 连接路由：executable、arguments、cwd、环境 inherit/set/unset 完整贯通（GAP-04）
- [ ] B3 store 完整保存和加载 local.environment 与 closePolicy（GAP-04）
- [ ] B4 工作区恢复为未连接占位，点击后才连接（GAP-05）
- [ ] B5 多行粘贴预览和确认，取消时发送零字节（GAP-06）

### C. Windows 实机验证（P1 门槛）
- [ ] C1 CAP-01：ConPTY 前后字节、TERM 实际值、terminfo、应答归属
- [ ] C2 SSH-CFG-01 / T07 在 Windows OpenSSH 上实连：未知主机、密钥变化、密码、口令、拒绝连接
- [ ] C3 WIN-01/02/03：vim/top/tmux、中文 IME、混合 DPI、设备丢失、四窗格持续输出时的延迟分位数
- [ ] C4 VM-01：Hyper-V 解析各分支（见 GAP-03 验收）
- [ ] C5 100 次关闭/重连循环：进程、句柄、内存趋势
- [ ] C6 每份报告绑定提交 SHA 与环境版本

### D. 日常操作（P2）
- [ ] D1 连接新建、编辑、删除，分组管理（GAP-07）
- [~] D2 历史搜索：快捷键已定义，需实现搜索、跳转、软换行映射、取消和限额（GAP-07）
- [ ] D3 配色方案：传入 `PaneBackendConfig.colors`，支持导入 .itermcolors，让 appearanceProfileId 生效（GAP-07、§3.2）
- [ ] D4 回滚行数和内存可配置；统一后端与核心的默认值（GAP-09）
- [ ] D5 快捷键展示（编辑放 v0.2+）；实现搜索之前，Ctrl+Shift+F 不分配默认值（§6）
- [ ] D6 Ctrl+C 默认行为写入设计决策或恢复为设计默认（GAP-07）
- [ ] D7 OSC 52 读/写开关（默认关闭）
- [ ] D8 元数据滚动日志（5 MiB × 3）、诊断包导出（GAP-13）

### E. 资源与交付（P2）
- [ ] E1 会话执行器迁移，或基于实测写 ADR（GAP-08）
- [ ] E2 `itcore_history_rows` 增加缓存，避免每次输出都 O(历史) 重排（GAP-08）
- [ ] E3 会话和应用两级内存账本，含受控拒绝（GAP-09）
- [ ] E4 根 CMake 默认使用仓库内 nlohmann；补齐 core01_02.json；缺数据时让必需测试失败（GAP-11）
- [ ] E5 CMakePresets、根 README、GitHub Actions（Linux plain/ASan/TSan + Windows MSVC）（GAP-11）
- [ ] E6 统一许可表述；补根 LICENSE、SPDX、上游文件哈希清单、依赖审计（GAP-12）
- [ ] E7 STATUS/SOURCE_MAP 去除陈旧条目，变更历史单独存放（§6）
- [ ] E8 GlyphCache 图集与 glyph run 渲染路径（STATUS 第 4 批，也是连字的前提）

### F. iTerm2 特色能力（v0.2+，各自立项）
- [ ] F1 Shell Integration：OSC 133 提示符标记、OSC 7 当前目录、OSC 1337 的 CurrentDir/RemoteHost/SetMark/SetUserVar；跳转到上/下一条提示符；新标签继承目录
- [ ] F2 通知：OSC 9 / 777 → Windows Toast；OSC 1337 RequestAttention
- [ ] F3 Kitty 键盘协议、modifyOtherKeys
- [ ] F4 鼠标 1005/1015/1016；DECSLRM 左右边距；矩形操作与选择性擦除；XTPUSHSGR/XTPOPSGR
- [ ] F5 内联图片：OSC 1337 File、Sixel（可选 Kitty 图形）
- [ ] F6 tmux 控制模式（`tmux -CC`）集成，需要独立设计
- [ ] F7 完整 Profile 体系：每连接的配色、字体、按键、回滚和启动命令
- [ ] F8 触发器、智能选择、语义历史（点击路径打开）
- [ ] F9 广播输入、Hotkey 下拉窗口、会话日志记录到文件
- [ ] F10 状态栏组件、徽标、标签活动指示
- [ ] F11 背景图片、透明度、Mica/Acrylic；连字（依赖 E8）
- [ ] F12 WSL 连接类型；密码管理（Windows Credential Manager）
- [ ] F13 自动更新与崩溃报告（分发前）

明确不做或放远期：即时回放（DVR）、Python API、8 位 C1 控制、DECDHL/DECDWL。

## 9. 设计文档下一次更新建议

本次修订只更新根目录的 REVIEW.md，不修改实现代码和原设计。后续设计 v0.4 应做到：

1. 保留 v0.3 的目标和契约，追加四列：「固定实现 SHA」「代码已实现」「实机验收」「证据链接」。不再把 T01–T09 全部标为待实施，也不全部标为完成。
2. 登记明确的偏离，并分别写出恢复设计或接受偏离的依据：UI 线程执行核心、Win32 当前外壳、Ctrl+C 默认行为、5000 行 / 32 MiB 初始预算、DA1/DA2 身份。
3. 增加「与 iTerm2 的范围对照」一节，直接采用本报告 §3 的「v0.1 / v0.2+ / 不做」划分，避免范围随实现不断漂移。
4. 把本报告的 GAP-01–16 关联到设计条款和验收用例，防止把「库已实现」和「用户能完成操作」混为一谈。
5. 已修复的历史问题保留变更记录，并从当前待办中移除。所有「通过」记录都必须绑定提交和测试环境。
