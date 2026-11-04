# 第三方声明

termv2 采用 GPL-3.0-or-later（见 [LICENSE](LICENSE)）。完整的依赖与许可清单见 [docs/LICENSE-AUDIT.md](docs/LICENSE-AUDIT.md)。

## 所有 Windows 构建

| 组件 | 许可 | `licenses/` 中的文件 |
|---|---|---|
| nlohmann/json 3.11.3（`third_party/nlohmann_json`） | MIT | `nlohmann_json-LICENSE.txt` |
| Unicode 字符数据库派生表（宽度、大小写折叠） | Unicode License v3 | `Unicode-LICENSE.txt` |
| MinGW-w64 运行时（libstdc++ / libgcc，静态链接） | GPL-3.0 with GCC Runtime Library Exception | — |
| MinGW-w64 运行时（winpthreads / CRT，静态链接） | MIT 风格 / ZPL 2.1 | 待补（见 LICENSE-AUDIT §5） |

`it2w_license_notices` 目标随 `it2w_shell` 构建，把 `termv2-LICENSE.txt` 与上表文件放进 `it2w_shell.exe` 旁的 `licenses/`。

## Direct SSH

`IT2W_SSH_DIRECT=ON` 的 MinGW 产品静态链接以下固定依赖；源码 URL 和强制 hash 校验见
[`cmake/SshDirectDeps.cmake`](cmake/SshDirectDeps.cmake)。

| 依赖 / 源码包 | 许可 | SHA-256 |
|---|---|---|
| [OpenSSL 3.5.9 — openssl-3.5.9.tar.gz](https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz) | Apache-2.0 | `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a` |
| [libssh2 1.11.1 — libssh2-1.11.1.tar.gz](https://libssh2.org/download/libssh2-1.11.1.tar.gz) | BSD-3-Clause；`agent_win.c` 另保留 BSD-2-Clause 声明 | `d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7` |

libssh2 本地补丁 [`cmake/ssh_a_openssh_only.cmake`](cmake/ssh_a_openssh_only.cmake)：
**移除 Pageant 后端条目**，只删除 agent 后端表中的该条目；OpenSSH 命名管道路径及其 I/O、认证实现未改。

构建 Direct 产品时，`it2w_third_party_notices` 默认随全量构建运行，将以下文件放在
`it2w_shell.exe` 旁的 `licenses/`，分发时须一并携带：

- `libssh2-COPYING.txt`：libssh2 源码的 `COPYING` 原文。
- `libssh2-agent_win.c.txt`：libssh2 的完整 `src/agent_win.c`，保留其中的许可声明。
- `OpenSSL-LICENSE.txt`：OpenSSL 源码的 `LICENSE.txt` 原文。
- `THIRD-PARTY-NOTICES.md`：本声明。

独立 `ssh_a_probe` 使用同一套复制命令，目录仍为 `ssh-a-licenses/`。
上述许可均与本项目采用的 GPL-3.0-or-later 兼容；对应源码的随包或书面获取途径见
[`UPSTREAM.md`「第三方静态依赖」](UPSTREAM.md#第三方静态依赖)。源码链接以仓库根目录为基准。
