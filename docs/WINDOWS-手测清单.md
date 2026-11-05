# Windows 实机手测清单

更新：2026-11-05，main `38a258b` 之后。
用途：把散在各设计文档「只能在 Windows 上验」一节、STATUS、HANDOFF §4.6、REVIEW §8 C 批里的手测项合成一份，按优先级排好。截至本日，**下面没有一项在 Windows 上跑过**。

做法：从上往下做。P0 不过就先停下来报告，后面的多半也会受影响。每节末尾的「来源」指向写明期望的设计文档，有疑问以那里为准。

## 0. 准备

- [ ] **取构建产物**。GitHub → Actions → CI，选最新一次 main 的运行，下载 artifact `it2w_shell-mingw-<提交>`（`it2w_shell.exe` 与 `licenses/`）。
- [ ] **取 Direct 构建**：同一次运行里的 `it2w_shell-mingw-direct-<提交>`（ADR-006 之后每次推送都编）。Direct 相关的测试（P1-3、P1-4、P1-5、P2-4 读）用这个 exe；它也是发布包用的构建。
- [ ] 两个 exe 分别放进不同目录（各带自己的 `licenses/`）。配置目录二者共用：`%APPDATA%\it2w`。如果以前用过旧版本，**先备份这个目录**。
- [ ] 测试机记录：Windows 版本与内部版本号（`winver`），conhost 版本，显示缩放（100% / 150%），是否装了中文输入法。
- [ ] 准备一台 Linux SSH 服务器：一个用户用密钥登录（公钥已 `ssh-add` 进 Windows OpenSSH agent），另一个用户只允许密码。有条件再准备一台 Hyper-V 虚拟机（Linux，开 sshd）。
- [ ] 可选：DebugView（看 `OutputDebugString`）。

**两条约定**：

- 清单里的 `printf`、`cat`、`yes`、`vim`、`top`、`stty` 都在 **SSH 会话（Linux bash）** 里执行，本地 cmd 没有这些命令。
- 手改 `settings.json` / `connections.json` 后要**重启 it2w_shell** 才生效（只在启动时读一次）；用界面（编辑器、菜单）改的立即生效。

**每次测试记录**：用系统菜单（标题栏图标或 Alt+Space）「导出诊断包…」，里面有「提交（配置时）」和 Windows 版本（REVIEW C6）。失败时把诊断包、截图和步骤一起发回。

## P0 冒烟（先做）

### P0-1 启动与本地终端
- [ ] 双击 `it2w_shell.exe`：窗口出现，自动开一个本地终端（cmd 或 `%ComSpec%`），能输入 `dir`、`echo 中文`。
- [ ] 外观与之前的版本一致：背景深灰 `#1E1E1E`、前景浅灰、光标蓝色（DS-9 §7-1）。
- [ ] 拖动窗口边缘改变大小：行列跟着变，没有残影；`mode con` 显示的尺寸与窗口相符。
- [ ] 中文输入法：候选窗出现在光标附近，上屏的中文正确。
- [ ] 在 100% 和 150% 缩放的显示器之间拖动窗口：字号与格子随之调整（Per-Monitor V2）。
- [ ] 关闭窗口：有运行中的会话时先弹确认，确认后退出，任务管理器里没有残留的 `it2w_shell.exe`、`conhost.exe`。

来源：HANDOFF §4.6、REVIEW C3、DS-9 §7。

### P0-2 新建 SSH 连接并连接（GAP-07 验收路径）
- [ ] 会话树空白处右键「新建连接…」：填名称、主机、用户名，认证 agent，传输「跟随全局默认」→ 确定。会话树出现这条连接。
- [ ] 双击连接：经 `ssh.exe` 连上（任务管理器能看到该窗格的 `ssh.exe`）。`ls`、`vim`、`top` 正常。
- [ ] 右键「编辑…」改端口或用户名 → 确定 → 新开标签按新配置连接；已经开着的窗格不受影响。
- [ ] `%APPDATA%\it2w\connections.json` 里能看到这条连接，旁边有 `.bak`。

