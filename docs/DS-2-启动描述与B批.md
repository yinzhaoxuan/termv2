# DS-2 启动描述，以及 A5 / A1 余量 / B1–B5 的设计

日期：2026-11-04
基线：main `a06bbf6`
出处：`docs/HANDOFF.md` §4.1、§4.2、§4.3 A1 余量、§6；`REVIEW.md` §4 GAP-03..06、§6「配置加载校验」；`REVIEW-增补-2026-10-31.md` §3 DS-2 / DS-5 / DS-6。

本仓库内没有设计 v0.3 原文。下面凡是写「决定」的地方，是本文替原设计补的选择；以后找到原文有冲突，以原文为准再改。

## 0. 约束

- 能放进平台无关代码的逻辑都放进 `ui/src`、`ui/include/uicfg`，或 `app/pane_backend/pane_logic.*`，并在 Linux / macOS 的 ctest 里单测。
- `ui/shell_win32/main.cpp` 和 `win32_pane_backend.cpp` 只做胶水：调用纯函数、弹框、接线。这两处目前在开发机上编不了（没有 MinGW），改动越薄越好。
- 不改 `ITCORE_ABI_VERSION`，不动 `ports/core`。
- Direct SSH 的默认值和环境变量开关保持原样（HANDOFF §4.5）。

## 1. A5：加载时校验

### 1.1 文件级校验

新增 `uicfg::validateConnectionsFile(const ConnectionsFile&) -> FileValidation`：

```cpp
struct FileValidation {
  std::vector<ValidationError> fileErrors;                          // 组 ID 重复等
  std::map<std::string, std::vector<ValidationError>> byConnection; // 连接 ID → 该连接的错误
  bool ok() const;
};
```

检查项：

- 每个连接跑现有 `validate(c)`。
- `connectTimeoutSeconds` 必须在 1–600 之间。这一条加进 `validate(c)`，字段名 `ssh.connectTimeoutSeconds`。
- 连接 ID 非空且唯一。重复时，**每一个**同 ID 的连接都记一条 `id` 错误。
- 组 ID 唯一。`groupId` 非空时必须指向已有的组，否则记 `groupId` 错误。

### 1.2 load 不再把非法值当成合法

- `connFrom` 里 `port` 只接受 JSON 整数，且在 int 范围内。字符串、小数、超过 int 范围的数、`true` 都不抛异常（抛异常会让整个文件变成 Corrupt），而是把 `port` 记成 0，让校验报「端口必须是 1–65535 的整数」。`connectTimeoutSeconds` 同样处理（非整数记 0）。
- `loadConnections` 在 Ok / RecoveredBackup 两种状态下跑 `validateConnectionsFile`，结果放进 `LoadResult::validation`。状态不变，文件不变成只读：一条坏连接不能拖垮其它连接。
- 保存仍然先校验，有错就拒绝（现有行为）。

### 1.3 启动前再校验

DS-2 的 `describeLaunch`（§3）在生成启动描述之前，再对**这一条**连接跑一次 `validate(c)`，并检查 ID 是否重复。有错返回 `LaunchError`，文本形如 `连接配置无效（endpoint.port）：端口必须是 1–65535 的整数`。端口只在校验通过后才转成 `uint16_t`。`main.cpp` 里的 `(uint16_t)c->endpoint->port` 删除。

### 1.4 主界面

启动时 `validation` 非空：在状态栏 `statusNote` 提示「connections.json 有 N 条连接配置无效，已禁止启动」，并 `OutputDebugStringW` 每条错误。连接树里这些连接仍然显示，双击后窗格进入 Failed，错误文本来自 §1.3。

## 2. A1 余量：启动时清理过期快照

新增 `uicfg::cleanupStaleSnapshots(const fs::path& snapshotDir) -> CleanupResult { int removed; std::vector<std::string> errors; }`：

- 只删除普通文件，文件名符合 `ssh-<数字>-<数字>.conf`（旧名）或 `ssh-<数字>-<数字>-<数字>.conf`（`sshSnapshotLeafName`）。其他文件不碰，子目录不进。
- 目录不存在返回 `removed=0`，不报错。
- 调用时机：`wWinMain` 拿到配置目录锁（`app.lock.tryAcquire`）之后、创建任何窗格之前。锁保证没有其他实例在用这个目录，本进程还没启动会话，所以删掉的都是残留。
- `ssh_snapshot.h` 那句「这里不删残留；启动时清理仍待配置目录锁接入」改成指向新函数。

## 3. DS-2：统一启动描述

### 3.1 模型（`uicfg/model.h`）

