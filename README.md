# termv2

termv2 是从 iTerm2 派生的 C++20 Windows 终端，使用 CMake 构建。产品壳为 `it2w_shell.exe`，本地会话使用 ConPTY，SSH 默认使用系统 `ssh.exe` + ConPTY，也可选编进程内 libssh2 直连。Linux/macOS 原生构建用于可移植逻辑与单元测试，不构建 Windows 产品壳。

## 快速开始

需要 CMake ≥ 3.24、支持 C++20 的编译器和 make。nlohmann/json 默认使用仓库内头文件，无需安装系统包；可用 `-DUICFG_USE_SYSTEM_NLOHMANN=ON` 显式选择系统 nlohmann_json ≥ 3.11。

```sh
cmake --preset dev
cmake --build --preset dev -j8
ctest --preset dev -j8
```

`dev` 使用 `RelWithDebInfo`，输出到 `build/`。`asan`（ASan/UBSan）和 `tsan`（TSan）使用各自构建目录，命令中替换 preset 名即可，需 Clang 或 GCC 支持。测试 preset 均排除需要私有 SSH 实验环境的 `T07.ssh_live`，`tsan` 另排除 `PANE-11.flood_close_destroys_continuation_target`。

CORE-01/02 输入向量不在仓库中，`ITCORE_VECTORS` 默认指向 `/workspace/qa/vectors/core01_02.json`；缺失时告警并跳过。配置时加 `-DIT2W_REQUIRE_VECTORS=ON` 可将缺失变成错误；用 `-DITCORE_VECTORS=/path/to/core01_02.json` 指定已有向量。

在 Ubuntu 24.04 安装交叉编译器后构建 Windows 产品（默认目标含 `it2w_shell.exe`）：

```sh
sudo apt-get update
sudo apt-get install -y mingw-w64 g++-mingw-w64-x86-64-posix
cmake --preset mingw
cmake --build --preset mingw -j8
```

产品壳位于 `build-mingw/ui/it2w_shell.exe`。交叉构建不运行原生测试。

`mingw-direct` 在 `build-mingw-direct/` 启用 `IT2W_SSH_DIRECT=ON`，首次构建会下载并编译固定版本、校验 SHA-256 的 OpenSSL 与 libssh2：

```sh
cmake --preset mingw-direct
cmake --build --preset mingw-direct -j8
```

运行时环境变量见 [HANDOFF §2](docs/HANDOFF.md#2-如何构建与测试)。

## 接下来读

- [交接与构建测试说明](docs/HANDOFF.md)（构建依赖与 preset 以本 README 和 CMake 为准）
- [当前状态与待验证项](docs/STATUS.md)
- [剩余工作与推荐顺序](docs/TODO.md)
- [Windows 实机手测清单](docs/WINDOWS-手测清单.md)
- [实现审查清单 2026-11-05](docs/审查清单-2026-11-05.md)（ConPTY 关闭路径、Ctrl 符号键）
- ADR：[窗格 pump](docs/ADR-001-pump状态机.md)、[OSC 52 读能力](docs/ADR-002-CAP01-P06-osc52-read.md)、[Direct SSH](docs/ADR-003-ssh-direct.md)、[Ctrl+C 默认](docs/ADR-004-Ctrl+C默认行为.md)、[itcore ABI 规则](docs/ADR-005-itcore-ABI版本规则.md)
- 设计：[DS-2 启动描述与 B 批](docs/DS-2-启动描述与B批.md)、[DS-9 外观 Profile](docs/DS-9-外观Profile.md)、[DS-10 OSC 52 读确认](docs/DS-10-OSC52读确认.md)、[DS-12 Direct 首次信任](docs/DS-12-Direct首次信任.md)、[DS-13 按连接选择传输](docs/DS-13-按连接选择传输.md)、[DS-14 连接编辑器](docs/DS-14-连接编辑器.md)、[DS-15 诊断包导出](docs/DS-15-诊断包导出.md)、[DS-16 快捷键展示](docs/DS-16-快捷键展示.md)、[DS-17 内存账本](docs/DS-17-内存账本.md)
- [与 iTerm2 的功能差距路线](docs/ROADMAP-iTerm2-parity.md)
- [上游来源](UPSTREAM.md)、[上游文件清单](docs/UPSTREAM-FILES.md)
- [第三方声明](THIRD-PARTY-NOTICES.md)、[许可与依赖审计](docs/LICENSE-AUDIT.md)

## 许可

GPL-3.0-or-later，见 [LICENSE](LICENSE)。`ports/core` 按 iTerm2（GPL v2 或更新版本）改写，来源见 [UPSTREAM.md](UPSTREAM.md)。
