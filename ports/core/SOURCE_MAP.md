# ports/core 上游来源对照（SOURCE_MAP）

上游：https://github.com/gnachman/iTerm2 固定提交 `f29420ae4d3e0f986ee24ea286885d32af58175a`（本地 `/workspace/iterm2-upstream`）。
许可：iTerm2 为 GNU GPL v2；派生文件保留来源声明并按 GPL v2 发布（`SPDX-License-Identifier: GPL-2.0-only`）。

## T04：CSI / Parser

| 本仓库 | 上游文件 | 上游函数/结构 | 修改与原因 |
|---|---|---|---|
| `src/vt_parser.cpp` `Parser::step` 中 `State::Csi` 的 C0 分支 | `sources/VT100/VT100CSIParser.m` | `AdvanceAndEatControlChars` | 行为保留：CAN/SUB/ESC 取消序列；ENQ BEL BS HT LF VT FF CR SO SI DC1 DC3 DEL 作为 incidental 输出为 `C0` Token（先于 CSI Token）；其余 C0 吞掉。改为逐字节状态，去掉 `CVector`/`VT100Token`（Foundation 对象）。8 位 C1 控制（上游 `support8BitControlCharacters`）固定为关闭，0x80+ 按 UTF-8 处理。 |
| `csi_begin` | 同上 | `CSIParamInitialize` | 参数改为 `std::vector`，用于跨分块保存状态。 |
| `State::Csi` 首字节 | 同上 | `ParseCSIPrefix` | 保留：首个参数字节为 `< = > ?` 时作为私有前缀。 |
| `State::Csi`/`CsiPhase::Params`、`csi_end_part`、`csi_end_param` | 同上；`sources/VT100/iTermParser.h` | `ParseCSIParameters`、`iTermParserAddCSIParameter`、`iTermParserAddCSISubparameter`、`VT100CSIPARAM_MAX`/`VT100CSISUBPARAM_MAX`(16) | 参数上限 16 保留，超出丢弃并置 `trunc`（上游静默丢弃）。参数中间出现 `< = > ?` → 无法识别（保留）。**差异**：数值超过 INT32_MAX 时饱和并置 `trunc`（上游置 unrecognized → `VT100_UNKNOWNCHAR`；设计文档 15.2 要求显式数值上限/饱和）。**差异**：空子参数（如 `38:2::255`）保留为 `null`（上游丢弃空子参数，导致位置错位）；以冒号开头的参数输出为 `[null, ...]`（上游忽略）。子参数上限改为每参数 16（上游为整个序列共 16）。最后一段完全为空的参数不计数，与上游一致（`1;m`→`[[1]]`，`1;;m`→`[[1],[null]]`）。 |
| `CsiPhase::Inter` | 同上 | `ParseCSIIntermediate` | **差异**：保留全部中间字节（上限 4，超出置 `trunc`）；上游打包进 `cmd` 时只保留最后一个。 |
| `CsiPhase::Garbage`、`csi_finish` | 同上 | `ParseCSIGarbage`、`ParseCSIFinal` | 保留：最终字节前的非控制“垃圾字节”标记为无法识别，最终输出 `INVALID`。 |
| `Parser::finish` | 同上 | `+decodeFromContext:...`（`VT100_WAIT` 时回退上下文） | 上游在数据不足时回退并等待重新解析整个缓冲；改为有状态状态机，中间状态保存在 `Parser`，不需回退、不重复消费，切分方式无关。EOF 时丢弃未完成序列并输出 `INCOMPLETE`（设计 15.2）。 |
| `Token` / `to_json` | `sources/VT100/VT100Token.h`、`iTermParser.h` `CSIParamDescription` | `VT100Token`、`CSIParam` | 改为 C++ 结构体；规范化 JSON 输出用于测试。**未移植**：`SetCSITypeAndDefaultParameters` 的命令类型映射与默认值填充（属于语义层，留到 T05 屏幕模型时移植）；规范化输出保留原始省略（`null`）。 |
| 游标 | `sources/VT100/iTermParser.h` | `iTermParserContext` 及 `Peek/Advance/Backtrack/ConsumeInteger` | 由逐字节 `feed` 循环取代，不再需要回溯；`iTermParserDebugString` 等依赖 `NSString` 的调试函数不移植。 |