```cpp
struct LocalEnvironment {
  bool inherit = true;
  std::map<std::string, std::string> set;   // 名称 → 值
  std::vector<std::string> unset;
};
struct LocalSettings {
  std::string executable;
  std::vector<std::string> arguments;
  std::optional<std::string> workingDirectory;
  LocalEnvironment environment;
  std::string closePolicy = "terminateSessionTree";
};
```

store 完整读写 `environment` 和 `closePolicy`（B3），读进来的值原样写回。字段缺失时取上面的默认值。

校验（加进 `validate(c)` 的 local 分支）：

- `executable`、参数、`workingDirectory`、环境的名和值都不能含 NUL；`executable` 和 `workingDirectory` 不能含 CR/LF。
- 环境变量名非空，不含 `=`。同一名字（不区分大小写，Windows 语义）不能同时出现在 `set` 和 `unset`；`set` 里也不能有只差大小写的两个名字。
- `closePolicy` 目前只认 `terminateSessionTree`。其他值：加载照常，校验报 `local.closePolicy` 错误（「不支持的关闭策略」），因此也启动不了。

### 3.2 启动描述（新文件 `uicfg/launch.h` / `ui/src/launch.cpp`）

```cpp
struct LocalLaunchDesc {
  std::string executable;                      // 空 = 默认 shell（%ComSpec%），只出现在空 connectionId
  std::vector<std::string> arguments;
  std::optional<std::string> workingDirectory; // 已经决定：null → ctx.userHome；userHome 也空则 nullopt（继承）
  std::optional<std::vector<std::pair<std::string, std::string>>> environment; // nullopt = 原样继承
};
struct SshLaunchDesc {
  std::string host; uint16_t port; std::string user;
  AuthMode auth; std::string identityFile; uint32_t connectTimeoutSec;
};
struct HyperVLaunchDesc {
  SshLaunchDesc ssh;                           // host 留空
  std::string vmId; std::optional<std::string> adapterId;
  std::string addressFamily; std::optional<std::string> fallbackHost;
  bool startIfOff = false;
};
struct LaunchError { std::string message; };
using LaunchDesc = std::variant<LocalLaunchDesc, SshLaunchDesc, HyperVLaunchDesc, LaunchError>;

struct LaunchContext {
  std::string userHome;                                  // UTF-8；Windows 上取 %USERPROFILE%
  std::vector<std::pair<std::string, std::string>> parentEnvironment;  // 只在需要合成环境时用
};
LaunchDesc describeLaunch(const ConnectionsFile&, const std::string& connectionId, const LaunchContext&);
```

规则：

- 空 `connectionId` → `LocalLaunchDesc{}`（默认 shell，继承环境和 cwd）。`--command` 仍然只给第一个空 ID 的本地窗格，与保存的 local 配置无关。
- 找不到连接 → `LaunchError{"连接配置不存在"}`。
- 先做 §1.3 的校验。
- `kind=local` → `LocalLaunchDesc`。`workingDirectory=null` 映射成 `ctx.userHome`（REVIEW GAP-04 的「cwd=null 按设计映射到用户主目录」）。
- `kind=ssh` 且 `mode=manual` → `SshLaunchDesc`。
- `kind=ssh` 且 `mode=hyperv` → `HyperVLaunchDesc`。不再拿 `fallbackHost` 当成手动地址直接连。

### 3.3 环境合成（纯函数）

```cpp
// nullopt：inherit=true 且 set/unset 都空，交给 CreateProcess 继承
std::optional<std::vector<std::pair<std::string, std::string>>>
composeEnvironment(const LocalEnvironment&, const std::vector<std::pair<std::string, std::string>>& parent);
```

