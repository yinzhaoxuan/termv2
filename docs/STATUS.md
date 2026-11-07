# iterm2-win 进度汇总

交接说明见 [HANDOFF.md](HANDOFF.md)（目录、构建、架构）。剩余工作见 [TODO.md](TODO.md)。本文件先给「当前状态」，下面是按日期倒序的进度流水。2026-10-23 前后的旧快照已移到 [HISTORY-2026-10.md](HISTORY-2026-10.md)，内容已过时，只供追溯。根目录 `REVIEW.md` §8 的勾选已回写到 2026-11-04。

## 2026-11-07：QA 配置反馈与首次信任提示修复

基线 `0ff2335`，设计见 [DS-19](DS-19-QA配置与首次信任提示.md)，逐项结论见 [QA 清单](qa/buglist-20261106.md#2026-11-07-修复核查保留原始报告)。

- Direct 未知主机：能提供主机密钥信息时引导「核对指纹…」，没有信息时说明无法在此确认；不改变主机密钥校验或信任写入流程。
- 配置加载：缺失 `schemaVersion` 给出明确原因；启动状态栏与调试输出提示配置损坏、备份恢复或高版本只读。权限默认关闭及原文件保护规则保持原样。新增有效/无效 OSC 52 配置与加载提示的回归用例。
- QA 复核：P3 前留存的设置文件缺 `schemaVersion`；已补完整示例与测试前有效配置检查。原资源增长步骤会保留新标签，不是同窗格重连，已修订手测步骤并保留原始观测。
- Windows 测试兼容：黄金文件比对显式使用 UTF-8，并统一输出行尾，修正中文系统代码页与 CRLF 造成的误报；JSON 内的转义控制字符仍精确比较。
- 本机使用便携 LLVM-MinGW 20261104（Clang 23.1.5）、CMake 4.4.5、Ninja 1.13.2，Windows x64 Debug 完整构建通过（含 `it2w_shell.exe`，`IT2W_SSH_DIRECT=OFF`）。CTest 共 171 项：首轮 169 项通过；`extra.golden` 的编码/行尾问题修正后单独通过；`PANE-11` 首轮触发人为设置的 60 秒超时，单独放宽到 240 秒后于 104.84 秒通过。最终 171 项全部通过，含配置、Direct 错误映射、核心 OSC 52 与读确认门的回归。测试临时目录显式设在工作目录，避免沙箱默认临时目录差异；未运行私有 SSH 实验环境的在线验收。
- **未完成的实机验收**：正式签名；有效配置下的 OSC 52 本地/Direct 写和 Direct 读确认；固定窗格数的重连资源测量。未构建 Direct 依赖，不把纯逻辑测试当作 Direct 产品验收；外部 CORE-01/02 向量仍缺失。

## 当前状态（2026-11-04）

| 模块 | 现状 | 验证 |
|---|---|---|
| 终端核心 `ports/core` | VT 解析、屏幕与历史（10000 行 / 64 MiB 默认）、SGR 子参数与下划线样式、UCD 16 宽度、OSC 8 / 52、DA / DECRQSS / XTVERSION / XTGETTCAP（VT220 身份）、选区、搜索、编码（键 / 鼠标 / 粘贴） | Linux / macOS ctest；CORE-01/02 外部向量未入仓 |
| 渲染 `renderer` | D2D / DWrite，脏行画布，光标与 IME，配色读自核心（DS-9） | 逻辑单测；像素效果未实机 |
| 窗格后端 `app/pane_backend` | pump 状态机（ADR-001）、ConPTY / 兼容 SSH / Direct SSH / Hyper-V、诊断日志 5 MiB × 3、OSC 52 读逐次确认（DS-10）、丢弃字节上报、会话 / 应用内存账本（DS-17） | 纯逻辑单测；Windows 只经 CI mingw 编译 |
| 传输 `platform/windows` | ConPTY、`ssh.exe` 快照、libssh2 Direct（含首次信任 DS-12）、Hyper-V 解析；默认传输按 ADR-006 选择（内置默认仍兼容） | Direct 由 CI `mingw-direct` 编译（ADR-006 起每次推送） |
| 主界面 `ui` | 会话树与连接编辑器（DS-14）、标签 / 分屏、历史搜索、多行粘贴确认、外观 Profile 与 `.itermcolors`（DS-9）、按连接选择传输（DS-13）、快捷键展示（DS-16）、诊断包导出（DS-15） | uicfg 单测；Win32 代码只经 CI mingw 编译 |

- **测试**：macOS 本机 ctest 172 项全过；CI 每次推送跑 linux / asan / tsan / mingw。
- **最大缺口**：上表「未实机」的部分全部没有在 Windows 上跑过。手测清单汇总在 [TODO.md](TODO.md) §1（每个设计文档的「只能在 Windows 上验」一节）。手测结果请写进本文件，并附「导出诊断包…」里的提交号与 Windows 版本（REVIEW C6）。
- **许可**：GPL-3.0-or-later（[LICENSE-AUDIT.md](LICENSE-AUDIT.md)）。
- **默认值**：SSH 默认仍走兼容模式（`ssh.exe`）；Direct 改默认待 ADR（TODO §2.1）。OSC 52 读写默认全关。

## 2026-11-05：DS-18 字形运行渲染（E8）

见 [DS-18-字形运行渲染.md](DS-18-字形运行渲染.md)（四轮审查后批准）。

- 文本从逐格 `DrawTextW` 改为按段塑形、按格定位：`AnalyzeScript` / `AnalyzeBidi`（只取层级，不重排）/ `MapCharacters` → `planItems`（每个不降级的核心格完整落在一个塑形项里）→ `GetGlyphs` / `GetGlyphPlacements` → `placeOnGrid` → `DrawGlyphRun`。不做位图图集。
- 连字：settings.json 隐藏字段 `fontLigatures`（默认关），字体预设加「Cascadia Code」。块光标格单独成段，反色字形复用底层的绘制计划。
- 彩色字形：COLR v0 经 `TranslateColorGlyphRun`，0xFFFF 层用当前文本颜色。
- 塑形缓存 4 MiB（键含核心格分组）；`pane-diag.log` 每 10 秒一条 `render.stats`（文本 CPU 时间按无文本 / 部分 / 全屏分类的 p50 / p95 / max、缓存命中率、降级计数）。诊断包摘要加 `fontLigatures`。
- 旧路径原样保留：`ITERM2WIN_TEXT_PATH=drawtext`，作基线与退路，下个版本删除。删除了从未使用的 `glyph_cache.h`（图集、`plan_runs`）。
- 测试：`text_runs.h` 纯函数（分段、RTL 判定、`planItems` 不变量、定位、缓存、隔离、计划、计时统计）；settings、诊断包。macOS ctest 172 项全过。
- **未验证**：`renderer.cpp` 的 DirectWrite 胶水只能由 CI `mingw` 编译；画面、连字、emoji、复杂文字和耗时见手测清单 P2-2a、P3-2a。

## 2026-11-05：ConPTY 复审修复

见 [审查清单-2026-11-05-复审.md](审查清单-2026-11-05-复审.md)。

- 启动与关闭的交接：新增 `lifeLock_`，`close()` 读状态与登记请求成为一个整体；`publishRunning()` 在锁内发布 Running 并取走 Starting 期间的请求。快速退出的子进程不会再停在 Running。
- 事件创建失败：构造时记下错误码，`start()` 在建任何线程、进程前走 `failStart`；closer 只在 `WAIT_OBJECT_0` 时清理。
- **未验证**：只做了静态复查，待 CI `mingw` 编译和 Windows 故障注入。

## 2026-11-05：ADR-006 SSH 默认传输（阶段 1）

见 [ADR-006-SSH默认传输.md](ADR-006-SSH默认传输.md)。用户已批准；阶段 1 不改变现有用户的行为（内置默认仍是兼容）。

- 选择规则：连接 `ssh.transport` > 环境变量 `ITERM2WIN_SSH_DIRECT`（`1` 直连 / `0` 兼容，新增 `0`）> settings.json 隐藏字段 `sshDefaultTransport`（`auto` / `compatible` / `direct`）> 内置默认 `kBuiltinSshDefault`。`auto`：构建含 Direct 且 agent 认证 → 直连，否则兼容。纯函数 `resolveSshTransport` 取代 `wantDirectSsh`。
- 直连失败、且不是连接自己写明的：`errorText` 末尾说明来源（环境变量 / settings / 默认）和改回 `ssh.exe` 的办法。「此构建未包含直连 SSH。」和「直连 v0 仅支持 agent 认证」两条文字只说原因，改法按来源追加。
- 每次启动 SSH 写诊断 `ssh.transport direct=0/1 source=…`；诊断包配置摘要加 `sshDefaultTransport`；settings 值非法时启动状态栏提示一次，原值保存时原样写回。
- 连接编辑器：传输第一项显示「跟随全局默认（当前：直连 / 兼容）」，切换认证方式时刷新。
- CI：`mingw-direct` 每次推送都跑，缓存 `build-mingw-direct/ssh-a-deps`。发布包用这个构建。
- 测试：`resolveSshTransport` 全表（连接 × 环境变量三态 × settings 四态 × 内置三态 × 构建 × 认证）、`sshDirectEnv`、来源提示与诊断文字；settings 往返与 8 种非法值；编辑器标签；诊断包摘要。macOS ctest 172 项全过。
- **未验证**：`win32_pane_backend.cpp`、`connection_dialog.cpp`、`main.cpp` 的改动要等 CI `mingw` / `mingw-direct`；缓存是否命中要看第二次运行；Windows 手测见手测清单 P1-3 新增的五项。

## 2026-11-05：实现审查修复

见 [审查清单-2026-11-05.md](审查清单-2026-11-05.md)。

- ConPTY（`platform/windows/conpty/conpty_transport.cpp`）：closer 改为在 `start()` 时建好的常驻线程，`close()` 只置位 `closeRequest_`，工作线程发起关闭不会再等待自己；`doClose` 在 `ClosePseudoConsole` 后一直等写线程退出才置位 `closedEvent_`；monitor 先于关闭 `process_` join；无写线程时关闭 `inWrite_`；析构函数先等所有线程退出。只做了静态复查，待 CI mingw 编译和 Windows 实机验证。
- 核心编码：Ctrl+`@ [ \ ] ^ _` 发 `00 1B–1F`。新增 encode 与 planKey 链路测试，ctest 171 项全部通过。

## 2026-11-05：Windows 实机手测清单与文档回写

- [WINDOWS-手测清单.md](WINDOWS-手测清单.md)：把各设计文档、STATUS B 批、HANDOFF §4.6、REVIEW C 批的手测项合并，按 P0–P3 排好，写明期望、出处与回报格式。CI 的 mingw / mingw-direct 任务改为上传 `it2w_shell.exe` 与 `licenses/`（`89e5f52`，首个 artifact 已生成）。
- 文档回写到 11-05：HANDOFF（11-05 更新说明、环境变量语义、第 4–6 节）、README 文档索引、REVIEW §8 C 批指向清单、REVIEW 增补 10-31 回写表、ROADMAP（OSC 52 读、F-1、F-4、Direct 补齐）。
- CI：`89e5f52` 全绿（linux / asan / tsan / mingw）。`mingw-direct` 仍待手动触发。

## 2026-11-05：DS-17 会话与应用两级内存账本（E3）

设计：[DS-17-内存账本.md](DS-17-内存账本.md)。

- **拉取式计量**（不预扣、不归还）：新增核心 API `itcore_memory_usage_get`（网格 / 历史 / 字素池 / 事件与解析器缓冲 / 搜索）、`itcore_snapshot_row_bytes`、`itcore_snapshot_pool_bytes`、`itcore_set_history_limits`（运行时调低历史上限并立即淘汰，池预算随之）。ABI 兼容新增，版本仍 0，ABI-02 已补断言。
- 会话合计 = 核心 + 渲染器当前快照（快照持有的字素池与核心同 epoch 不重复计；旧 epoch 的 store 计入）+ 旧 attempt 归档。传输队列不计（`io_queues` 固定上限，每会话 ≤ 约 8.1 MiB）；显存不计。
- **策略**（`uicfg::balanceMemory`）：单会话 > 128 MiB 调低其历史上限；合计 > 512 MiB 从历史最大的会话开始压，每个不低于 4 MiB。调低是粘性的（重连 / 新建前不回调）。UI 每 2 s 计量一次，缩减时状态栏提示一次。
- **受控拒绝**：新建标签、分屏、点「连接」前检查合计 + 16 MiB 是否超过 512 MiB，超过则弹框拒绝；重连与首次启动的本地窗格不检查。诊断包增加账本合计、会话数与最大值。
- macOS ctest 172/172；新增 MEM-02 ×3、PANE-32 ×3（含 10 会话各填满 64 MiB 历史后压到应用上限、100 次快照滞留循环）、uicfg_memory_ledger，在 ASan / TSan 下通过。
- **未验证**：`win32_pane_backend.cpp`、`main.cpp` 等 CI `mingw`；Windows 手测见 DS-17 §6。

## 2026-11-05：DS-8 余项 ABI 版本规则（ADR-005）

- [ADR-005](ADR-005-itcore-ABI版本规则.md)：`ITCORE_ABI_VERSION` 只在不兼容变化时加 1（删改函数 / 签名、已有字段布局或含义、已有枚举值、收紧契约）；新增函数、枚举末尾追加、启用保留位、调用方结构体末尾追加字段（配最初尺寸常量）都是兼容变化。此前的 SGR 扩展位、新事件、新函数、DS-10 生存期说明都属兼容，版本保持 0。
- `itcore.h` 开头写明调用方义务（忽略未知事件类型但照常释放、忽略未知 `flags` 位、`struct_size = sizeof`），并标出 `itcore_cell.flags` 已用 bit 0–13、保留 bit 14–15。
- 新增 `ABI-02.layout`：257 条 `static_assert` 覆盖 12 个公开结构体的全部字段偏移 / 尺寸、15 组枚举的 98 个值与 5 个常量宏（64 位目标）。任何布局或数值变化都会编译失败，迫使改动者按 ADR-005 §4 判断并在提交信息里写明。
- macOS ctest 165/165。

## 2026-11-04：E6 许可统一（GPL-3.0-or-later）

- 项目所有者决定整体采用 **GPL-3.0-or-later**：上游 iTerm2 为「GPL v2 或更新版本」（其 `COPYING` 说明实际按 v3）；Direct 静态链接的 OpenSSL 3（Apache-2.0）与原先标的 `GPL-2.0-only` 不兼容。
- 根目录 `LICENSE`（GPLv3 全文，SHA-256 与 gnu.org 一致）；`third_party/unicode/LICENSE.txt`（Unicode License v3）。
- SPDX：201 个项目源文件统一为 `GPL-3.0-or-later`（新增 188、替换 13 个 `GPL-2.0-only`）；生成的 UCD 宽度表与大小写折叠表注明 Unicode 数据来源（生成脚本同步）。
- 新增 `it2w_license_notices`：所有 Windows `it2w_shell` 构建都把 termv2 / nlohmann_json / Unicode 许可与 THIRD-PARTY-NOTICES 放进 `licenses/`（此前只有 Direct 构建有）。
- 文档：[UPSTREAM-FILES.md](UPSTREAM-FILES.md)（19 个参照过的上游文件在固定提交上的 git blob 与 SHA-256，按「移植来源 / 参照」分类）、[LICENSE-AUDIT.md](LICENSE-AUDIT.md)（依赖闭包与分发清单）；UPSTREAM、THIRD-PARTY-NOTICES、SOURCE_MAP、README 表述统一。
- 未完成：MinGW-w64 winpthreads / CRT 的许可文本尚未自动打包（LICENSE-AUDIT §5）。本文不是法律意见。
- macOS ctest 164/164。**未验证**：`it2w_license_notices` 等 CI `mingw`。

## 2026-11-04：E7 文档整理

- STATUS 顶部新增「当前状态」表；2026-10-23 前后的后半部分（模块状态、T08 / T09、P0 批次、接口问题、未实机项、构建提示）原样移到 [HISTORY-2026-10.md](HISTORY-2026-10.md)，开头列出已知过时的说法。HANDOFF 里引用这些章节的 4 处改指 HISTORY。
- `ports/core/SOURCE_MAP.md`：宽度表（UCD 16 生成表）、快照共享字素池（`PoolRef`）、已实现的 OSC 8 / 鼠标 / 粘贴编码、历史显示行分组计数（NEW-02）改为现状；删去旧机器的本地路径。

## 2026-11-04：NEW-05 余项 `droppedOutputBytes`

- `uishell::SessionStatus` 末尾加 `droppedOutputBytes`（ADR-001 §2.8 / §9.5）：终态时由后端填入，重连清零；`errorText` 仍附「输出可能不完整，丢弃字节数=N」，向后兼容。
- 状态栏：当前窗格 Exited / Failed 且有丢弃时追加「输出不完整（丢弃 1.5 KiB）」（`uicfg::droppedOutputNote`，与区域设置无关）。`SessionStatus` 的初始化改为指定初始化（`{.state = Starting}`）。
- macOS ctest 164/164。**未验证**：`win32_pane_backend.cpp`、`main.cpp` 等 CI `mingw`；实机触发需洪峰关闭（HANDOFF §4.6）。

## 2026-11-04：DS-10 OSC 52 读剪贴板逐次确认（NEW-04）

设计：[DS-10-OSC52读确认.md](DS-10-OSC52读确认.md)。

- `osc52.allowRead=true`（仍默认关）时，Direct 会话里的读请求不再立即应答：后端保留该请求事件，经延迟事件通知 UI（不在 pump 栈里弹框，ADR-001）；UI 弹 TaskDialog（「允许」「拒绝」，默认拒绝；「本会话记住此选择」），用户允许后**此刻**才读剪贴板并应答。一次最多一个待决请求，其余记 `osc52 read busy` 并丢弃。「本会话」= 当前 attempt，重连 / 新会话 / 销毁都清掉。没有接确认回调的前端一律拒绝。
- Direct 改为 `supportsOsc52Read=true`（取代 D-1 的临时关闭）；ConPTY 不变（收不到读请求，ADR-002）。
- 写入：每个 attempt 第一次经 OSC 52 成功写剪贴板时，状态栏提示「「<连接名>」中的程序写入了剪贴板（OSC 52）」。
- 可移植：`ClipboardReadGate` 状态机、`clipboardReadPrompt` 文案（复用粘贴预览规则）；真实核心上验证读请求事件可在之后的 feed 之后再编码应答（BEL / ST 结尾）。`itcore.h` 只补注释。
- macOS ctest 164/164（新增 10 项）。
- **未验证**：`win32_pane_backend.cpp`、`main.cpp`、`clipboard_prompt.cpp` 等 CI `mingw`；`direct_ssh_transport.h` 只在 `mingw-direct` 里编译（需手动触发）；Windows 手测见 DS-10 §7。

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