来源：DS-14 §7-1。

## P1 连接与传输

### P1-1 兼容模式 SSH 的错误分支
- [ ] 未知主机：`ssh.exe` 在终端里询问指纹，确认后连上；再连不再询问。
- [ ] 服务器换主机密钥后再连：失败，提示主机密钥变化。
- [ ] 主机写错 / 端口不通：失败，文字是「连接被拒绝」或超时一类，不卡在「正在连接」。
- [ ] 只允许密码的用户、认证选「交互」：在终端里输入密码能登录。
- [ ] 密钥文件认证（编辑器里选「密钥文件」→ 浏览）：能登录；路径含中文也可以。

来源：REVIEW C2、T07。

### P1-2 本地程序连接
- [ ] 新建「本地程序」连接：浏览选 `pwsh.exe` 或 `powershell.exe`，参数两行（如 `-NoLogo` 与 `-NoExit`），工作目录留空 → 启动在用户主目录。
- [ ] 参数含空格和中文、显式工作目录：都按原样生效。
- [ ] 手改 connections.json 给它加 `environment.set` / `unset`：生效。
- [ ] 程序路径或工作目录不存在：窗格显示明确的错误，可以重连。

来源：DS-14 §7-3、STATUS 2026-11-04「A5、A1 余量、DS-2、B1–B5」。

### P1-3 Direct SSH 与按连接选择传输（用 mingw-direct 的 exe）
- [ ] 不设环境变量：编辑器里把一条连接的传输改成「直连」→ 连接走 Direct（`pane-diag.log` 里有 `ssh.direct.*`，任务管理器里**没有**该窗格的 `ssh.exe`）；其他连接仍是 `ssh.exe`。
- [ ] 设 `ITERM2WIN_SSH_DIRECT=1` 后启动：传输写「兼容」的连接仍是 `ssh.exe`，没写的走 Direct。
- [ ] 「直连」+ 密钥文件：编辑器保存时报错、焦点落在认证；手改 JSON 绕过时，启动时状态栏报连接配置无效。
- [ ] 用**普通** mingw exe（不含 Direct）连一条「直连」连接：窗格 Failed，文字提示「此构建未包含直连 SSH」。
- [ ] Direct 下 `vim`、`top`、改窗口大小（远端 `stty size` 跟着变）、`exit` 都正常。
- [ ] 设 `ITERM2WIN_SSH_DIRECT=0` 后启动：没写传输的连接走 `ssh.exe`，即使 settings.json 写了 `"sshDefaultTransport": "direct"`；写「直连」的连接仍走 Direct。
- [ ] 不设环境变量，settings.json 写 `"sshDefaultTransport": "auto"` 后重启：没写传输的 agent 连接走 Direct，密钥文件 / 密码连接走 `ssh.exe`。编辑器里第一项显示「跟随全局默认（当前：直连）」，把认证切到「密钥文件」后变成「（当前：兼容）」。
- [ ] 同上，把 agent 服务停掉再连 agent 连接：窗格 Failed，文字末尾说明「此连接因 settings.json 的 sshDefaultTransport 使用直连」及改法；不会自己改用 `ssh.exe`。
- [ ] settings.json 写 `"sshDefaultTransport": "Direct"`（大小写错）：启动时状态栏提示「sshDefaultTransport 无效，已按默认处理」，连接走 `ssh.exe`；之后在界面里改快捷键等触发保存，文件里的 `"Direct"` 原样保留。
- [ ] `pane-diag.log` 每次连接有一行 `ssh.transport direct=… source=…`，来源与上面各步一致；「导出诊断包…」的配置摘要有 `sshDefaultTransport = …`。

来源：DS-13 §6、ADR-003、ADR-006。

