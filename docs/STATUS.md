# iterm2-win 进度汇总


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
1. **关闭不阻塞 UI**：`requestClose` 把关闭交给本会话的关闭线程后立即返回。该线程调 `transport.close(User)`，由传输层 closer 执行 T03A §1.4 的 9 步和 3 s 强杀，再 `waitClosed(5 s)`。结果 post 到 UI 队列，UI 线程 `finishSession`（排空输出、finish_input、最终快照）后发 `onStatus(Exited)`，超时发 `onStatus(Failed, "关闭超时，已强制结束")`。超时后线程继续等传输真正结束，并在该线程上析构。析构函数先 close 队列，再把传输交给关闭线程（若尚未开始），只释放自己的引用，**不 join、不等待**。多窗格各自一条线程，并行关闭。单测 PANE-01：立即返回、只经 drain 送达、8 个会话并行（≈单个耗时）、超时路径、关闭中销毁不回调。
2. **粘贴**：直接 `itcore_encode_paste`，由核心按 DECSET 2004 加标记并清洗 NUL 与内嵌起止标记（7 位 / C1，循环删除），已删除 `bracketedFromProbe`/`buildPaste`/`sanitizePasteBody`。核心返回 -2 或传输返回 TooLarge 时调 `onPasteRejected(bytes, 32768)`。单测 ENC-02。
3. **`itcore_mode_query(core, mode)`** → `ITCORE_MODE_NOT_RECOGNIZED/SET/RESET`（0/1/2，与 DECRPM Pm 一致），仅 DEC 私有模式，覆盖 1 4（DECSCLM，仅记录） 7 9 25 47 1000 1002 1003 1006 1047 1049 2004。`CSI ? Ps $ p` → `CSI ? Ps ; Pm $ y`。单测 CORE-11。ANSI 模式另用 `itcore_ansi_mode_query(core, mode)`（4 IRM、20 LNM），保持原函数签名以稳定 C ABI；DEC ?4 与 ANSI 4 互不影响，CORE-13 覆盖。
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
   - 滚轮每格 3 行。鼠标模式下转发 WHEEL_UP/DOWN（Shift 强制本地）；备用屏无鼠标模式时发上/下方向键；否则滚动视口。
   - 双击选词：词字符为字母数字、非 ASCII 以及 `/-+\~_.`（上游默认），其余标点单独成词。
   - 三击选逻辑行（跨软换行）。双击/三击之后的拖动按同一单位扩展，新 ABI 为 `itcore_selection_start_unit`。
   - 拖出上下边缘时，每 50ms tick 自动滚动 1 行。
   - `pane_logic` 中的 `Viewport`、`ClickCounter`、`routeWheel` 由 PANE-03 覆盖。
5. **DECRQM ANSI**：`CSI Ps $ p`（不带 ?）与私有模式分开处理，回复 `CSI Ps;Pm$y`。4（IRM）和 20（LNM）回 1 或 2，其余回 0；ANSI 25 与 `?25` 互不相干。新实现 LNM（LF/VT/FF 兼回车）。单测 CORE-13 覆盖 2004、1049、25 的开和关，`?9999`→0、`9999`→0，以及 4 和 20；LF/VT/FF 在 LNM 开和关时的列位置、Enter/Alt+Enter 编码均有对照。Viewport::scroll 用 int64 截断，PANE-03 覆盖 INT_MIN/INT_MAX。

### T09 Hyper-V 辅助（实现）
- `platform/windows/common/hyperv_resolve.*`（平台无关）：VM ID 规范化（仅 GUID）、查询 JSON 解析（v=1，标量/数组、BOM）、地址筛选（排除 0.0.0.0、127/8、169.254/16、::、::1、无 scope 的 fe80::/10）、候选选择、`HyperVResolver` 状态机与 `runHyperVResolution`（轮询 2 s、单次查询 5 s、预算 60 s；只在策略允许时启动一次；多候选不盲选；VmNotFound 不回退，AccessDenied/模块缺失/启动失败可回退手动地址）。
- `platform/windows/hyperv/`：固定 PowerShell 脚本（-EncodedCommand，VM ID 走环境变量）+ Job/超时/取消；`HyperVTransport` 后台解析后启动内层 SshTransport。
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
