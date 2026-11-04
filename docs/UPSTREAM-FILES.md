# 上游文件清单（REVIEW GAP-12 / §8 E6）

上游：https://github.com/gnachman/iTerm2 ，固定提交 `f29420ae4d3e0f986ee24ea286885d32af58175a`。
生成：2026-11-04，对该提交做浅层、无 blob 的 `git fetch` 后逐个读取下列文件（`git rev-parse <提交>:<路径>` 得 blob，内容做 SHA-256）。

本项目**没有逐字拷贝**上游文件：`ports/core` 是按这些文件的结构与行为用 C++ 重写的派生作品，逐函数的对应与差异见 [ports/core/SOURCE_MAP.md](../ports/core/SOURCE_MAP.md)。下表「关系」一列：

- **移植来源**：termv2 的源码注释直接写明对应这个上游文件，按派生作品对待。
- **结构参照 / 行为参照**：只在 SOURCE_MAP 或设计文档里用来对照结构或行为，没有沿用其代码。

核对方法：`git -C <iTerm2 克隆> rev-parse f29420ae4d3e0f986ee24ea286885d32af58175a:<路径>` 应等于下表 blob；或取该提交的原始文件求 SHA-256。

| 上游路径 | 关系 | termv2 中的位置 | git blob | SHA-256 | 字节 |
|---|---|---|---|---|---|
| `COPYING` | 许可声明 | — | `220aac3aff3450cef5105d609c6e5982d8e015eb` | `e8623589e5af3639c9725d7b3307c726c0e4428684d3c5a3c0f5fbdd98be8efd` | 222 |
| `LICENSE` | GPLv2 全文（上游） | — | `a8613c7ec4d6bc4bbde06b8178aada340803abd3` | `990f1da86e9f58355fc82b5cd486b70aaeb53ae0ba9722424a0270ff0d458de9` | 14818 |
| `sources/VT100/VT100CSIParser.m` | 移植来源 | `ports/core/src/vt_parser.{h,cpp}`、`screen.h` | `0b24e03dcc4980a7da8d77c6da99812c77698737` | `535a2f9c4cbbba79d279521b40f749353975615e903ff56c2aab8f82830595b8` | 32909 |
| `sources/VT100/iTermParser.h` | 移植来源 | `ports/core/src/vt_parser.{h,cpp}` | `4631618d512d7498e2766be4e58fa322746ca055` | `5b28f355471a195046e9917bc18ddf83280673a2e15579828675789a5d4c0903` | 9798 |
| `sources/VT100/VT100Token.h` | 结构参照 | SOURCE_MAP（`Token`） | `2697610114cbb311b52758ede775348db4830dfd` | `d81f7f158bc29bb2677b4db0b406cbf0533dcca4f3b65dd3b64eec966d9cd532` | 20856 |
| `sources/VT100/VT100Parser.m` | 行为参照 | SOURCE_MAP「评审偏离」 | `47cac920669d972ed38ab45b359bafc7f6812696` | `c60a8eebf7b456d2b6cd757da7a28b7c89e5cb99975b750f1b28fdccd8310483` | 29603 |
| `sources/VT100/VT100Grid.h` | 移植来源 | `ports/core/src/screen.h` | `e7bde082ce87665adfc75abf7e1e3bba7b10c61c` | `cba492b6ef12f4be9660d6ba3bc7376d47ea5eecebe915dd30b291a5d4a66d69` | 23027 |
| `sources/VT100/VT100Grid.m` | 移植来源 | `ports/core/src/screen.h` | `513481cd5ac2c0f2f44cc231a44e891af495403d` | `2947994af78caba65bdd2edd940b811afbb61ecf187220463a6bafdc44525a8a` | 149278 |
| `sources/VT100/VT100Terminal.m` | 移植来源 | `ports/core/src/screen.{h,cpp}`、`core.cpp` | `20530316dd4ffeffb44aee1e9155b36d99f6b52f` | `f7b900b94aacd60bb0f157d3c2021e7088b2fc67479ce902a689f0debe6ffeee` | 267686 |
| `sources/VT100/VT100GraphicRendition.m` | 行为参照 | SGR 子参数（GAP-14） | `29188458d2b64cc0bb4eddf12d15f945c227f240` | `9a3da8783f77b7e2cffa8e3361fde87b667c770834180961539e68510e6eb4db` | 24511 |
| `sources/VT100/VT100Output.m` | 行为参照 | DA 身份（`docs/identity-14.3.md`） | `66a5230acf2c84424d9956e881a74097a62dfe8b` | `0cdb20e69996ef61ba0928039db86e5131ff679ea94f76cd037ea6750fbde703` | 61706 |
| `sources/VT100Screen/VT100Screen.m` | 行为参照 | SOURCE_MAP（VS16、DSR 等） | `202fd6626be7324b4099e340ceebeb0b63fcbc7c` | `9557d23350aa4e83144edb5abc2cf040155e00099aa6bf954609eb5b11755aab` | 98121 |
| `sources/ScreenChar/ScreenChar.h` | 移植来源 | `ports/core/src/screen.h` | `7e3f92a730fb8a2903f0f6c0d30c535fdf058e0b` | `2aa94719914b74dbc1e7b9936f9aaadc76d8ffffefdf0cc118a80250ec20374e` | 32353 |
| `sources/ScreenChar/ScreenChar.m` | 移植来源 | `ports/core/src/screen.{h,cpp}`、`ucd_width.h`、`tools/gen_width_table.py`（`StringToScreenChars`） | `14aaa38a620dddd49ce0085eb6b2c6d0c0703b99` | `c52de5b93b149b010c6cefb89ff6000d42598bc6cf2e0c8443f19530b382722f` | 39647 |
| `sources/CharacterSets/iTermCharacterWidth.c` | 移植来源 | `ports/core/src/ucd_width.h`、`tools/gen_width_table.py` | `21f003ccf2c9130d5dccf4ca8f4d59a2bb37323d` | `a7d79380b554b3e251be0a36a6b8af328af6bdc9fec99badf2871c07609b4e84` | 28908 |
| `sources/LineBuffer/LineBuffer.h` | 移植来源 | `ports/core/src/screen.h`、`search.h` | `b6429e787c701f109bc9fa4f02b268530dbfc6b6` | `b455b97d70b4c08c0b4bff9725987175c74437949de992049b0996853f32890b` | 15689 |
| `sources/Selection/iTermSelection.m` | 行为参照 | 选词规则（SOURCE_MAP「选区粒度」） | `1ddd1bb41f55852d3b0b88c01f62866893c5c51c` | `408bf92e0b2d3eb869d7b29772a43010b0094bae008750a09d8af00e727998fd` | 68006 |
| `sources/ExternalSessionState/iTermURLStore.m` | 行为参照 | OSC 8 id 规则（SOURCE_MAP） | `002b97c4016aff6fe96697c8d6fe44f87ef2d1f0` | `a98281e1d6bc08779f55d9e77cc11b57d0bb2c6609c1b6abe8310a88447d0178` | 10675 |
| `sources/PTYSession/PTYSession.m` | 行为参照 | 重连归档（SOURCE_MAP `itcore_export_text`） | `d86df41f20eaad85c2b42a166427dba95175c606` | `f308d219880c1ba7d2a1e3fdab78033ae9699b8722d7d3fdda708ee2a4ad35f2` | 1203963 |

上游 `COPYING` 原文：「iTerm2 is licensed under GPL v2 or later. See the LICENSE file in this directory for the full license. Because of Apache 2.0-licensed dependencies, which are only compatible with GPL v3, the license is effectively GPL v3.」本项目据此整体采用 GPL-3.0-or-later（见 [LICENSE-AUDIT.md](LICENSE-AUDIT.md)）。

新增或改动移植来源时：在源码注释写明上游路径，更新 SOURCE_MAP，并把该文件补进本表（含 blob 与 SHA-256）。