### P1-4 Direct 首次信任（用 mingw-direct 的 exe）
- [ ] 连一台 `%APPDATA%\it2w\ssh\known_hosts` 里没有的主机：窗格出现「核对指纹…」；点开显示类型与 `SHA256:` 指纹，默认按钮是取消。
- [ ] 取消：不写 known_hosts。确认：写入一行并自动重连成功；之后再连直接成功。
- [ ] 改服务器主机密钥后再连：失败，提示密钥变化，**没有**信任按钮。

来源：DS-12 §6。

### P1-5 Hyper-V
- [ ] 编辑器新建「SSH（Hyper-V 虚拟机）」→「选择…」：列出虚拟机（未提权 / 未装 Hyper-V 时显示对应中文错误）；选中后网卡列表更新；保存后能连接，标题为「Hyper-V <地址>」。
- [ ] 虚拟机关机时连接：窗格给出「启动并连接」，点击后启动并连上；普通双击不会启动虚拟机。
- [ ] 多网卡 / 多地址：给出「选择地址…」，选中后只影响这一次。
- [ ] 解析失败且填了回退地址：标题为「手动地址 <地址>」。
- [ ] 用 mingw-direct exe 连一台新虚拟机：首次信任按 `hv-<guid>` 写进 known_hosts；改虚拟机 IP（DHCP）后仍能直接连上。

来源：DS-14 §7-2、STATUS「B1」、DS-12 §6、REVIEW C4。

### P1-6 工作区、粘贴与快照
- [ ] 开四个 SSH 窗格（分屏）后退出再启动：布局恢复，四个窗格都是「未连接」占位，任务管理器里 `ssh.exe` 数为 0；点「连接」或在占位上按 Enter 才启动。
- [ ] 粘贴多行文本：先弹预览确认；确定只发送一次、不额外补回车；取消一个字节都不发。
- [ ] 在 `%APPDATA%\it2w\ssh\snapshots` 里放一个假的 `ssh-1-2-3.conf`，启动后被清掉。

来源：STATUS「B4、B5、A1」。

## P2 界面功能

### P2-1 连接编辑器的其余项
- [ ] 校验：端口填 `abc` → 红字提示、焦点在端口、对话框不关。
- [ ] 分组：新建、重命名、删除（连接移到未分组）；树的展开状态不乱。F2 编辑 / 重命名、Delete 删除、Shift+F10 弹菜单。
- [ ] 删除一条有运行中窗格的连接：窗格继续运行。
- [ ] 程序运行时用记事本改 connections.json，再用编辑器保存：拒绝并提示重启。
- [ ] 把 connections.json 的 `schemaVersion` 改成 99 后启动：右键菜单项置灰，并说明只读。
- [ ] 150% 缩放下对话框不裁切、Tab 顺序合理、Esc 取消。

来源：DS-14 §7。

### P2-2 外观与配色
- [ ] 系统菜单「导入 iTerm2 配色…」导入一个 `.itermcolors`（如 Solarized Dark）：确认框列出读到的项；已打开的本地与 SSH 窗格立即换色，新窗格也是新配色。「恢复默认配色」后回到原样。
- [ ] 在终端里 `printf '\e]11;#336699\a'`：背景变色；`printf '\e]11;?\a'` 的应答与屏幕一致；`printf '\e]111\a'` 恢复。
- [ ] settings.json 定义只改背景的 Profile `prod`，某连接写 `"appearanceProfileId": "prod"`：只有该连接背景不同。写一个不存在的 ID：状态栏提示一次并用默认。
- [ ] `scrollbackLines: 200` 的 Profile：输出 1000 行后只能回滚约 200 行；默认能回滚到 10000 行。
- [ ] `vim -u NONE` 里 `:echo v:termresponse` 有值，启动没有明显延迟；波浪下划线（`printf '\e[4:3mwavy\e[0m\n'`）画成波浪线。

来源：DS-9 §7、HANDOFF §4.6。

