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