## 非上游 VT100CSIParser 范围、按设计文档新增

| 本仓库 | 依据 | 说明 |
|---|---|---|
| `State::Ground` UTF-8 | 设计第 4、14.2 节 | 跨分块 UTF-8 解码，非法字节输出 U+FFFD；EOF 时残缺序列 → `INCOMPLETE`。未检查过长编码/代理区（已知差异，后续与上游 `VT100Parser`/`NSString` 解码核对）。 |
| `State::Esc`/`EscInter` | 设计 15.2 | ESC + 中间字节 + 最终字节输出 `ESC` Token；CAN/SUB/ESC/0x80+ 取消（`INVALID`）。 |
| `State::Str`/`StrEsc`、`str_append` | 设计 15.2 | OSC（BEL 或 ST 结束）、DCS/SOS/PM/APC（ST 结束）。负载上限 1 MiB，超限后丢弃并置 `trunc`，仍识别终止符；丢弃再超 1 MiB → `ERROR ParserLimitExceeded` 并停止解析（Dead）。CAN/SUB 取消；ESC 后非 `\` 取消并重新进入 ESC。 |
| `tools/itcore_test_tokens.cpp` | 测试计划 CORE-01/02 | 命令行驱动。 |

## 评审偏离（T04 追加）
- CAN/SUB 在 Esc/Csi/Str 状态：输出 INVALID 后丢弃该字节（不再作为 C0 输出）；ESC 仍重入。上游 iTerm2 VT100Parser 将其作为控制字符输出。
- UTF-8 严格解码：拒绝过长编码（E0 80..9F、F0 80..8F）、代理区（ED A0..BF）及 > U+10FFFF（F4 90+），输出 U+FFFD。
- OSC/DCS 负载中除 BEL/ESC/CAN/SUB 外的 C0 字节被忽略。
- core.cpp：按设计 §14.1 接入 vt_parser（有界事件队列 1024、NEED_DRAIN、ParserLimitExceeded→FATAL、finish_input 幂等）。

## T05：屏幕模型（Grid / Cell / Scrollback）

| 本仓库 | 上游文件 | 上游函数/结构 | 修改与原因 |
|---|---|---|---|
| `src/screen.h` `Cell` / `itcore_cell` | `sources/ScreenChar/ScreenChar.h` `screen_char_t` | 字素簇 ID、宽、fg/bg/样式、continuation | 去掉 UTF-16/`complexChar` 私有码点表，改为 grapheme 池 + `ITCORE_CELL_CONTINUATION`；颜色统一 `0xTTRRGGBB`（设计 14.2）。 |
| `src/screen.h` `Row` / `Grid` | `sources/VT100/VT100Grid.{h,m}` | 光标、滚动区、行、默认字符、scrollUp/appendChars | 去除 DVR/代理/脏行；行带 `id`/`revision`/`itcore_wrap`（hard/soft/wide）；0 基坐标在协议边界转 1 基。 |
| `src/screen.h` `Scrollback` | `sources/LineBuffer/LineBuffer.h` | 未包装逻辑行 + 按宽重排 | 双重限额（行数与字节，设计 15.4：默认 10000 / 64MiB）；重排合并连续 soft 逻辑行；无 LineBlock 分块/搜索索引（P1 子集）。 |
| `src/screen.cpp` `apply_csi_defaults` | `sources/VT100/VT100CSIParser.m` `SetCSITypeAndDefaultParameters` | CUP/HVP→1,1；ED/EL/DSR/DA/SGR→0；CU*/ICH/DCH/IL/DL/…→1 | 仅实现 P1 网格/模式/查询相关命令；返回命令标签供应用层分派；其余 CSI 仍由解析器消费、无副作用。 |
| `src/screen.cpp` `Screen::do_sgr` / `set_mode` | `sources/VT100/VT100Terminal.m` `executeSGR` / DECSET/DECRST | 粗体/斜体/下划线/反色、16/256/真彩（分号与冒号子参数）、DECAWM/DECOM/IRM/DECTCEM、47/1047/1048/1049、2004 | 无 SGR 栈、无图片/鼠标扩展；DA1 应答拟定 `?1;2c`（设计 14.3，未冻结）。 |
| `src/screen.cpp` `cell_width` | `sources/ScreenChar/ScreenChar.m` 宽字符逻辑 | 东亚宽≈2、组合/ZWJ=0 | **差异**：未移植完整 UCD/emoji ZWJ 序列宽度；默认策略固定为精简区间表（设计 14.2）。 |
| `src/core.cpp` 快照 ABI | 设计 14.1 / 14.2 `Snapshot` | `acquire_snapshot` / grapheme / cursor | 快照自持字素池拷贝；`first_row` 相对视口顶（负=历史）；销毁 core 后快照仍可读至 release。 |

### T05 已知差异（相对上游完整 VT100Screen）
- 未移植：标记/提示符状态机、DVR、完整 bidirectional、OSC 8 写入超链接池、六进制/图像、鼠标报告编码、bracketed paste 的 encode 路径（`encode_*` 仍 -100）。
- 滚动历史：按显示行驱逐并标记 hard/soft；软换行合并在重排阶段完成，不保留上游 LineBlock 元数据。
- 宽度重排在快照读取时按当前列宽计算，不维护持久 wrapped 索引缓存。
- `finish_input` 对残缺 UTF-8 显式写入 U+FFFD（解析器仅计 `Incomplete`）。

## T05 评审追加 + T06 encode / 渲染原型（同提交）

| 本仓库 | 说明 |
|---|---|
| `screen.cpp` `write_char` 组合 + `wrap_pending` | 组合字符在 wrap_pending 时回写**本行**最后一个非 CONTINUATION 格，不触发软换行。 |
| `Grid::clear_for_write` / `repair_wide_orphans` | 写格前清宽字孤儿（CONTINUATION→基字清空；窄盖宽清多余尾格）；ICH/DCH 后行内修复。 |
| `ScreenModes` + `set_mode` | DECCKM (?1)、鼠标 9/1000/1002/1003/1006、DECKPAM/DECKPNM（ESC =/>）。 |
| `Screen::dec_mode_state` + DECRQM | 上游 `VT100Terminal` DECRQM 应答（`CSI ? Ps ; Pm $ y`）；ABI `itcore_mode_query`。 |
| `Screen::sel_*` | 对应上游 `iTermSelection` 的字符粒度子集：端点用 (逻辑行 id, 字符序号) 代替绝对坐标，跟随历史重排；复制规则同上游默认（软换行拼接、去行尾空白）。 |
| `itcore_snapshot_generation` | 快照携带最近成功 `resize_gen`。 |
| `encode.cpp` | `encode_text` / `encode_paste`（≤32KiB、bracketed 事务、正文剥 NUL 与内嵌 200~/201~ 标记）/ `encode_key`（DECCKM/应用键区）/ `encode_mouse`（1000/1002/1003+SGR 1006）。 |
| `renderer/d2d/*` | ISwapChainHost + D2D/DWrite 网格原型；字形 advance 不参与列定位；`caret_rect_px` + 预编辑覆盖层。 |
| `conpty_transport.cpp` `failStart` | Wait 读线程后 `CloseHandle(reader_)`；正常 close 路径同样释放。 |

## 与上游一致的行为（评审确认，刻意保留）

- **行尾 VS16 / 肤色修饰符不加宽**：基字已落在行末列时，后续 U+FE0F 或 Emoji 修饰符只并入字素簇，不把单元格升为宽 2，也不触发换行回填。对应上游 `VT100Screen`/`ScreenChar` 在 `wrap_pending` 位置的处理。
- **无 `id=` 的 OSC 8 链接不自动分配 id**：相邻、URL 相同但未带 `id=` 的链接不会被合并为同一链接（hover 分组仅按显式 id），与上游 `iTermURLStore` 一致。

## 字素池生命周期（本端实现，非上游）

- 池满时 `intern` 返回预留的 U+FFFD id（最后一个槽位），不再退化为 0/空格。
- 压缩触发：池字节数 > 预算 75%（预算默认 `max(1 MiB, mem_limit/8)`，压缩后阈值取 `max(75% 预算, 2×存活字节)` 防抖）、RIS（`ESC c`）、ED 3。
- 压缩从主屏、备用屏、滚动历史、REP/字素簇续写状态与当前 OSC 8 链接收集仍在用的 grapheme/hyperlink id，重建新 store，重映射所有 Cell；epoch +1，所有行 revision 以全局单调计数推进；`bytes_` 按存活条目重新计算。旧快照持有旧 store，不受影响。
- 行 revision 由 Screen 级单调计数器分配（主/备用屏共享），滚动/插删行/reset/清池都会取新值；resize 保留原 revision。历史行在快照中以当前计数作 revision。快照 `itcore_snapshot_epoch()` 暴露池 epoch。

## 选区粒度 / 视口 / ANSI DECRQM（T08 R2）

- `itcore_selection_start_unit(unit)`：0 字符、1 词、2 逻辑行。词类沿用上游默认 "Characters considered part of a word"：`/-+\~_.`、字母数字、非 ASCII；空白一类，其它标点各自成词。逻辑行沿软换行段向两侧扩展。扩展时按头点与锚点单位范围的先后关系吸附（同上游 `iTermSelection` 的 word/line 模式）。
- 选区变化：`sel_touch(old)` 只给新旧范围内的网格行取新 revision，全局计数推进一次，历史行随之变脏。
- `itcore_history_rows`：当前列宽下历史显示行数（备用屏为 0）。快照的 `first_row<0` 显示历史；光标按视口相对行给出，移出视口则不可见。
- ANSI DECRQM（`CSI Ps $ p`）：对应上游 `VT100Terminal` 的 ANSI 分支，4→IRM、20→LNM，其余 Pm=0。LNM 置位时 LF/VT/FF 兼回车，Enter 编码为 CR LF（上游 newlineMode）；ABI：DEC 私有模式用 `itcore_mode_query`，ANSI 模式用 `itcore_ansi_mode_query`，两个编号空间互不影响（?4 DECSCLM 仅记录状态 vs 4 IRM）。
- T09 Hyper-V：无上游对应（Windows 专有）。`hyperv_resolve.*` 地址解析策略，`hyperv_query.*` PowerShell 查询，`hyperv_transport.*` 解析后接 SSH。
- P0 第 1 批：`screen.cpp` 字符集/制表位/SGR 扩展/DECSCNM/DECSCUSR/DECALN/DECSTR/DEC DSR ← 上游 `VT100Terminal.m`、`VT100Screen` 对应处理（setCharacterSet、tab stops、DECALN、softReset、DSR）。
- P0 第 2 批：`screen.cpp` DA2 ← `VT100Output.m`（`>64;2500;0c`）；`core.cpp` `winops`（CSI 14/16/18/22/23 t）← `VT100Terminal.m` 窗口操作；OSC 0/1/2/4/10/11/12/104/110–112 ← `VT100Terminal.m` executeXtermSetKvp / 颜色查询；默认调色板按 xterm。
- `itcore_export_text` / `Screen::export_text`：重连归档用，无上游直接对应（近似上游 `PTYSession` 重启时保留旧内容的做法）。