### P2-3 快捷键、Ctrl+C、诊断包
- [ ] 系统菜单「快捷键…」：26 个动作齐全；在 settings.json 改一个、解绑一个（设为 `""`）、制造一个冲突，重启后状态列分别显示「已修改」「未绑定」「冲突」；「打开配置目录」能打开。
- [ ] 选中一段文字按 Ctrl+C：复制并清除选区；没有选区时 Ctrl+C 中断前台程序（`ping -t` 能被停下）。settings.json 设 `"copyOrInterrupt": ""` 后，Ctrl+C 总是中断，且之后改字号仍能保存设置。
- [ ] 「导出诊断包…」：预览可滚动、日志很大时不卡死；主机、用户、IP、指纹已替换为 `<host-1>` 之类；保存的文件用记事本打开中文正常；取消不写文件；同名提示覆盖。

来源：DS-16 §4、ADR-004、DS-15 §7。

### P2-4 OSC 52 剪贴板（读需要 mingw-direct exe）
- [ ] settings.json 加 `"osc52": {"allowWrite": true, "allowRead": true, "maxBytes": 0}` 后重启。
- [ ] 任一会话 `printf '\e]52;c;aGVsbG8=\a'`：剪贴板变成 `hello`，状态栏提示一次。
- [ ] Direct 会话 `printf '\e]52;c;?\a'`：弹确认框（默认「拒绝」），预览显示当前剪贴板；允许后远端收到 base64；拒绝后没有应答；勾选「本会话记住」后同一会话不再询问，重连后重新询问。
- [ ] 兼容 SSH / 本地会话里同样的读请求：不弹框（conhost 吞掉了请求，ADR-002）。
- [ ] 有条件：Neovim 配 OSC 52 provider，粘贴时确认后成功。

来源：DS-10 §7。

## P3 压力与资源

### P3-1 内存账本
- [ ] 开 8 个窗格各 `cat` 一个大文件到历史上限：任务管理器里的进程内存与诊断包「内存（账本）」同一量级（账本不含显存与传输队列，会偏小）。
- [ ] Profile 设 `memoryLimitMiB: 1024` 的窗格输出 200 MiB：历史被压到约 128 MiB，状态栏提示一次。
- [ ] 合计接近 512 MiB 时新开标签：被拒绝，提示文字正确；关几个窗格后可以新开。

来源：DS-17 §6。

### P3-2 洪峰与关闭
- [ ] `cat` 一个 50 MB 文件后 `exit`：尾部完整，最终状态是「已退出」。洪峰期间点一个链接，确认框约 0.25 s 内弹出。
- [ ] `yes` 刷屏时 Ctrl+Shift+W 确认关闭，重复 100 次：不崩溃。
- [ ] 输出被丢弃时（洪峰中强行关闭）：状态栏显示「输出不完整（丢弃 …）」。
- [ ] 对同一个 SSH 窗格重连 100 次：任务管理器里进程数、句柄数、内存没有持续上涨（REVIEW C5）。
- [ ] 四个窗格同时持续输出时，键盘输入的回显延迟可接受（REVIEW C3 要求记录延迟分位数，能测则测）。

来源：HANDOFF §4.6、ADR-001、REVIEW C3 / C5。

### P3-3 诊断输出
- [ ] DebugView：默认不刷 `[conpty] reply written=`；设 `ITERM2WIN_OSC52_PING=1` 后才有。
- [ ] `%APPDATA%\it2w\logs\pane-diag.log` 存在，每行有 sid、attempt、phase；超过 5 MiB 后滚动出 `.1`、`.2`。

来源：HANDOFF §4.6、STATUS「D-2」「D-5」。

## 回报格式

每做完一节，回一段：

```
P1-3  提交 <诊断包里的提交号>  Windows <版本>  exe: mingw-direct
- [x] 第 1 条
- [ ] 第 3 条：<现象>，诊断包 it2w-diag-YYYYMMDD-HHMMSS.txt
```

我据此修问题，并把结果写进 STATUS（绑定提交号与 Windows 版本）。全部通过的节，也请回一句「通过」。
