# iterm2-win 进度汇总

更新时间：2026-10-23（Asia/Shanghai）

本文件跟踪从核心/传输到最小 Win32 窗的提交进度，以及**尚未在真实 Windows 机器上验证**的项。

## 最近提交（摘要）

| 提交 | 内容 |
|------|------|
| `9d7f716` | T05 屏幕模型 Grid/Cell/Scrollback；CSI 默认参数与 SGR；快照 ABI；CORE-03/04/05、MEM-01 子集 |
| `aab3c7e` | T06+T05 评审：`encode_*`、D2D/DWrite 原型、宽字孤儿与 `wrap_pending` 组合、快照 `generation`、ConPTY `failStart` 清 reader |
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
3. **`itcore_mode_query(core, mode)`** → `ITCORE_MODE_NOT_RECOGNIZED/SET/RESET`（0/1/2，与 DECRPM Pm 一致），覆盖 1 7 9 25 47 1000 1002 1003 1006 1047 1049 2004。`CSI ? Ps $ p` → `CSI ? Ps ; Pm $ y`。单测 CORE-11。
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
5. **DECRQM ANSI**：`CSI Ps $ p`（不带 ?）与私有模式分开处理，回复 `CSI Ps;Pm$y`。4（IRM）和 20（LNM）回 1 或 2，其余回 0；ANSI 25 与 `?25` 互不相干。新实现 LNM（LF/VT/FF 兼回车）。单测 CORE-13 覆盖 2004、1049、25 的开和关，`?9999`→0、`9999`→0，以及 4 和 20。

### 接口问题（未改头文件，待 UI 定夺）
1. “UI 翻译按键”与子窗焦点冲突：子窗把 WM_KEY*/WM_CHAR SendMessage 给父窗，IME 留在子窗处理。
2. **UI 须在退出时调用 `it2app::shutdownPaneBackends(3000)`**：在所有窗格销毁之后、WinMain 返回之前调用。头文件里没有对应的跨平台入口，建议在 `PaneBackendFactory` 旁边加一个 shutdown 钩子。
3. Shift+PgUp 等滚动快捷键由 UI 调 `scrollViewport`。后端不处理键盘滚动，因为按键会转发给父窗。
4. SSH 下 hasForegroundJob 无法看到远端，Running 期间恒为 true（仅供参考）。
5. attach 在会话启动失败时仍返回 true（状态 Failed）。
6. 词分隔符目前写死在核心，尚无配置接口。LNM 下回车键是否发 CR LF 未实现（上游 newlineMode），目前只影响输出侧的 LF。
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
