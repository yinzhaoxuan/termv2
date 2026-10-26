# termv2 实现与设计差距评审

日期：2026-10-26  
对照设计：《iTerm2 Windows 移植设计 v0.3》（2026-10-23，会话中形成的设计文档，未包含在本次仓库提交中；相关要求在下文逐项说明）  
仓库：[yinzhaoxuan/termv2](https://github.com/yinzhaoxuan/termv2)  
审查基线：main，`cd71b7ee1f1da122b0be869be24b79f078d62b94`（2026-10-23）。以下链接全部固定到该提交。

## 1. 结论与证据边界

当前应定位为“终端核心与 Win32 集成已有较多实现、尚未完成 Windows 日常使用验收”的阶段。核心解析、屏幕、输入编码、D2D 渲染、ConPTY、SSH 配置隔离、工作区模型和 Hyper-V 后台模块都已有代码。无需重做这些模块；下一阶段应集中修复集成问题、接通用户操作和补齐实机证据。

不能据目录数量或单测数量给出可信的完成百分比。尤其不能把 Hyper-V 库存在视为主界面已经支持 Hyper-V，也不能把本地终端模型存在视为保存的 PowerShell 配置已经能启动。

本次读取了固定提交的完整文件树，以及核心、传输、主界面、配置、构建和代表性测试源文件，进行了静态调用链核查。没有编译或运行该程序，没有连接或启动用户的 VM。当前环境 PATH 中未发现 CMake、cl 或 clang，因此本报告不声称完成 Windows 验证。仓库 STATUS 中的 Linux 86/86、ASan/TSan 和 mingw 通过属于项目自述，不是本次复跑结果，也不能自动视为当前 HEAD 的测试报告。GitHub Actions 查询返回 0 条运行记录，树中没有 .github/workflows；这只说明缺少该平台上的验证证据，不表示其他环境从未测试。

下文“已实现”指代码可定位；“代码缺陷”指可由当前调用链推导的问题，尚未实机复现；“待验收”表示不能仅凭静态代码判定。

## 2. 设计覆盖矩阵

| 设计部分 | 已看到的实现 | 仍有差距 | 判定 |
|---|---|---|---|
| T01 源码派生与来源 | UPSTREAM 固定 SHA，SOURCE_MAP 对照到上游函数，派生文件头 | 许可表述冲突；未见完整许可副本及选定上游文件哈希清单 | 部分完成 |
| T03/T04 核心构建与解析 | C++20/C ABI、VT 状态机、分块、上限、事件/EOF 测试 | CORE-01/02 输入向量依赖仓库外绝对路径 | 有实现，复现不完整 |
| T05 屏幕与输入 | 主/备用屏、SGR、Unicode/字素池、历史重排、选区、键盘/鼠标/粘贴编码 | 历史搜索未接；完整资源账本缺失；EOF 集成有问题 | 主要能力已有代码 |
| T03A/T07 传输与 SSH | ConPTY/Job、读写队列、关闭线程、独立 -F 快照、三种认证策略 | 会话 ID/快照碰撞；Windows SSH 和能力通路缺实测证据 | 有实现，不能验收通过 |
| T06 渲染与输入法 | D2D/DWrite、字体/DPI、IME、脏行、设备恢复代码 | 真机视觉、IME、混合 DPI、设备丢失和延迟待验证 | 有实现，待实机 |
| T08 工作区 | 会话树、标签、双向分屏、复制粘贴、重连归档、布局持久化 | 连接编辑器、历史搜索、完整设置界面未接；恢复行为违背设计 | 部分可用路径 |
| 本地 PowerShell | LocalSettings 与 LaunchSpec 类型已定义，空 ID 能启动本地 shell | 保存的 local 配置不能路由；参数、cwd、环境未贯通 | 集成缺口 |
| T09 Hyper-V | 查询、地址筛选、轮询/超时、后台传输与纯逻辑测试 | 主 UI 不注入 resolveHyperV；缺 VM/地址选择与启动操作 | 后台有实现，主流程未接 |
| 配置可靠性 | schema/revision、临时替换、备份、高版本只读、目录锁 | local.environment 不保真；加载校验与提示需补 | 部分完成 |
| 生命周期/性能/诊断 | 单窗格异步关闭、pump 时间预算、部分内存限额、stderr 诊断 | 解析仍在 UI 线程；全局内存预算、日志导出及性能基线未闭环 | 与详细设计有偏离 |

## 3. 优先修复的代码与集成问题

优先级：P1 = 阻塞主要使用路径或可能丢失输出，应在下一次试用前处理；P2 = v0.1 日常试用验收前补齐。编号用于后续追踪，不代表已经在 GitHub 创建 Issue。

### GAP-01 · P1：不同 SSH 窗格争用同一个快照文件

**证据：** [app/pane_backend/win32_pane_backend.cpp:330](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L330)、[app/pane_backend/win32_pane_backend.cpp:360](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L360)、[app/pane_backend/win32_pane_backend.cpp:382](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L382) 的启动均传入 `SessionId{1}`；每个后端的 attempt 从 1 开始。[platform/windows/ssh/ssh_transport.cpp:46](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/platform/windows/ssh/ssh_transport.cpp#L46) 按 `ssh-<sid>-<aid>.conf` 命名并用 CREATE_NEW 创建。主 UI 为所有连接提供同一个 snapshots 目录（[ui/shell_win32/main.cpp:865](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L865)）。

**触发与影响：** 第一个 SSH 窗格尚未结束时，双击另一连接、再开同一连接，或分屏新建 SSH 窗格，都会尝试创建 `ssh-1-1.conf`。第二次创建失败；崩溃遗留文件也可能影响下次启动。它直接阻塞设计中“同一配置打开多个独立 Session”。

**建议：** 为每个运行会话生成独立 session ID，每次重连递增 attempt；快照路径同时具备跨应用重启的唯一性，保持 CREATE_NEW。不要靠覆盖现有文件解决。

**验收：** 同配置和不同配置各开多个 SSH 窗格，交叉关闭/重连；快照互不覆盖、不互删。预置旧快照后仍能新建会话。

### GAP-02 · P1：结束会话时可能提前 EOF，丢失队列尾部输出

**证据：** [app/pane_backend/win32_pane_backend.cpp:413](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L413) 的 finishSession 只调用一次 pump，随后立即 itcore_finish_input。[app/pane_backend/frame_pump.h:14](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/frame_pump.h#L14) 给 pump 设置每轮 64 KiB / 8 ms 预算，预算用尽返回 FedMore；[app/pane_backend/win32_pane_backend.cpp:521](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L521) 只是安排定时续跑。[ports/core/src/core.cpp:338](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L338) 在 finished 后拒绝 feed，返回 FATAL。

**触发与影响：** 收到传输 Closed/Failed 时，输出队列还剩超过一轮预算的数据。第一轮后就标记 EOF，后续定时器拿到的剩余字节无法继续解析。错误报告或命令最后几行可能丢失，UTF-8 尾部也可能被过早判残缺。

**建议：** 将“传输已经结束”与“核心已经消费完尾部”分开。进入排空状态，允许有预算的多轮续跑；在传输最终关闭且队列确认清空后，才 finish_input、生成最终快照、发布 Exited/Failed。故障丢弃路径单独报告不完整。

**验收：** 模拟队列含 10 × 16 KiB 数据，末尾放唯一标记和跨块 UTF-8；先通知传输关闭，再处理 UI 队列，确认全部消费后才 EOF。现有 [tests/app/test_pane_logic.cpp:439](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/tests/app/test_pane_logic.cpp#L439) 只覆盖多轮 pump，不覆盖 finishSession 与 EOF 的组合。

### GAP-03 · P1：Hyper-V 后台已写，正式主界面没有接入

**证据：** 后端有 resolveHyperV 与 HyperVTransport 分支（[app/pane_backend/win32_pane_backend.cpp:331](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L331)），但 [ui/shell_win32/main.cpp:876](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L876) 的 makeFactory 只赋值 resolveSsh。[ui/shell_win32/main.cpp:851](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L851) 遇 Hyper-V 配置只使用 fallbackHost，否则直接返回“Hyper-V 地址解析尚未接入”。

**影响：** 即使 VM ID 配置正确且 VM 正在运行，从主界面连接也不会查询 Hyper-V。配置备用地址时会直接走该地址，没有执行设计中的解析、候选选择及回退判断。

**建议：** 复用现有后台模块，补 Connection → HyperVLaunch 映射并注入工厂；增加解析进度、多地址选择、明确的“启动并连接”操作及可见的手动回退。普通连接操作不能暗中启动 VM。

**验收：** 真机覆盖运行中 VM、关机 VM、VM 不存在、权限不足、模块缺失、地址变化、多个候选及取消。记录实际走自动解析还是手动回退。

### GAP-04 · P1：保存的本地 PowerShell 配置无法执行

**证据：** [ui/include/uicfg/model.h:36](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/include/uicfg/model.h#L36) 有 LocalSettings；但后端仅在 connectionId 为空时走 LocalT，其余最终请求 resolveSsh（[app/pane_backend/win32_pane_backend.cpp:309](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L309)）。主 UI 的 resolveSsh 明确拒绝非 SSH 配置。空 ID 默认使用 ComSpec/cmd，未读取已保存 local 的 arguments/workingDirectory。

此外 [ui/src/store.cpp:66](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L66) 加载 local 时不读取 environment；[ui/src/store.cpp:97](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L97) 保存时固定输出 inherit=true、set={}、unset=[]，不能保留设计中的环境覆盖。

**影响：** 空白“本地终端”可以有启动路径，但保存一项 PowerShell 连接并双击会失败；环境配置即使手工写入也不会生效，后续保存还可能重置它。

**建议：** 统一解析成区分 Local/SSH/Hyper-V 的启动描述，或增加 resolveLocal；完整传递 executable、arguments、cwd、inherit/set/unset，落实路径与环境校验。按照设计将 cwd=null 映射到用户个人目录。

**验收：** 从连接树启动 PowerShell，验证带空格/中文参数、显式 cwd、null cwd、环境 unset/set；保存再加载逐字段保持，程序不存在/目录不存在有明确错误。

### GAP-05 · P1：恢复工作区时自动启动所有有效连接

**证据：** [ui/shell_win32/main.cpp:927](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L927) 恢复标签后逐个 createPane；[ui/shell_win32/main.cpp:383](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L383) 对非缺失引用立即 startSession。

**与设计差距：** v0.3 §13.3 要求恢复为未连接状态，由用户点击连接；当前仅“引用缺失”的窗格不会启动。

**建议与验收：** 将布局构建和会话启动拆开，恢复时只建占位视图。保存一个包含四个 SSH 窗格的工作区后重启，应启动零个 ssh.exe；点击一个窗格才新建一次连接。首次安装是否自动开本地终端可作为独立规则保留。

### GAP-06 · P1：多行粘贴没有预览确认

**证据：** [ui/shell_win32/main.cpp:481](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L481) 从剪贴板读取后直接调用 backend->paste；[app/pane_backend/win32_pane_backend.cpp:638](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L638) 只做状态、大小和编码处理。未见设计 §12.4 要求的多行预览/确认路径。

**影响：** 32 KiB 限额和 bracketed paste 编码已实现，但不能替代用户确认。远端未启用 bracketed paste 时，多行文本可能直接执行。

**建议与验收：** 发送前对规范化后的多行内容展示预览，确认后只提交一次原事务，不额外补回车；取消应发送零字节。保留已有 NUL/内嵌标记清洗与大小上限。

## 4. v0.1 交付仍需补齐的部分

### GAP-07 · P2：会话管理的模型领先于用户界面

主 UI 已有会话树筛选、双击连接、多标签、分屏、复制粘贴和字号调整，不是完全的占位壳。顶层 CMake 构建会链接真实 PaneBackend；只有单独构建 ui 子项目才启用 stub（[ui/CMakeLists.txt:29](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/CMakeLists.txt#L29)）。

但在当前主程序入口和消息分派中未找到连接新建/编辑/删除、分组管理、VM 选择、完整字体/主题/快捷键设置界面。不能用 uicfg 的模型及单测替代这些入口。历史搜索尤其明确未接：[ui/shell_win32/main.cpp:541](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L541) 直接注释 searchHistory 未实现。

**建议：** 先补能完成“新建 Ubuntu 连接→保存→连接→编辑→再次连接”的最小编辑器；再补当前 Session 历史搜索（结果跳转、软换行映射、取消与限额）和主题/快捷键设置。发布前提供经过校验的示例配置与首次运行说明。

另有明确交互偏离：设计默认 Ctrl+C 始终发送中断；当前默认 copyOrInterrupt 在有选区时复制（[ui/src/settings.cpp:23](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/settings.cpp#L23)、[ui/shell_win32/main.cpp:531](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/shell_win32/main.cpp#L531)）。应恢复设计默认，或将该改变作为明确的产品决策记录。

### GAP-08 · P2：核心仍在 UI 线程运行，预算不等于完成线程隔离

[app/pane_backend/win32_pane_backend.cpp:142](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L142) 明确 pump 在 UI 线程执行；feed、事件处理、快照和 present 也在这条路径。64 KiB / 8 ms 与低优先级定时器是已落实的响应性改进，但设计 §6/§15 要求每会话执行器串行修改核心、UI 消费快照。

当前预算检查以块为边界，不能中断一次耗时 feed；后续快照、重排和绘制也不全在该 8 ms 范围内。多窗格输出仍竞争同一 UI 线程。

**建议：** 将其标为临时架构偏离。先在真实 Windows 上测四窗格持续输出、键盘延迟、缩放和关闭；按设计迁移到会话执行器，或基于测量形成正式 ADR 决定保留何种模型。不能只凭 PUMP-01 单测宣称延迟达标。

### GAP-09 · P2：有局部限额，没有看到完整的会话/应用内存账本

历史、字素池、输入/输出队列、粘贴及旧 attempt 文本已具备局部限制。[ports/core/src/screen.cpp:583](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L583) 将 memory_limit 交给历史，并另外分配字素池预算；[ports/core/src/core.cpp:583](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/core.cpp#L583) 快照分配不经全局额度申请。在已审阅的启动、核心、快照和主 UI 路径中未见设计中的 Session 128 MiB / 应用 512 MiB 统一账本与受控拒绝。

后端默认 5000 行/32 MiB（[app/pane_backend/win32_pane_backend.h:27](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.h#L27)）与设计建议 10000 行/64 MiB 也不同。初始值可以调整，但要记录并统一，不能将其称为完整 Session 实际存活内存上限。

**建议与验收：** 为真实存活对象记账，覆盖旧快照仍持有的池、历史、队列及归档；新增压力下淘汰和拒绝新建策略。多会话接近限额、100 次重连及滞留快照时，验证没有虚扣、无限增长或悬空引用。图形显存与进程 Private Bytes 分开统计。

### GAP-10 · P1 验收门槛：Windows 通路、TERM 与端到端行为没有闭环证据

[docs/T03A-Windows规格-v0.1.md:6](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/docs/T03A-Windows规格-v0.1.md#L6) 明确全部条目未实机验证；[docs/STATUS.md:137](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/docs/STATUS.md#L137) 列出 ConPTY、D3D、Windows SSH、IME 等待测项。代码中有探针，不等于已有 CAP-01 的 ConPTY 前后字节、TERM/terminfo 与应答归属记录。

设计将 T03A 设为冻结 T05/传输职责之前的门槛，实际代码已推进到 T09，但对应实验仍缺证据。应将它视为需要尽快补上的架构验证，而非留到最后的形式测试。DA2 当前使用上游默认应答（[ports/core/src/screen.cpp:1265](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.cpp#L1265)），也需要结合实际实现和通路核对，不应仅因上游如此回复就认定兼容能力已达成。

**最小实机包：** 固定 Windows/Ubuntu/OpenSSH/字体/DPI/GPU 版本，验证本地 PowerShell、Ubuntu SSH、vim/top/tmux、中文 IME、鼠标、32 KiB 粘贴、备用屏、尺寸同步、断网、连续输出中退出、100 次关闭循环。保存 CAP-01 能力矩阵、尾部字节记录、进程/句柄趋势及输入延迟分位数。

### GAP-11 · P2：干净环境构建和测试复现仍不完整

[CMakeLists.txt:48](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/CMakeLists.txt#L48) 的 CORE-01/02 输入默认指向 `/workspace/qa/vectors/core01_02.json`；文件不存在时只 WARNING 并跳过。仓库虽有 golden 输出，却没有对应这份输入。干净检出可能得到“测试通过”但实际少跑基础向量。

[ui/CMakeLists.txt:5](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/CMakeLists.txt#L5) 在非交叉编译时默认 find_package(nlohmann_json)，不会自动使用已随仓库提交的头文件。它不必然造成构建失败，但新机器需要额外配置，当前没有根 README 说明完整依赖及主程序路径。

**建议：** 把所有必要测试数据纳入仓库；必需测试缺数据时失败而非静默减项。提供 Windows 原生构建预设、依赖说明、运行入口、测试命令和产物位置；接入 Linux 核心测试与 Windows 编译/可自动化生命周期 CI。每次报告写明提交 SHA、注册/执行/跳过用例和附件。

### GAP-12 · P2，分发前解决：来源与许可材料需要统一

优点是已经固定上游 `f29420ae4d3e0f986ee24ea286885d32af58175a`，SOURCE_MAP 还记录了函数级差异，不能把本项目简单判作没有来源记录的重写。

但当前 [UPSTREAM.md:4](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/UPSTREAM.md#L4) 写“GPL v2 或更新版本（整体按 GPL v3）”，[ports/core/SOURCE_MAP.md:4](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/SOURCE_MAP.md#L4) 及 [ports/core/src/screen.h:1](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ports/core/src/screen.h#L1) 写 GPL-2.0-only。固定上游的 [COPYING](https://github.com/gnachman/iTerm2/blob/f29420ae4d3e0f986ee24ea286885d32af58175a/COPYING) 说明 v2 或更新、上游整体因依赖而实际适用 v3。这些仓库内表述不能同时作为清晰的分发说明。

当前树中未见项目根的 LICENSE/COPYING、所选上游文件逐项哈希清单；只有 nlohmann_json 自带 LICENSE。未见单独的依赖闭包审计产物。

**建议：** 按实际移植文件与实际引入依赖核对来源及适用许可，统一 SPDX/根文档/分发说明，补齐许可文本、原作者与修改声明、上游路径和文件哈希。不要仅根据上游全量应用的依赖结论，推断本项目实际依赖组合的最终许可。这是材料与一致性审查，不是法律结论。

### GAP-13 · P2：诊断输出尚未达到设计中的可支持性

后端 onDiagnostic 直接 fprintf(stderr)，且忽略回调传入的 session/attempt（[app/pane_backend/win32_pane_backend.cpp:306](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L306)）；主窗口目标是 WIN32 GUI。尚未看到设计要求的 5 MiB × 3 滚动日志、诊断包预览/脱敏导出及完整性能记录入口。

**建议：** 先实现元数据日志：版本、唯一 session/attempt、状态、阶段、耗时和错误码；保留原始终端输出默认不落日志。日志与用户可见错误串联，使 Windows 试用问题能定位到具体会话。

## 5. 另外需要纳入回归的边界

这些不替代前述主线，但适合在修对应模块时一并处理。

- **NeedDrain 零进展：** [app/pane_backend/frame_pump.h:42](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/frame_pump.h#L42) 在 accepted=0 时把当前剩余块计为已消费。若返回 NEED_DRAIN，契约应是排空事件后重试同一段字节，并设置有界的无进展故障处理。不能为避免忙循环而跳过数据。当前后端经常排事件，触发条件仍应专门构造验证。
- **缺少 ssh.exe 的状态：** [platform/windows/ssh/ssh_transport.cpp:38](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/platform/windows/ssh/ssh_transport.cpp#L38) 在客户端不存在时只发送 onFinished 并返回 nullopt；[app/pane_backend/win32_pane_backend.cpp:373](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/app/pane_backend/win32_pane_backend.cpp#L373) 的 onFinished 仅保存 sshExit，没有显式结束状态路径。应验证窗格不会一直留在 Starting，并让所有启动失败进入同一清理/展示流程。
- **配置加载校验：** [ui/src/store.cpp:109](https://github.com/yinzhaoxuan/termv2/blob/cd71b7ee1f1da122b0be869be24b79f078d62b94/ui/src/store.cpp#L109) 解析后直接返回 Ok，保存才调用 validate；非法端口在主 UI 被转换为 uint16_t。应在加载和启动前统一检查端口、超时、唯一 ID 和引用；不要让类型转换把非法配置变成另一个有效目标。
- **文档陈旧：** STATUS 中仍把“退出 shutdown 钩子”“SSH 错误尾部旁路”“固定字体度量”等列为待处理或旧状态，但当前源码已包含相应修复；SOURCE_MAP 也保留一些被后文覆盖的旧限制。更新为单一当前状态，并给变更历史单独位置，避免重复实现。

## 6. 推荐推进顺序与验收产物

| 批次 | 工作 | 本批退出标准 |
|---|---|---|
| A：修复会话基础 | GAP-01 ID/快照隔离；GAP-02 EOF 排空；补启动失败与零进展回归 | 两个 SSH 窗格并存且重连不互扰；大量尾部输出完整；失败不挂在 Starting |
| B：打通主要使用场景 | GAP-03 Hyper-V 注入；GAP-04 本地 PowerShell 路由；GAP-05/06 恢复与粘贴行为 | 从主程序完成本地 PowerShell、手动 SSH、Hyper-V 发现并连接三条路径 |
| C：同步开展 Windows 验证 | T03A/CAP-01、SSH-CFG-01、WIN-01/02/03、VM-01、DPI/IME | 固定提交的结果表、能力矩阵、日志/截图与失败项；通路不满足则先修架构 |
| D：补齐日常操作 | 连接编辑、分组、历史搜索、字体/主题/快捷键；诊断日志 | 不手改 JSON 即可完成最常用配置；搜索和错误恢复可操作 |
| E：资源与交付 | 会话执行器或正式 ADR、全局内存账本、可复现构建/CI、来源与许可材料 | 性能/内存实测达标，干净环境能构建并执行完整必需测试 |

C 应在 A/B 的最小可测路径具备后立即开始，不应等 D/E 完成再验证。暂不扩大到 SFTP、图片协议、复杂 shell integration 或重换 UI 框架。

## 7. 设计文档下一次更新建议

本次提交仅新增根目录 REVIEW.md，记录上述固定提交的评审结果，不修改实现代码或原设计。后续设计 v0.4 应做到：

1. 保留 v0.3 的目标和契约，追加“固定实现 SHA、代码已实现、实机验收、证据链接”四列，不再把 T01–T09 全部标为待实施，也不全部标完成。
2. 登记明确偏离：UI 线程核心执行、Win32 当前外壳、Ctrl+C 默认行为、5000/32 MiB 初始预算；分别写出恢复设计或接受偏离的依据。
3. 将本报告 GAP-01–13 关联到设计条款和验收用例，防止“库已实现”与“用户可完成操作”混淆。
4. 对已修复的历史问题保留变更记录，从当前待办中移除；所有“通过”记录必须绑定提交与测试环境。
