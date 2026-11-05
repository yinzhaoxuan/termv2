# ADR-006：SSH 默认传输——按认证方式自动选择，分两阶段切换

- **状态**：Accepted（2026-11-05，用户批准）。阶段 1 已实现，阶段 2 等门槛。
- **出处**：`docs/TODO.md` §2.1；ADR-003 §决策 1；HANDOFF §4.5；DS-12、DS-13。
- **范围**：没写 `ssh.transport` 的 SSH / Hyper-V 连接走哪种传输；全局默认放在哪里；发布包是否含 Direct；改默认前要满足的条件。本地连接不受影响（始终 ConPTY）。

## 背景

现在的规则（DS-13 §3）：连接写明 `compatible` / `direct` 时以连接为准；没写时看环境变量 `ITERM2WIN_SSH_DIRECT`，值为 `1` 走直连，否则走兼容。构建默认 `IT2W_SSH_DIRECT=OFF`，CI 的 `mingw-direct` 只能手动触发。

把 Direct 改成默认的两个前置条件已经在 main 上：首次信任界面（DS-12）和按连接选择传输（DS-13）。缺的是 Windows 实机记录，以及 `mingw-direct` 至今没有在 DS-10、DS-12、DS-13 之后编译过。

### 两种传输对 agent 连接是等价的

托管配置（`configSource=managed`，首版唯一支持的取值）下，两种传输能做的事基本一样：

| | 兼容（ConPTY + `ssh.exe`） | 直连（进程内 libssh2） |
|---|---|---|
| 认证 | agent、密钥文件、交互（密码） | **仅 agent** |
| agent 模式的认证 | 快照写 `PreferredAuthentications publickey`、`PasswordAuthentication no`，不会退回密码 | 只用 agent |
| 主机信任 | `configDir/ssh/known_hosts`，`StrictHostKeyChecking ask`（`ssh.exe` 在终端里询问） | 同一个文件，未知主机走 DS-12「核对指纹…」 |
| `~/.ssh/config`、ProxyJump、转发、keepalive | 都不用（快照显式关掉） | 都不支持 |
| 远端尺寸 | ConPTY 转发 | SSH window-change |
| OSC 52 读、DCS / Sixel / tmux -CC 等序列 | ConPTY 会吞掉（ADR-002） | 字节透明；OSC 52 读逐次确认（DS-10） |
| 本机进程 | 每个窗格一个 `ssh.exe` + conhost | 没有子进程 |

所以，对 agent 认证的连接，直连没有功能损失，还多了序列透明和更少的进程。对密钥文件和密码认证的连接，直连 v0 根本不能用。agent 没启动、agent 里没有被接受的密钥这类问题，两种传输失败得一样。

## 决策

### 1. 新增「自动」取值，按认证方式静态选择

没写 `ssh.transport` 的连接，默认取值为 **自动（`auto`）**：

- 构建含 Direct **且** 认证方式为 agent → 直连；
- 否则 → 兼容。

这是启动前根据配置算出的静态选择，不是失败后回落。ADR-003「不自动回落」的原则不变：选中直连后连接、主机密钥或认证失败，窗格照常 Failed，不再用 `ssh.exe` 重试。

`auto` 不会选中构建里没有的传输，所以不含 Direct 的构建里自动结果总是兼容。将来直连支持密钥文件时，只需扩大第一条的条件。

`connections.json` 的 `ssh.transport` 仍只接受 `"compatible"` / `"direct"` 或省略。`"auto"` 不写进连接；连接里写 `"auto"` 仍按 DS-13 报非法值。省略就表示跟随全局默认，再写一个 `"auto"` 只会多一种等价写法。

### 2. 全局默认放进 settings.json（隐藏字段），环境变量保留为覆盖

优先级从高到低：

