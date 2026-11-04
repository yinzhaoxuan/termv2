# 上游来源
- 仓库: https://github.com/gnachman/iTerm2
- 固定提交: f29420ae4d3e0f986ee24ea286885d32af58175a
- 上游许可：GPL v2 或更新版本（上游 `COPYING` 说明因 Apache-2.0 依赖实际按 GPL v3）
- 本项目许可：**GPL-3.0-or-later**（根目录 [LICENSE](LICENSE)）。派生文件保留来源声明，并带 `SPDX-License-Identifier: GPL-3.0-or-later`
- 参照过的上游文件与 SHA-256：[docs/UPSTREAM-FILES.md](docs/UPSTREAM-FILES.md)；依赖与许可审计：[docs/LICENSE-AUDIT.md](docs/LICENSE-AUDIT.md)

## 第三方静态依赖

`IT2W_SSH_DIRECT=ON` 时，静态链接的 libssh2 1.11.1 和 OpenSSL 3.5.9 属于本项目
（GPL-3.0-or-later）的对应源码。发布时须附上固定的 `libssh2-1.11.1.tar.gz`、`openssl-3.5.9.tar.gz`
源码包及 `cmake/ssh_a_openssh_only.cmake`，或提供指向这些确切文件及其 hash 的书面获取途径。
版本、上游 URL、完整 SHA-256、许可和补丁说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)；
随可执行文件附带 `licenses/` 不能代替对应源码的交付。
