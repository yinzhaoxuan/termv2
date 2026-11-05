# DS-12 Direct SSH 首次信任界面

日期：2026-11-04
出处：HANDOFF §4.5、ADR-003「Hyper-V 主机密钥身份」末段、`docs/ssh-direct.md`「认证与主机信任」、REVIEW 增补二 D-3。

## 0. 目标与边界

现在 Direct 只读取 `configDir/ssh/known_hosts`。新主机（包括新的 Hyper-V 虚拟机）只能先切到兼容模式（`ssh.exe`）确认一次指纹。本设计在 Direct 里补上「展示指纹 → 用户确认 → 写入 known_hosts → 重连」。这是把 Direct 改成默认传输的前置条件之一，但**本设计不改默认值**。改默认还需要另一项：按连接选择传输（D-5 余项，DS-2 的延伸），另行决定。

不做的事：

- 不做 accept-new，不做「以后总是信任」。每个新主机都要用户确认一次。
- **密钥变化（HostKeyChanged）不提供信任按钮。** 变化可能是中间人攻击，界面只展示新旧情况和处理办法，用户自己去编辑 known_hosts。
- 不信任 `ssh-dss`（已废弃），这种情况按「不支持的主机密钥类型」报错。
- 不写哈希条目。Windows OpenSSH 默认 `HashKnownHosts no`，兼容模式写的也是明文，两种模式保持一致。
- 兼容模式不变：`ssh.exe` 在 ConPTY 里自己询问。

## 1. 数据从哪里来

`direct_ssh_hostkey.cpp` 在 `libssh2_knownhost_check` 之前已经拿到三样东西：查找名 `hostLower`（普通 SSH 是 `host` 或 `[host]:port`；Hyper-V 是 `hv-<guid>`）、原始公钥 `key/length`、类型名 `name`，指纹也已经算好。失败时现在只把这些拼进 `detail` 字符串。

改动：`HostKeyUnknown` 失败时，额外带上结构化的 `PresentedHostKey`：

```cpp
struct PresentedHostKey {
  std::string knownHostsName;  // 写入 known_hosts 的第一列，与本次查找串完全相同
  std::string keyType;         // ssh-ed25519 / ecdsa-sha2-nistp256 / ssh-rsa …
  std::string keyBase64;       // 原始公钥 base64（known_hosts 第三列）
  std::string fingerprint;     // "SHA256:…"，不带尾部 '='
};
```

它放在平台无关的 `platform/windows/common/ssh_direct_logic.h`，并沿着已有的失败通道（`DirectFailure` → `SshExitInfo` → 窗格）一路带到 `Win32PaneBackend`。Hyper-V 内层走同一条通道。`HostKeyChanged` 不带这个结构。

## 2. 窗格状态

沿用 DS-2 §4 的结构化恢复字段：

```cpp
struct HostKeyOffer { std::string knownHostsName, keyType, keyBase64, fingerprint; };
struct SessionRecovery {
  enum Kind { None, StartVmAndConnect, ChooseAddress, TrustHostKey } kind = None;
  std::vector<AddressChoice> choices;
  std::optional<HostKeyOffer> hostKey;   // TrustHostKey 时有值
};
```

后端在 Direct 失败且带 `PresentedHostKey` 时设 `TrustHostKey`。映射写成 `pane_logic` 的纯函数，并单测。`errorText` 保持原样。

## 3. 界面

- `uicfg::paneOverlay` 遇到 `TrustHostKey`，按钮文字为「核对指纹…」。
- 点击后弹出确认框（`MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2`，默认按钮是取消）。文案由纯函数 `uicfg::hostKeyTrustPrompt(const HostKeyOffer&, const std::string& knownHostsPath)` 生成：

```
无法验证主机「<knownHostsName>」的身份。
密钥类型：<keyType>
指纹：<fingerprint>

请通过可信渠道核对指纹，例如在服务器控制台运行：
  ssh-keygen -lf /etc/ssh/ssh_host_<短类型>_key.pub
一致后再信任。信任后会写入：
<knownHostsPath>
并重新连接。
```

  `<短类型>` 取 `ed25519` / `ecdsa` / `rsa`。
- 确定：写入 known_hosts（§4），成功后重连一次；失败就在状态栏显示原因，不重连。取消：什么都不做。
- `HostKeyChanged`：按钮仍是「重连」，`errorText` 照原样显示（里面已经有类型和指纹）。另外在覆盖层文字后追加一句：「主机密钥与已信任的不同。确认服务器确实更换过密钥后，手动删除 known_hosts 中的旧条目再连接。」

## 4. 写入 known_hosts（纯函数，平台无关）

放在 `uicfg`（configDir 归 UI 管）：

```cpp
// 校验后生成一行；任一字段非法返回 nullopt
std::optional<std::string> knownHostsLine(const HostKeyOffer&);
// 追加一行；文件不存在就创建。用 writeDurable + replaceFile 整体重写（与 connections.json 相同的原子方式），
// 不做就地追加，避免半行。已存在完全相同的行时不重复写，返回 true。
bool appendKnownHost(const std::filesystem::path& file, const std::string& line, std::string* error);
```

校验：

- `knownHostsName` 非空，不含空白、`,`、控制字符；不以 `@` 或 `|` 开头（那是标记和哈希条目）。
- `keyType` 只接受 `ssh-ed25519`、`ecdsa-sha2-nistp256/384/521`、`ssh-rsa`。
- `keyBase64` 只含 base64 字符，长度为 4 的倍数。
- 行格式：`<knownHostsName> <keyType> <keyBase64>\n`。原文件末尾没有换行时，先补一个换行。

已有同名但类型不同的条目时照样追加。OpenSSH 允许一个主机有多种类型的密钥，Direct 的「已知类型在前」逻辑本来就按类型查找。

## 5. 安全说明

- 写入的是**本次失败时服务器出示的那把钥匙**。重连时服务器若换了别的钥匙，会得到 `HostKeyChanged`，不会被静默接受。
- 确认框默认按钮是取消，回车不会误信任。
- 诊断日志里本来就有指纹（D-2），不新增敏感字段。

## 6. 测试

| 位置 | 用例 |
|---|---|
| ssh_direct_logic（Linux） | `PresentedHostKey` 的类型名映射；DSS 被拒。 |
| pane_logic | 带 `PresentedHostKey` 的失败 → `TrustHostKey`；HostKeyChanged → None。 |
| uicfg | `knownHostsLine` 各种非法输入；`appendKnownHost` 新建文件、末尾无换行时补换行、重复行不重复写、写失败时原文件不变；`hostKeyTrustPrompt` 文案；`paneOverlay` 的按钮文字与 HostKeyChanged 的追加文字。 |
| Windows 手测 | 未信任主机 → 核对指纹 → 写入后连上；取消不写；再连直接成功；改服务器密钥后 → HostKeyChanged、无信任按钮；Hyper-V 新虚拟机按 `hv-<guid>` 写入，换 DHCP 地址后仍能连上。 |

Direct 代码只在 `IT2W_SSH_DIRECT=ON` 时编译。CI 的 `mingw-direct` 任务当时只能手动触发；ADR-006 之后每次推送都跑。