| 来源 | 取值 | 说明 |
|---|---|---|
| 连接 `ssh.transport` | `compatible` / `direct` | DS-13，不变 |
| 环境变量 `ITERM2WIN_SSH_DIRECT` | `1` → 直连；**`0` → 兼容**；其他值或未设 → 不参与 | 新增 `0`，用于排查时整体退回 `ssh.exe`。只在进程启动时读一次 |
| settings.json `sshDefaultTransport` | `"auto"` / `"compatible"` / `"direct"` | 新增隐藏字段，不进设置界面 |
| 内置默认 | 阶段 1：`compatible`；阶段 2：`auto` | 见决策 4 |

`sshDefaultTransport` 照 `osc52` 的办法处理：文件里有这个字段才写回；值非法时保留原文，按内置默认处理，并在状态栏提示一次「settings.json 的 sshDefaultTransport 无效，已按默认处理」。和 `osc52` 一样经 `PaneBackendConfig` 传给窗格，只影响此后新建的窗格；手改文件要重启产品才生效。

为什么不只用环境变量：Windows 用户要在快捷方式或系统设置里改环境变量，改默认之后如果直连出问题，退回的路径太绕。为什么还保留环境变量：手测和排查时不用改文件，也和现有文档、手测清单一致。

显式选中直连（来自连接、环境变量或 settings 的 `"direct"`）而构建不含 Direct 时，行为不变：窗格 Failed，不回落。

### 3. 选择结果带上来源，报错和编辑器都看得见

纯函数（`app/pane_backend/pane_logic.*`）取代现在的 `wantDirectSsh`：

```cpp
enum class SshDefaultTransport { Auto, Compatible, Direct };
enum class TransportSource { Connection, Environment, Settings, Builtin };
struct TransportChoice { bool direct; TransportSource source; };

// perConnection：连接写明的传输；env：ITERM2WIN_SSH_DIRECT 解析结果（1/0/不参与）；
// settings：settings.json 的值（非法或缺省为 nullopt）；builtin：内置默认。
TransportChoice resolveSshTransport(std::optional<it2w::SshTransportMode> perConnection,
                                    std::optional<bool> env,
                                    std::optional<SshDefaultTransport> settings,
                                    SshDefaultTransport builtin,
                                    bool buildHasDirect, bool authIsAgent);
```

- 直连失败、且来源不是 `Connection` 时，`errorText` 末尾追加一句：「此连接按默认使用直连；如需改用 ssh.exe，在连接编辑器里把传输改为兼容。」用户看得出直连是默认选的，也知道怎么换。
- 连接编辑器（DS-14）的传输下拉框第一项从「跟随全局默认」改成「跟随全局默认（当前：直连）」或「（当前：兼容）」，括号里的结果由同一个函数对编辑中的认证方式实时算出。
- 每次启动 SSH 时写一条诊断 `ssh.transport`，内容为 `direct=0/1 source=<来源>`，诊断包（DS-15）原样带上。

### 4. 分两阶段

**阶段 1（现在可以做）**：实现决策 1–3，内置默认仍是 `compatible`。行为对现有用户完全不变。同时：

- 发布包使用 `mingw-direct` 构建（`IT2W_SSH_DIRECT=ON`）。开发预设的默认值不变。
- CI 的 `mingw-direct` 改为每次推送都跑，用 `actions/cache` 缓存 `build-mingw-direct/ssh-a-deps`（OpenSSL / libssh2 的 ExternalProject 目录）。缓存键是 `cmake/SshDirectDeps.cmake`（版本与 SHA-256）、补丁脚本和两个工具链文件的哈希。没有改成「预编译前缀」：许可文本要从 ExternalProject 的源码目录复制（`SshDirectLicenses.cmake`）。缓存没起作用时只会退回完整构建。只能手动触发的任务一直没人跑（DS-10、DS-12、DS-13 的 Direct 代码至今没编译过），这本身就是风险。

**阶段 2（满足门槛后）**：把内置默认从 `compatible` 改成 `auto`，代码只改一处，并在 STATUS 和发布说明里写明。

阶段 2 的门槛，全部满足才切换：

