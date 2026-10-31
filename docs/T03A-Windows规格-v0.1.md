# T03A Windows 侧规格说明 v0.1

- 负责人：Windows 系统；实现：架构师
- 依据：《iTerm2-Windows移植设计-v0.3》第 6、13.4、13.5、15、16、19 节；`ports/core/include/itcore.h`（C ABI 草案 v0）；上游固定 SHA `f29420ae4d3e0f986ee24ea286885d32af58175a`
- 外部参考：微软 [Pseudoconsoles](https://learn.microsoft.com/en-us/windows/console/pseudoconsoles)、[Creating a Pseudoconsole session](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)、[CreatePseudoConsole](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)、[ResizePseudoConsole](https://learn.microsoft.com/en-us/windows/console/resizepseudoconsole)、[ClosePseudoConsole](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole)、[Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)、[Get-VMNetworkAdapter](https://learn.microsoft.com/en-us/powershell/module/hyper-v/get-vmnetworkadapter)
- 状态：设计规格，**全部条目均未实机验证**。标注“待实测”的内容在 T03A 实验中填写结果后才能冻结。
- 范围：只写规格，不含实现代码。文中的 API 名称用来说明调用次序，不是可以直接编译的代码。

---

## 1. ConPTY 会话生命周期与句柄归属

### 1.1 句柄清单与所有者

| 句柄 | 创建者 | 所有者（负责关闭） | 继承 | 说明 |
|---|---|---|---|---|
| `inRead`（ConPTY 读端） | Transport.start | ConPTY 创建成功后**立即**由 start 关闭 | 否 | 交给 `CreatePseudoConsole`，伪控制台内部复制一份；我方保留会导致 EOF 永远不到 |
| `inWrite`（我方写端） | Transport.start | 写线程 | 否 | 输入调度器的唯一出口 |
| `outRead`（我方读端） | Transport.start | 读线程 | 否 | 关闭全过程必须持续读取，见 1.5 |
| `outWrite`（ConPTY 写端） | Transport.start | ConPTY 创建成功后**立即**关闭 | 否 | 同 `inRead` |
| `HPCON` | `CreatePseudoConsole` | 生命周期协调者（Closer） | — | 只允许 `ClosePseudoConsole` 一次，靠原子标志保证幂等 |
| 属性列表 `LPPROC_THREAD_ATTRIBUTE_LIST` | start | start | — | 子进程创建后立刻 `DeleteProcThreadAttributeList` 并释放内存 |
| `hJob` | start | Closer | **否** | `SECURITY_ATTRIBUTES.bInheritHandle=FALSE`；子进程不能拿到 |
| `hProcess` / `hThread` | `CreateProcessW` | Closer / start | 否 | `hThread` 恢复后立刻关闭；`hProcess` 用于等待退出和取退出码 |
| 快照文件 `-F` | SnapshotWriter | attempt 清理器 | — | 见 3.1；attempt 结束后再删除 |

所有管道用 `CreatePipe` 创建，并传入 `bInheritHandle=FALSE`。ConPTY 端的句柄由伪控制台自己转交给客户端进程，不靠句柄继承。

### 1.2 创建顺序（Starting）

1. 按第 13.5 节计算初始网格：可见区域、字体、DPI，最小 20×5。隐藏窗格推迟到能算出尺寸时再启动。
2. 两次 `CreatePipe`，得到 in 和 out 两组管道，均不可继承。
3. `CreatePseudoConsole(size, inRead, outWrite, flags=0, &hpc)`。`flags` 首版取 0；`PSEUDOCONSOLE_INHERIT_CURSOR` 需要我方回应光标位置查询，先不用，CAP-01 再评估。
4. **成功后立即关闭 `inRead`、`outWrite`。** 失败则关闭全部四个管道句柄，转 Failed。
5. **先启动读线程**开始读取 `outRead`，然后才创建进程。ConPTY 可能在客户端连接时就开始输出，比如初始化序列。
6. `CreateJobObjectW(sa{bInheritHandle=FALSE}, NULL)`；`SetInformationJobObject(JobObjectExtendedLimitInformation)` 设置 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，**不**设置 `BREAKAWAY_OK` 和 `SILENT_BREAKAWAY_OK`（第 13.5 节默认禁止 breakaway）。
7. `InitializeProcThreadAttributeList` 写入一个属性 `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = hpc`。
   - 可选，待实测：加 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 并给空的显式列表，或者直接 `bInheritHandles=FALSE`。首版采用 `bInheritHandles=FALSE`。实验要确认在 Win11 上这样做不影响 ConPTY 附着。
   - 可选，待实测：`PROC_THREAD_ATTRIBUTE_JOB_LIST` 可以在创建时直接入 Job，没有窗口期。如果验证可用，可以替代第 8 步的关联操作，但仍保留挂起创建。
8. `CreateProcessW(applicationName=检测到的绝对路径, commandLine=按 CommandLineToArgvW 规则编码的参数, bInheritHandles=FALSE, flags=EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env=第 13.5 节定义的环境块, cwd=确定的目录, &siEx)`。
9. `AssignProcessToJobObject(hJob, hProcess)`。**失败**时 `TerminateProcess(hProcess)`（进程此时还没执行任何用户代码），走第 1.4 节的失败清理，报告 StartFailed(JobAssign)。
10. `ResumeThread(hThread)`，然后 `CloseHandle(hThread)`。
11. 删除属性列表；注册进程退出等待（线程池 `RegisterWaitForSingleObject` 或专门线程）；状态切到 Running。

不变量：进入 Running 时，我方进程里只剩 `inWrite`、`outRead`、`hpc`、`hJob`、`hProcess` 五个对象，每个只有一个所有者。

### 1.3 尺寸调整（ResizePseudoConsole）

- UI 线程按像素、字体和 DPI 计算行列，钳到至少 20×5，然后投递 `Resize{cols, rows, generation}` 给 Session 执行器。
- 合并规则（来自渲染的补充约束）：只有**还没执行**的 resize 可以互相合并，**不能跨过**已经排队的鼠标或输入事件。
- 执行器顺序：`itcore_resize(core, cols, rows, generation)` → `ResizePseudoConsole(hpc, {cols, rows})`。HRESULT 失败要记入诊断但不终止会话；连续失败 3 次转 Failed（阈值待定）。
- 只能在 Running 状态调用。Closing 及之后状态忽略，并且 **不得** 与 `ClosePseudoConsole` 并发：两者都经过 Closer 的状态锁串行。这把锁只保护 hpc 的有效性，**不能**在持锁期间等待 I/O。
- ConPTY resize 后可能重绘并输出整屏序列，这部分要在 CAP-01 中记录。

### 1.4 关闭顺序（Closing，对应第 15.3 节）

**死锁约束（微软文档）**：`ClosePseudoConsole` 在旧版本 Windows 上会一直阻塞，直到伪控制台把剩余输出写进 `outWrite` 所在的管道。如果此时没有人读 `outRead`，管道写满，Close 就永远不返回。微软文档写明 Windows 11 24H2 起行为有变化（Close 不再等待），但我们必须两种行为都兼容，具体以实测为准。因此：

1. 标记 Closing，增加 attempt 失效计数：新输入、计时器和 Hyper-V 查询都作废，输入调度器拒绝新单元。
2. **读线程继续读取。** 读到的数据进入关闭排空路径（1.5），不受 UI 背压影响。
3. 写线程：停止发起新的 WriteFile。正在阻塞的 WriteFile 用 `CancelSynchronousIo(写线程)` 取消，或者首版直接改用 overlapped 写加 `CancelIoEx`（待实测选一种，建议 overlapped）。随后写线程关闭 `inWrite` 并退出。
4. 在**工作线程**（不能是 UI 线程，不持有核心锁）上执行 `TerminateJobObject(hJob, 退出码)`，确保 ssh.exe 和本地 shell 子树全部终止。
5. 同一工作线程执行 `ClosePseudoConsole(hpc)`，hpc 置为无效（原子交换，保证幂等）。
6. 等待读线程读到 EOF（`ReadFile` 返回 `ERROR_BROKEN_PIPE`），或者达到排空上限，然后关闭 `outRead`。
7. 等 `hProcess` 进入 signaled，取 `GetExitCodeProcess`，关闭 `hProcess`，最后 `CloseHandle(hJob)`。由于有 KILL_ON_JOB_CLOSE，即使上面任何一步异常，最后一个 Job 句柄关闭时也会兜底清理。
8. 等核心执行器把剩余任务做完，发布最终快照，转 Closed，或者在故障路径转 Failed。
9. 删除本 attempt 的 SSH 配置快照文件。

**初始进程自行退出**（Running → 进程退出事件）：立即从第 1 步开始执行同一套流程，**不等**输出 EOF。同一个 ConPTY 下可能还有其他客户端，没有 Close 就不会出现 EOF。

每个阶段都记录开始时间。总体目标 ≤3 秒（第 18.2 节）。超时只记录卡住的阶段并让 UI 保持可用，**不提前释放**其他线程仍在使用的句柄。

### 1.5 输出排空

- 正常 Running：输出队列上限 4 MiB，达到 3 MiB 暂停读取，回落到 1 MiB 恢复。
- Closing：走独立的排空路径，不看水位，读线程持续读。核心正常时把尾部数据解析到 EOF；核心已故障，或者排空累计超过**额外 4 MiB**，就改为有界流式读取并丢弃，打上“数据不完整”标记并记录丢弃字节数，然后继续推进关闭。
- 读线程只做 `ReadFile` 和入队，不调用核心，不调用 UI 回调。

---

## 2. SessionTransport 接口建议（C++20）

以下是接口形状建议，不是实现。

### 2.1 类型

```text
enum class TransportState { Idle, Starting, Running, Closing, Closed, Failed };
// Resolving 属于 Session 层（HyperVResolver），不在 Transport 内部

struct AttemptId { uint64_t value; };          // 每次 start 单调递增，重连一定换新值
struct SessionId { uint64_t value; };

struct LaunchSpec {                             // local 和 ssh 统一为可执行文件加参数数组
  std::filesystem::path executable;            // 绝对路径
  std::vector<std::wstring> arguments;         // 不做 shell 拼接
  std::optional<std::filesystem::path> workingDirectory;
  EnvironmentBlock environment;                // 已执行 inherit、unset、set 并完成校验
  GridSize initialSize;                        // ≥ 20×5
  ClosePolicy closePolicy;                     // 首版只有 terminateSessionTree
  std::optional<std::filesystem::path> sshSnapshotFile; // ssh 时由 attempt 持有
};

enum class InputKind { UserText, PasteTransaction, Reply };
struct InputUnit { InputKind kind; std::vector<std::byte> bytes; uint64_t seq; };

enum class TransportFault {
  StartFailed, JobAssignFailed, PipeBroken, InputStalled /*5 s*/,
  ReplyBackpressure, OutputLimit, ResizeFailed, CloseTimeout };
```

### 2.2 接口

```text
class SessionTransport {
public:
  // 只能从 Idle 调用一次；同步返回只表示请求已受理，结果通过 onStateChanged 通知
  virtual Expected<void> start(SessionId, AttemptId, const LaunchSpec&) = 0;

  // 都是非阻塞调用，满了立即返回 QueueFull，调用方不能等待
  virtual EnqueueResult enqueueUserInput(InputUnit) = 0;   // 4 MiB 队列
  virtual EnqueueResult enqueueReply(InputUnit) = 0;       // 64 KiB 队列
  virtual EnqueueResult beginPaste(size_t totalBytes) = 0; // ≤ 32 KiB，否则拒绝
  virtual void          cancelPasteBody() = 0;             // 尽力补发结束标记

  virtual void resize(GridSize, uint64_t generation) = 0;  // 由执行器在 itcore_resize 之后调用
  virtual void close(CloseReason) = 0;                     // 幂等；多次调用等于一次

  // 消费端：核心执行器按 attempt 拉取输出
  virtual std::optional<OutputChunk> tryPopOutput() = 0;   // 出队后释放水位
};

struct TransportCallbacks {   // 每个回调都带 SessionId 和 AttemptId，接收方先比对 attempt，不一致直接丢弃
  void onStateChanged(SessionId, AttemptId, TransportState, std::optional<TransportFault>);
  void onOutputAvailable(SessionId, AttemptId);            // 只用来唤醒执行器
  void onExited(SessionId, AttemptId, uint32_t exitCode);
  void onDiagnostic(SessionId, AttemptId, DiagRecord);     // 记录阶段、耗时、丢弃字节数
};
```

### 2.3 状态机

| 当前 | 事件 | 下一状态 | 动作 |
|---|---|---|---|
| Idle | start | Starting | 按 1.2 执行 |
| Starting | 第 1.2 步任一失败 | Failed | 逆序释放已经创建的资源 |
| Starting | 第 1.2 步完成 | Running | — |
| Starting | close | Closing | 等创建步骤到达安全点后进入 1.4 |
| Running | close / 进程退出 / 故障 | Closing | 1.4；故障记入 fault |
| Closing | 清理完成 | Closed（用户关闭或正常退出）/ Failed（故障） | 发布最终状态 |
| Closed/Failed | 任何调用 | 不变 | 返回 InvalidState；重连由上层新建 Transport 和新 AttemptId |

### 2.4 输入调度（第 15.2 节）

- 两个队列：**用户输入 4 MiB**、**协议应答 64 KiB**，容量分开计入内存账本，入队都不阻塞。
- 只有一个写线程，一次处理一个完整单元。普通文本单元最多 4 KiB，按 UTF-8 边界切分。
- 调度优先级：应答 > 尚未开始的普通输入。已经开始写的单元必须写完，否则进入故障关闭；**禁止重发已写入的前缀**。
- bracketed paste 是一个不可交错的事务：开始标记、正文、结束标记之间不插入应答和按键。正文可以分块物理写入；单次最多 32 KiB。
- 应答队列持续满载时报告 `ReplyBackpressure`，不能阻塞核心。
- **停滞超时 5 秒**：有待写数据，且写入连续 5 秒没有进展，就报告 `InputStalled` 并进入故障清理。计时器由独立的生命周期任务管理。诊断文案写“输入传输停滞”，不写成认证失败。

### 2.5 输出水位

- 队列上限 4 MiB：到 3 MiB 暂停 `ReadFile`，降到 1 MiB 恢复。暂停靠读线程等待事件实现，不丢弃数据。
- 水位只由核心消费推动，不由 UI 渲染推动。
- 与 `itcore.h` 的配合：`itcore_feed` 返回 `ITCORE_NEED_DRAIN` 时，执行器先处理 `ITCORE_EV_REPLY_BYTES`，通过 `enqueueReply` 发出，然后再继续喂数据；`ITCORE_FATAL` 时调用 `close(Fault)`。`itcore_resize` 的 generation 原样传给 `resize`。

### 2.6 线程

| 线程 | 职责 | 禁止 |
|---|---|---|
| UI | 窗口消息、计算网格 | 调用核心、等待 Transport |
| Session 执行器 | 修改核心、调用 resize、入队输入 | 等待 WriteFile 或队列空位 |
| 读线程 | ReadFile 和入队 | 调用核心或 UI |
| 写线程 | 出队和 WriteFile | 持有核心锁 |
| Closer（线程池） | Terminate、Close、等待、释放 | 运行在 UI 线程、持有核心锁 |

---

## 3. SSH-CFG-01：第 13.4 节快照选项逐项验证清单

### 3.1 前置条件

- 记录 `(Get-Command ssh.exe).Source` 和 `ssh -V` 的输出。首版目标是 `C:\Windows\System32\OpenSSH\ssh.exe`。
- 测试快照放在 `%TEMP%\it2w-cfg\snap-<attempt>.conf`，UTF-8 无 BOM。另外准备一个应用拥有的空文件 `empty_known_hosts` 和一个应用 known_hosts `app_known_hosts`。
- **污染配置**（用来证明隔离有效）：在测试用户的 `%USERPROFILE%\.ssh\config` 写入
  `Host *` / `HostName evil.invalid` / `StrictHostKeyChecking no` / `ProxyCommand cmd /c echo pwned` / `IdentityFile ~/.ssh/id_polluted` / `ForwardAgent yes`。
  有条件的话（需要管理员权限，只能在测试机上做）在 `%ProgramData%\ssh\ssh_config` 写入类似内容。
- 基础快照 `snap.conf`：
  ```text
  Host it2w-target
    HostName 192.0.2.10
    Port 22
    User tester
    StrictHostKeyChecking ask
    UserKnownHostsFile "C:/…/app_known_hosts"
    GlobalKnownHostsFile "C:/…/empty_known_hosts"
    VerifyHostKeyDNS no
    UpdateHostKeys no
    RequestTTY force
    ConnectTimeout 10
    BatchMode no
    ForwardAgent no
    ForwardX11 no
    PermitLocalCommand no
    ClearAllForwardings yes
    # 认证集合见 3.3
  ```
- 统一命令：`ssh -F <snap> -G it2w-target`，下文写作 **G**。断言时在 PowerShell 中用 `Select-String -Pattern '^<key> '` 取值。`ssh -G` 只说明解析出的配置，不能证明认证成功。

### 3.2 选项逐项

| # | 项目 | 命令 | 预期结果 |
|---|---|---|---|
| C01 | 用户配置隔离 | G（污染配置存在） | `hostname 192.0.2.10`，**不是** evil.invalid；不出现 `proxycommand` 行，或其值为 none；`identityfile` 中没有 id_polluted |
| C02 | 系统配置隔离 | 同上（ProgramData 被污染时） | 同 C01。`-F` 会替代用户配置，系统配置是否也被忽略要实测并记录 |
| C03 | HostName/Port/User | G | `hostname 192.0.2.10`、`port 22`、`user tester` |
| C04 | IPv6 地址 | 快照写 `HostName 2001:db8::10` 后执行 G | 原样输出；不能被当成 host:port 解析 |
| C05 | StrictHostKeyChecking | G | `stricthostkeychecking ask` |
| C06 | UserKnownHostsFile | G | 只有一个路径，就是 app_known_hosts（含空格、中文的路径见 C20） |
| C07 | GlobalKnownHostsFile | G | 只有 empty_known_hosts，不出现 `__PROGRAMDATA__\ssh\ssh_known_hosts` |
| C08 | VerifyHostKeyDNS | G | `verifyhostkeydns false`（或 no） |
| C09 | UpdateHostKeys | G | `updatehostkeys false`（或 no） |
| C10 | RequestTTY | G；实连时执行 `ssh -F snap it2w-target tty` | `requesttty force`；实连输出 `/dev/pts/N` |
| C11 | ConnectTimeout | G；对不可达地址 `192.0.2.1` 实连并计时 | `connecttimeout 10`；约 10 秒退出，退出码 255 |
| C12 | BatchMode | G | `batchmode no` |
| C13 | ForwardAgent | G | `forwardagent no` |
| C14 | ForwardX11 | G | `forwardx11 no` |
| C15 | PermitLocalCommand | G | `permitlocalcommand no`；没有 localcommand |
| C16 | ClearAllForwardings | G；再加一条命令行 `-L 1:x:1` 测试 | `clearallforwardings yes`；实连时本地端口不监听 |
| C17 | 禁止项不出现 | G | 不出现 `proxyjump`、`remotecommand`、`controlmaster auto`；`controlpath` 为 none 或不出现 |
| C18 | 指纹冲突 | 在 app_known_hosts 中写一个错误的 key 后实连 | 出现 “REMOTE HOST IDENTIFICATION HAS CHANGED”，拒绝连接，退出码 255，原文件没有被改写 |
| C19 | 首次指纹 | 用空的 app_known_hosts 实连 | 出现 ask 提示；输入 yes 后**只**写入 app_known_hosts，用户的 `~/.ssh/known_hosts` 没有变化 |
| C20 | 路径编码 | `IdentityFile`、`UserKnownHostsFile` 用 `C:/测试 目录/a"b/key` 等路径 | 记录 Windows OpenSSH 对空格（加引号）、反斜线、`%`、`~`、`%d` 等 token 展开的实际行为，形成编码规则；无法表示的名称由生成器拒绝 |
| C21 | 客户端路径 | 应用启动参数用绝对路径；把 PATH 里放一个假的 ssh.exe | 实际执行的是检测到的绝对路径（用进程审计或 `Get-Process ssh | select Path` 确认） |
| C22 | 环境隔离 | 设置 `SSH_ASKPASS=calc.exe`、`SSH_ASKPASS_REQUIRE=force`，然后由应用启动 | 子进程环境中没有这两个变量；仍然在终端里提示密码 |
| C23 | 快照文件生命周期 | 启动后立刻删除快照模拟失败场景 | 正常流程在 attempt 结束后才删除；提前删除时 ssh 报告配置读取失败（用于确认读取时机） |

### 3.3 认证集合

| # | 模式 | 快照增量 | G 预期 | 实连预期 |
|---|---|---|---|---|
| A1 | agent | `PreferredAuthentications publickey`、`PubkeyAuthentication yes`、`PasswordAuthentication no`、`KbdInteractiveAuthentication no`、`IdentityFile none`、`IdentitiesOnly no` | `identityfile none` 或为空，**没有**默认 `id_rsa`、`id_ed25519` 等 | 启动 `ssh-agent` 服务并加载密钥后可以登录；agent 为空时报 Permission denied (publickey)，不弹密码提示 |
| A2 | keyFile | 同上，但 `IdentityFile "<选定>"`、`IdentitiesOnly yes`、`IdentityAgent none` | identityfile 只有选定文件；`identityagent none` | 有口令的 key 在终端提示 passphrase；agent 中的其他 key 不会被尝试（用 `-v` 日志确认） |
| A3 | interactive | `PubkeyAuthentication no`、`PreferredAuthentications keyboard-interactive,password` | `pubkeyauthentication no` | 只出现密码提示；`-v` 日志中没有 publickey 尝试 |
| A4 | 默认身份和证书自动加载 | A1、A3 下在 `~/.ssh` 放 `id_ed25519` 和 `id_ed25519-cert.pub` | 不出现在 identityfile 和 certificatefile 中 | `-v` 日志中不会 Offering 这些文件 |
| A5 | 必要选项不支持 | 对任意一条，在旧版客户端上执行 G | 如果出现 `Bad configuration option`，应用应报告“客户端不兼容”，**不能悄悄省略这一项** |

记录字段按第 18.1 节：Windows build、`ssh -V`、命令、预期、实际、结论。

---

## 4. 关闭压力测试、IO-01、CAP-01 场景清单

### 4.1 关闭压力（WIN-03、第 15.3 节）

| # | 场景 | 通过标准 |
|---|---|---|
| X01 | 本地 `cmd /c "for /l %i in () do @echo xxxxx"` 持续输出时关闭，循环 100 次 | 每次 ≤3 秒；UI 没有无响应；没有挂死的 Close |
| X02 | 同 X01，故意**暂停读线程 5 秒**后再关闭（模拟读端没被服务） | 证明设计中“关闭期间持续读取”是必要的：旧系统上未读取时 Close 会阻塞；按规格实现后不会阻塞。记录 OS build 和行为差异 |
| X03 | 输出排空超过 4 MiB（关闭时有 `type 大文件`） | 有界丢弃，有“数据不完整”标记和丢弃字节数 |
| X04 | 写线程阻塞在 WriteFile（子进程不读 stdin）时关闭 | 写线程被取消并退出，没有泄漏 |
| X05 | 子进程派生孙进程（`cmd /c start /b ping -t`）后关闭 | Job 终止后没有残留进程（`Get-Process` 对比） |
| X06 | 初始 shell 先 `exit`，孙进程还在运行 | 立即进入清理，不卡在等待 EOF；孙进程被 Job 终止 |
| X07 | 应用进程被 `taskkill /f` 强杀 | KILL_ON_JOB_CLOSE 兜底，子进程树全部消失 |
| X08 | close 并发调用 10 次，同时 resize | 只执行一次 ClosePseudoConsole，没有重复 CloseHandle（App Verifier 句柄检查） |
| X09 | Starting 期间关闭（在第 1.2 步的 6、8、9 之间注入延迟） | 资源全部释放；不会出现运行起来但没进 Job 的进程 |
| X10 | AssignProcessToJobObject 注入失败 | 挂起的进程被终止，报告 JobAssignFailed |
| X11 | 启动/退出循环 100 次 | 进程句柄数、线程数、GDI/USER 对象没有持续增长趋势（PerfMon/Handle 记录曲线） |
| X12 | ssh.exe 正在等待密码输入时关闭 | 正常关闭，快照文件被删除 |

### 4.2 IO-01

| # | 场景 | 通过标准 |
|---|---|---|
| I01 | 32 KiB bracketed paste，远端 `cat >/dev/null` | 开始标记、正文、结束标记连续，中间没有其他字节（在 ConPTY 前抓取字节流对比） |
| I02 | 粘贴过程中远端发送 DA/DSR 查询 | 应答在粘贴事务**之后**发出，不插进事务中间 |
| I03 | 32 KiB + 1 字节的粘贴 | 被拒绝并提示，不自动拆分 |
| I04 | 远端停止读取（`sleep 3600`，不读 tty）并持续输入 | 达到 4 MiB 时 QueueFull；5 秒无进展报告 InputStalled 并进入故障清理；核心线程一直不阻塞（看执行器心跳） |
| I05 | 远端连续发出大量查询，而输入端被堵住 | 64 KiB 应答队列满时报告 ReplyBackpressure，不阻塞 |
| I06 | `yes` 或 10 MiB 输出，同时模拟核心变慢 | 水位在 3 MiB 停读、1 MiB 恢复，队列峰值不超过 4 MiB，正常路径不丢字节（校验哈希） |
| I07 | 中文 UTF-8 输入跨 4 KiB 单元边界 | 不切开多字节字符 |
| I08 | 粘贴中途取消 | 仍然发出结束标记；通道已失败则关闭会话 |

### 4.3 CAP-01

| # | 场景 | 记录内容 |
|---|---|---|
| P01 | 远端 `echo $TERM`、`infocmp`、`tput colors` | 协商到的 TERM 以及 terminfo 是否存在 |
| P02 | 远端用 `printf` 发出 SGR 256 色、真彩、粗体、斜体、下划线 | 对比 ConPTY 前（远端 `script` 录制）和 ConPTY 后（outRead 原始字节）的序列，找出改写和丢失 |
| P03 | DA1、DA2、DSR 6n、XTVERSION、DECRQM 查询 | 由谁应答（ConPTY 自己应答，还是透传给核心）；是否出现**重复应答** |
| P04 | 鼠标模式 1000/1002/1006 加 vim 鼠标 | 鼠标序列能否经过 ConPTY 输入端到达远端 |
| P05 | 备用屏 1049、bracketed paste 2004、焦点 1004 | 是否透传 |
| P06 | OSC 0/2 标题、OSC 8、OSC 52 | OSC 52 **读**被 conhost 输出侧吞掉（已定，见 ADR-002）；写透传。OSC 8 等仍待探针 |
| P07 | resize 之后 | ConPTY 是否插入重绘序列，以及是否带来光标偏移 |
| P08 | `PSEUDOCONSOLE_INHERIT_CURSOR` 对比 | 首屏行为差异 |

结论写成能力矩阵（每项标注：透传、改写、ConPTY 应答、丢失），每一项都附原始字节记录。**CAP-01 不通过就不冻结 core/transport 职责**（第 19.1 节 T03A 门槛）；必要时评估独立 SSH 通道。

---

## 5. 第 16 节 Hyper-V 解析的 PowerShell 查询方案（T09）

### 5.1 原则

- 只支持本机宿主机，按 **VM ID（GUID）** 查询，不用名称或通配符匹配。
- 用**固定脚本文本**加**绑定参数**：通过 `System.Management.Automation.PowerShell` 托管 API 的 `AddScript(固定脚本).AddParameter("VmId", guid)`；或者启动 `powershell.exe -NoProfile -NonInteractive -File <应用自带的只读 .ps1> -VmId <GUID>`，参数作为数组里单独的一项传递。**禁止**把 VM ID 拼进 `-Command` 字符串。
- 应用端先用 `Guid.TryParse` 或等价方法校验 VM ID 格式；脚本里再用 `[Guid]` 类型约束校验一次。
- 结果只用 `ConvertTo-Json -Compress` 返回结构化数据，不解析格式化文本。
- 单次查询超时 5 秒，轮询间隔 2 秒，等待启动和地址的总预算 60 秒；可以取消（终止查询进程或者调用 `PowerShell.Stop()`）。

### 5.2 固定查询脚本（只读）

```text
param([Parameter(Mandatory)][Guid]$VmId)
$ErrorActionPreference = 'Stop'
if (-not (Get-Module -ListAvailable -Name Hyper-V)) { emit {status:'ModuleMissing'}; exit }
try { $vm = Get-VM -Id $VmId } catch [UnauthorizedAccess / VirtualizationException] → emit {status:'AccessDenied' or 'NotFound', detail:类型名}
$nics = Get-VMNetworkAdapter -VM $vm
emit {
  status: 'Ok', vmId: $vm.Id, name: $vm.Name, state: $vm.State.ToString(),
  heartbeat: (Get-VMIntegrationService -VM $vm -Name Heartbeat).PrimaryStatusDescription,  # 名称可能本地化，待实测，改为按 Id 过滤
  adapters: [ for each $n: { id:$n.Id, name:$n.Name, mac:$n.MacAddress, switch:$n.SwitchName, ips:@($n.IPAddresses) } ]
} | ConvertTo-Json -Depth 4 -Compress
```

（伪代码，只描述字段和流程；实现由架构师完成。）

### 5.3 启动操作（单独的管理操作）

- 另一个固定脚本 `Start-VM -VM (Get-VM -Id $VmId)`，同样只接受绑定参数。只有用户点击“启动并连接”时调用，**只发一次**。
- 需要提权时单独发起提权请求（只针对这一步），终端主进程**不**以管理员身份运行。没有权限时显示“需要 Hyper-V Administrators 组或管理员权限”，普通 SSH 不受影响。

### 5.4 应用端处理

| 脚本结果 | 映射到第 16 节 |
|---|---|
| ModuleMissing / AccessDenied | “显示具体缺失能力”，提供手动目标入口 |
| NotFound | “虚拟机已移除或标识变化”，不自动换成同名 VM |
| state≠Running | “启动并连接 / 取消” |
| Running，过滤后地址为 0 | “等待地址，可改用手动地址”，有界重试 |
| 过滤后地址为 1 | 直接作为本次目标 |
| 过滤后地址多于 1 | 选择器（地址、网卡、地址族），并记住网卡 ID |

地址过滤：排除 `0.0.0.0`、`::`、回环地址，以及没有作用域的 `fe80::/10`；**不**排除私有网段；IPv4 优先只是排序策略。

### 5.5 VM-01 验证点

关机、运行但无地址、多网卡、VM 改名（按 ID 仍能命中）、删除后建一个同名新 VM（不能误连）、非管理员用户、查询中途取消、超过 5 秒超时（用注入延迟模拟）、VM ID 内容为 `'; Stop-Computer` 这类注入字符串（格式校验直接拒绝，不会执行）。

---

## 6. 待实测和未决项

| 项 | 归属 | 最晚 |
|---|---|---|
| Win11 不同 build 上 ClosePseudoConsole 是否等待排空 | T03A X02 | T05 前 |
| 写取消方式：CancelSynchronousIo 还是 overlapped 加 CancelIoEx | T03A X04 | T06 |
| PROC_THREAD_ATTRIBUTE_JOB_LIST 是否可用，能否和 PSEUDOCONSOLE 属性组合 | T03A | T06 |
| `-F` 是否同时屏蔽 ProgramData 系统配置 | SSH-CFG-01 C02 | T07 |
| OpenSSH token 和引号的编码规则 | C20 | T07 |
| Heartbeat 集成服务的名称是否本地化 | T09 | T09 |
| 专用 Ubuntu SSH 测试目标（地址和测试账号，只用密钥） | Gavin 提供 | T03A 开始实连前 |