- 名称比较不区分大小写（ASCII 折叠即可）。
- `inherit=true`：从父环境出发，删掉 `unset`，再写入 `set`（覆盖同名，保留 `set` 里的大小写）。
- `inherit=false`：从空集出发，但保留父环境里的 `SystemRoot`、`SystemDrive`、`windir`（没有这几个变量很多 Win32 程序起不来），再应用 `unset` 和 `set`。也就是说 `unset` 可以去掉这三个。
- 父环境里以 `=` 开头的项（`=C:=C:\` 这类每盘符当前目录）在 `inherit=true` 时保留，`inherit=false` 时不带。
- 结果按名称不区分大小写排序（CreateProcess 环境块的要求），并且非空。若结果真的为空，补一项 `SystemRoot`（取父环境值，没有就 `C:\Windows`），避免 `LaunchSpec::environment` 为空时被当成「继承」。

### 3.4 后端接线（`win32_pane_backend.*`）

`PaneBackendConfig` 新增：

```cpp
struct LocalLaunchOptions {
  std::wstring executable;                       // 空 = 默认 shell
  std::vector<std::wstring> arguments;
  std::optional<std::wstring> workingDirectory;
  std::vector<std::pair<std::wstring, std::wstring>> environment; // 空 = 继承
};
// connectionId 非空时先调。返回 nullopt 且 *error 空：不是本地连接，往下走 Hyper-V / SSH。
// 返回 nullopt 且 *error 非空：是本地连接但无效，窗格 Failed。
std::function<std::optional<LocalLaunchOptions>(const std::string&, std::string*)> resolveLocal;
```

本地分支条件改成「`connId_` 为空，或 `resolveLocal` 返回了值」。拿到值时：

- `executable` 不是绝对路径就用 `SearchPathW` 解析（带 `.exe` 扩展名）。找不到 → 启动前失败：`未找到程序：<原值>`。
- `workingDirectory` 有值但目录不存在 → 启动前失败：`工作目录不存在：<路径>`。
- 这两种都走 GAP-15 的同步失败路径（`settleSyncStart(SyncStart::PreStartFailed, …)`），窗格直接 Failed，可以重连。
- `--command` 只在 `connId_` 为空时生效（现有行为不变）。

主界面 `makeFactory` 注入三个解析器：`resolveLocal`、`resolveHyperV`、`resolveSsh`。三者都调用同一个 `describeLaunch`，各自只认自己那一种结果，遇到 `LaunchError` 就回错误文本。`resolveSsh` 遇到 Hyper-V 描述返回错误「内部错误：Hyper-V 连接未经 resolveHyperV」，正常情况下不会走到。

## 4. B1：主界面接入 Hyper-V

- `resolveHyperV` 把 `HyperVLaunchDesc` 映射成 `it2w::HyperVLaunch`：policy 的 vmId、adapterId、addressFamily、fallbackHost、startIfOff；ssh 部分与手动 SSH 共用一段构造代码（known_hosts、快照目录），`hostName` 留空。HostKeyAlias 由传输层按 vmId 生成（D-3 已在）。
- **普通连接不启动 VM**：`startIfOff` 默认 false。
- 恢复动作走**结构化字段**，不解析 `errorText`。`hyperv_resolve.cpp` 里 AddressAmbiguous 的说明是把「网卡名 地址」用中文逗号拼成一句，网卡名可能含空格，而且没有 adapterId，反解析不可靠。`uishell::SessionStatus`（UI 拥有的头文件）新增：

```cpp
struct AddressChoice { std::string adapterId; std::string label; };  // label：「<网卡名> <地址>」
struct SessionRecovery {
  enum Kind { None, StartVmAndConnect, ChooseAddress } kind = None;
  std::vector<AddressChoice> choices;   // ChooseAddress：按后端给的顺序
};
// SessionStatus 里加一项；只在 Failed 时有意义
SessionRecovery recovery;
```

  后端在 `HvAction::Fail` 时填：`failure == VmNotRunning` → StartVmAndConnect；`AddressAmbiguous` → ChooseAddress，choices 来自 `HvAction::candidates`。其余失败为 None。映射写成 `pane_logic` 里的纯函数（输入 failure 类别和候选列表，平台无关），在 `tests/app` 单测。`errorText` 保持原样，给人看。
- 窗格覆盖层：按钮文字由 §5 的 `uicfg::paneOverlay` 统一给出（Failed 且有恢复动作时为「启动并连接」/「选择地址…」，否则「重连」）。前者以 `startIfOff=true` 重连一次；后者弹菜单列出 `choices[i].label`，选中后以该项的 adapterId 重连一次。这些覆盖只作用于这一次重连，不写回 `connections.json`。
- 传递覆盖的方式：UI 线程上 `g->hvOverride = {connectionId, startIfOff, adapterId}`，调用 `reconnectPane`（后端在 UI 线程同步调用 `resolveHyperV`），返回后清空。`resolveHyperV` 读到匹配的覆盖就套用。
- 进度：Hyper-V 分支启动时，后端把 `status_.title` 设成「正在解析 Hyper-V 地址…」并发布；`HvAction::Connect` 时已有的标题「Hyper-V <地址>」/「手动地址 <地址>」覆盖它。手动回退因此是可见的。

## 5. B4：恢复工作区时不自动连接（DS-5）

- 决定：不新增 `SessionState`（会牵动后端和所有 switch），在 UI 侧给 `PaneHost` 加 `notStarted` 标志，与 `missingRef` 并列。
- `wWinMain` 恢复标签时，所有窗格（含空 ID 的本地窗格）都以 `notStarted=true` 创建，不调用 `startSession`。
- 首次运行（工作区里没有标签）仍然自动打开一个本地终端，这条单独保留。
- 新建标签、拆分窗格、双击连接：照常立即启动。
- 占位窗格：覆盖层文字「未连接：<连接名>。点击「连接」或按 Enter 开始」，按钮文字「连接」。点击按钮、在窗格里按 Enter、或者用鼠标单击窗格，都调用 `startSession` 并清掉 `notStarted`。
- 关闭占位窗格不需要确认（`infoFor` 把它当成 Exited）。
- 窗格覆盖层的全部决定（引用缺失、占位、Failed/Exited、按钮文字）集中在一个纯函数 `uicfg::paneOverlay(missingRef, notStarted, connName, status) -> PaneOverlay`，放进 `shell_logic` 并单测。`main.cpp` 只负责显示。
- `createPane` 用枚举 `PaneStart { Now, Placeholder }` 表示是否立即启动；恢复工作区用 Placeholder，新标签、拆分、双击连接用 Now。

## 6. B5：多行粘贴确认（DS-6）

- 触发：`normalizePasteText` 之后的文本里有 `\r`（任何换行，包括只有末尾一个换行。末尾换行在不支持 bracketed paste 的远端会直接执行）。
- 远端开着 bracketed paste 时**仍然确认**。UI 拿不到对端是否真的按 bracketed 处理，宁可多问。
- 「不再询问」：v0.1 只做全局开关，`settings.json` 隐藏字段 `confirmMultilinePaste`（布尔，缺省 true；类型不对按 true 处理并原样写回，做法与 osc52 相同）。没有界面。
- 预览纯函数：

```cpp
struct PastePreview { bool needsConfirm; std::string message; };
PastePreview pastePreview(const std::string& normalizedText, bool confirmEnabled);
```

  文案：「将粘贴 N 行、M 字节：」，接着最多 10 行，每行最多 120 个字符（按 UTF-8 码点截断，不切断多字节字符），超出部分写「…（其余 K 行未显示）」。控制字符（除换行外的 C0、DEL）显示成 `^X` 形式，让用户看得见。
- 确认：MessageBox（确定 / 取消，默认按钮是取消）。确定 → 用**同一段**文本调用一次 `paste`，不追加回车；取消 → 什么都不发。
- 32 KiB 上限、NUL 与内嵌标记清洗仍由后端负责，不在 UI 重复。

## 7. 测试

新增或扩展的 ui 单测（`ui/CMakeLists.txt` 的 foreach 里登记）：

| 用例 | 内容 |
|---|---|
| validate / store | 端口 0、65536、65558、"22"、22.5、true、1e10 都在加载后报 `endpoint.port`，文件状态仍是 Ok，其它连接不受影响；超时 0 / 601 报错；重复连接 ID 两条都报；未知 groupId 报错。 |
| store 往返 | local 的 environment（inherit=false、set 含中文值、unset）与 closePolicy 保存后重新加载逐字段相等；未知 closePolicy 原样保留。 |
| launch | 空 ID → 默认本地；local cwd=null → userHome；manual SSH 端口转换；hyperv → HyperVLaunchDesc 且不把 fallbackHost 当 host；坏端口 → LaunchError 文案；不存在 → LaunchError。 |
| composeEnvironment | inherit=true 无覆盖 → nullopt；unset 不区分大小写；set 覆盖；inherit=false 保留 SystemRoot/SystemDrive/windir；排序；结果非空。 |
| snapshots | 只删匹配的文件；`ssh-1-1.conf` 和 `ssh-12-3-4.conf` 删，`ssh-a-1.conf`、`known_hosts`、子目录不删；目录不存在。 |
| pane_logic（tests/app） | HvAction 失败类别 + 候选 → `SessionRecovery`：VmNotRunning、AddressAmbiguous（保持顺序、label 格式）、其它类别为 None。 |
| shell_logic | `paneOverlay`（引用缺失、占位、恢复按钮文字的优先级）、`pastePreview`（行数、字节、截断、控制字符、末尾单换行也要确认、关闭开关不确认）。 |
| settings | `confirmMultilinePaste` 缺省、true/false、类型错误原样写回。 |

Windows 上要手测的部分写进 STATUS「未实机项」：从连接树启动 PowerShell（带空格和中文参数、显式 cwd、null cwd、环境 set/unset）、程序或目录不存在的错误、Hyper-V 各分支（运行 / 关机 → 启动并连接 / 多候选 → 选择地址 / 回退地址标题）、恢复四个 SSH 窗格后 ssh.exe 进程数为 0、多行粘贴确认与取消。
