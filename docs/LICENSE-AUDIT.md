# 许可与依赖审计（REVIEW GAP-12 / §8 E6）

日期：2026-11-04
范围：main 上的源码，以及 CI 的 `mingw`（`it2w_shell.exe`）与手动 `mingw-direct`（含 Direct SSH）两种 Windows 产物。
性质：材料整理与一致性核对，**不是法律意见**。分发前如有疑问请咨询专业人士。

## 1. 结论

- **本项目整体采用 GPL-3.0-or-later**（2026-11-04 由项目所有者决定）。根目录 [LICENSE](../LICENSE) 为 GPLv3 全文（SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`，与 gnu.org 原文一致）。所有项目源文件带 `SPDX-License-Identifier: GPL-3.0-or-later`。
- 选择理由：
  1. 上游 iTerm2（固定提交 `f29420ae`）的 `COPYING` 写明「GPL v2 or later」，并说明因 Apache-2.0 依赖「effectively GPL v3」。派生作品可以按 v3 发布。
  2. Direct 构建静态链接 OpenSSL 3.5.9（Apache-2.0）。Apache-2.0 与 GPLv3 兼容，与 GPLv2-only 不兼容。此前 12 个核心文件标的 `GPL-2.0-only` 会让带 Direct 的二进制无法合规分发，已全部改为 GPL-3.0-or-later。
  3. 与 `UPSTREAM.md` 早先「整体按 GPL v3」的表述一致。
- 下表所有依赖都与 GPLv3 兼容。分发二进制时须随附 `licenses/` 目录与对应源码的获取方式（§4）。

## 2. 依赖闭包

### 2.1 所有 Windows 构建都包含

| 组件 | 进入产物的方式 | 许可 | 随包文件（`licenses/`） | 备注 |
|---|---|---|---|---|
| termv2 本身（含按 iTerm2 改写的 `ports/core`） | 源码编译 | GPL-3.0-or-later | `termv2-LICENSE.txt` | 上游文件与哈希见 [UPSTREAM-FILES.md](UPSTREAM-FILES.md) |
| nlohmann/json 3.11.3（`third_party/nlohmann_json`，仅头文件） | 编译进 `uicfg` | MIT（Expat） | `nlohmann_json-LICENSE.txt` | |
| Unicode 字符数据库派生表：`ucd_width_table.inc`（UCD 16.0.0，`tools/gen_width_table.py` 生成）、`case_fold_table.inc`（Python `unicodedata` 15.1.0 生成，仓库内无生成脚本） | 编译进 `itcore` | Unicode License v3（数据），表本身随项目 GPL-3.0-or-later | `Unicode-LICENSE.txt`（原文在 `third_party/unicode/LICENSE.txt`） | |
| MinGW-w64 运行时：libstdc++、libgcc | `-static -static-libgcc -static-libstdc++` 静态链接 | GPL-3.0 with GCC Runtime Library Exception 3.1 | 无须（运行库例外允许以任意许可分发结果） | |
| MinGW-w64 运行时：winpthreads、mingw-w64 CRT 启动代码 | 同上 | winpthreads：MIT 风格；CRT：ZPL 2.1 / 公有领域为主 | **待补**（§5） | 版本取决于 CI 的 Ubuntu `mingw-w64` 包 |
| Windows 系统库（user32、gdi32、comctl32、comdlg32、dwrite、d2d1、d3d11、dxgi、shell32、ole32、imm32、ntdll；Direct 另加 ws2_32、crypt32、bcrypt、advapi32） | 动态链接 | 系统组件 | 无须 | GPLv3 §1「System Libraries」 |

### 2.2 仅 `IT2W_SSH_DIRECT=ON`（`mingw-direct`）

| 组件 | 方式 | 许可 | 随包文件 | 备注 |
|---|---|---|---|---|
| OpenSSL 3.5.9 | 静态链接 | Apache-2.0 | `OpenSSL-LICENSE.txt` | 源码包 SHA-256 见 [THIRD-PARTY-NOTICES](../THIRD-PARTY-NOTICES.md) |
| libssh2 1.11.1（去掉 Pageant 后端的补丁） | 静态链接 | BSD-3-Clause；`agent_win.c` 另有 BSD-2-Clause 声明 | `libssh2-COPYING.txt`、`libssh2-agent_win.c.txt` | 补丁 `cmake/ssh_a_openssh_only.cmake` |

### 2.3 不进入产物

- 测试代码与测试数据（`tests/`、`ui/tests/`，含手写的 `.itermcolors` 样例）。
- 构建工具：CMake、MinGW-w64 编译器、Python（生成脚本）、Perl（OpenSSL 构建）。
- 运行时调用而不链接的外部程序：`ssh.exe`（Windows OpenSSH）、`powershell.exe`（Hyper-V 查询）、`conhost`（ConPTY）。

## 3. SPDX 与源码头

- 所有项目源文件（`ports/`、`renderer/`、`app/`、`platform/`、`ui/`、`tests/`、`cmake/` 与根 `CMakeLists.txt`）首行（或 shebang / `#pragma code_page` 之后）为 `SPDX-License-Identifier: GPL-3.0-or-later`。
- 生成的 Unicode 表另有一行说明数据来源与 Unicode License v3。
- `third_party/` 保持各自原样。
- 派生自上游的文件，在注释里写明上游路径；对应关系见 SOURCE_MAP 与 UPSTREAM-FILES。

## 4. 分发清单

1. `it2w_shell.exe` 与同目录的 `licenses/`：所有构建由 `it2w_license_notices` 目标放入 `termv2-LICENSE.txt`、`nlohmann_json-LICENSE.txt`、`Unicode-LICENSE.txt`、`THIRD-PARTY-NOTICES.md`；Direct 构建再由 `it2w_third_party_notices` 放入 OpenSSL / libssh2 的三个文件。
2. 对应源码（GPLv3 §6）：本仓库对应提交的源码，以及 Direct 构建时的 `openssl-3.5.9.tar.gz`、`libssh2-1.11.1.tar.gz` 与补丁（见 [UPSTREAM.md](../UPSTREAM.md#第三方静态依赖)）。随包附带，或提供书面获取途径。
3. 诊断包里的「提交（配置时）」可用来确认二进制对应的源码提交。

## 5. 待办

- **MinGW-w64 运行时声明**：winpthreads 与 CRT 的许可文本尚未自动放进 `licenses/`。它们来自构建机的 `mingw-w64` 安装（CI 为 Ubuntu 包，通常在 `/usr/share/doc/mingw-w64-common/` 下）。发布流程建立时，从构建机拷贝对应 `COPYING` 并核对版本；在那之前手动附带。
- `case_fold_table.inc` 没有生成脚本，来源只能看文件头。以后改动它时应补一个生成脚本。
- 本审计只覆盖 main 上实际编译进产物的组件。新增依赖时更新本文与 THIRD-PARTY-NOTICES。