1. **编译**：`mingw-direct` 在要发布的提交上通过。
2. **实机手测**：`WINDOWS-手测清单.md` 的 P1-3、P1-4、P1-5（Direct 部分）、P2-4（Direct 读）全部通过，P3-2（洪峰与关闭）在 Direct 连接上再做一遍。至少在一台 Windows 10 22H2 或 Windows 11 上，记录提交号和系统版本（REVIEW C6）。
3. **日常使用**：至少有一个工作日把常用连接设成 `sshDefaultTransport: "auto"`，没有出现兼容模式下不会出现的断线或异常。记下空闲断线的情况：两种传输都没有 keepalive，空闲断线不算直连的问题，但要确认表现一致。
4. **退回路径已验证**：`sshDefaultTransport: "compatible"` 和 `ITERM2WIN_SSH_DIRECT=0` 都能让同一连接退回 `ssh.exe`。

任一门槛不满足，就停在阶段 1，并把原因写进 STATUS。

## 不做

- 失败后自动换另一种传输重试（ADR-003）。
- 直连支持密钥文件、密码、ProxyJump、keepalive、`~/.ssh/config`。这些都是另外的设计，做完后只需扩大决策 1 的条件。
- 在设置界面里放全局传输开关。等真的有设置页再说；在那之前，连接编辑器的按连接选择已经够用。
- 改变本地连接。

## 后果

- ADR-003 §决策 1「运行时仍由 `ITERM2WIN_SSH_DIRECT=1` 控制」由本 ADR 取代；ADR-003 加一行指向这里。
- DS-13 §3 的真值表扩展为决策 2 的四级优先级；DS-13 中「不加 settings.json 里的全局默认」一条由本 ADR 取代。
- 阶段 2 之后，含 Direct 的发布包里，agent 连接默认走直连。新建连接默认就是 agent 认证，所以大多数新连接会走直连；用户第一次连接一台新主机时看到的是 DS-12 的「核对指纹…」，不再是 `ssh.exe` 在终端里的 yes/no 提问。
- 两种传输共用 `configDir/ssh/known_hosts`，阶段 2 切换时已信任的主机不用重新确认。

## 实现清单（阶段 1）

| 文件 | 改动 |
|---|---|
| `app/pane_backend/pane_logic.*` | `sshDirectEnv`（`1`/`0`/不参与）、`resolveSshTransport`、`kBuiltinSshDefault`、`directSourceHint` / `withDirectSourceHint`、`transportDiagText`；取代 `wantDirectSsh`。`selectSshTransport` 的文字只说原因（「此构建未包含直连 SSH。」），改法由调用方按来源追加 |
| `platform/windows/common/ssh_direct_logic.cpp` | `validateDirectAuthMode` 文字改为「直连 v0 仅支持 agent 认证；请 ssh-add 后使用 agent」，改法同样按来源追加 |
| `app/pane_backend/win32_pane_backend.cpp` | 环境变量解析为三态；每次 `startSession` 调 `resolveSshTransport`；diag 写来源 |
| `it2app::PaneBackendConfig`、`ui/shell_win32/main.cpp` 的窗格工厂 | 带上 settings 的 `sshDefaultTransport`（与 `osc52` 同路） |
| `ui/include/uicfg/settings.h`、`ui/src/settings.cpp` | `sshDefaultTransport` 读写、非法原文保留、只在存在时写回 |
| `ui/src/connection_form.cpp`、`ui/shell_win32/connection_dialog.*`、`ui/shell_win32/main.cpp` | `transportFollowLabel`；下拉框第一项显示当前结果，切换认证方式时刷新；settings 非法时的状态栏提示 |
| `ui/src/diag_bundle.cpp` | 配置摘要加 `sshDefaultTransport = 默认 / auto / compatible / direct / 无效（默认）` |
| `.github/workflows/ci.yml` | `mingw-direct` 每次推送都跑，加依赖缓存 |
| 测试 | `resolveSshTransport` 全表（四个来源 × 两种构建 × 两类认证）；settings 往返与非法值；编辑器标签；报错追加文字 |
| 文档 | ADR-003 指向本文；DS-13 §3、`ssh-direct.md`、HANDOFF 环境变量表、手测清单 P1-3 增加 `0` 值、settings 字段和来源提示的用例 |
