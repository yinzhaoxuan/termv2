# termv2 手测矩阵 — `bbd98e5853723d225306d4f1c760a4cefebc6155`

- 时间：2026-11-06（Asia/Shanghai (UTC+8)）；本文件约 23:30 更新（P1/P2 续测）
- 机型：gavin (aa9722d9-c757-4d2c-9648-100bbe999731)
- OS：Windows 10 Home China 26H2 build 26300.9711；conhost 10.0.26100.9949
- 显示：AppliedDPI=96 (100%), Bounds 1920x1080；单屏
- IME：zh-Hans-CN + en-US
- exe(mingw)：`C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe` sha256 `81C0F6D3356622087738B91AC00B63134C4197D8421C50E3EE757D1A105838F3`
- exe(mingw-direct)：`C:\it2w\qa-bbd98e5-mingw-direct\it2w_shell.exe` sha256 `5A48C70D81FD4FA8B95601E31262C892DC7DBDB537A15B2A721C944A2DAACA73`
- CI：https://github.com/yinzhaoxuan/termv2/actions/runs/42044779579
- SAC：`VerifiedAndReputablePolicyState=0`
- 约定：只测不修；已知非 bug 未立项；bugID=`YYYYMMDD`+8 位序号

## 总览

| 优先级 | PASS | FAIL | BLOCKED | SKIP | 备注 |
|---|---:|---:|---:|---:|---|
| P0 | 多数子项 | 0 | IME 人眼 / 150% 跨屏 / Linux 命令 / 完整退出残留 | 0 | 上轮；未回归 |
| P1 | 多项见下 | 0 | Direct env/settings 细分、HostKeyChanged、Linux、workspace 四窗格 | P1-5 | 本轮重点 |
| P2 | 多行粘贴预览（归 P1-6） | 1（OSC52 写） | OSC52 读 | 0 | P2-4 写 → bug 0002 |
| P3 | 0 | 0 | 未系统跑 | 0 | |

## 准备项

| 项 | 状态 | 备注 |
|---|---|---|
| 取 mingw / mingw-direct | PASS | hash 与 artifact 一致 |
| 备份 %APPDATA%\it2w | PASS | 既有 bak；本轮另备 known_hosts |
| Linux SSH | BLOCKED | 仅本机 Windows OpenSSH |
| Hyper-V | SKIP | |
| ssh-agent / 用户密钥 | BLOCKED | agent Stopped；无可用公钥 → Direct 认证失败（信任流程仍可测） |

## P0 结果

（上轮，未重跑。详见历史版本。）本地启动/外观/改大小/关确认、新建 SSH+ssh.exe、connections.json/.bak：**PASS**。Linux 命令 / IME 候选窗 / 150%：**BLOCKED**。

## P1 结果（本轮）

| 项 | 状态 | 备注 |
|---|---|---|
| P1-1 未知主机（兼容 ssh.exe） | PASS（上轮） | yes → known_hosts |
| P1-1 其余认证分支 | BLOCKED | 无密码用户/密钥文件环境 |
| P1-2 本地程序 powershell -NoLogo -NoExit | **PASS** | 子进程 `powershell.exe -NoLogo -NoExit`；双击树项可开 |
| P1-2 cwd 留空 → 用户主目录 | **PASS** | 提示符 `PS C:\Users\gavin>`（qa-local-pwsh） |
| P1-2 显式工作目录 | **PASS** | qa-local-env → `PS C:\Windows\Temp>` |
| P1-2 程序不存在 | **PASS** | 「会话失败: 未找到程序: …\missing-shell.exe」+「重连」 |
| P1-2 environment.set/unset | PARTIAL | JSON 已写 `IT2W_QA_MARKER`；未截到 echo 回显 |
| P1-2 参数含空格/中文 + 编辑器浏览新建 | BLOCKED/PARTIAL | 编辑器填表自动化不稳定；以 JSON 路径验收主行为 |
| P1-3 连接写 direct → Direct（mingw-direct） | **PASS** | `ssh.transport direct=1 source=connection`；`ssh.direct.phase` known_hosts/dns/connect/handshake/hostkey；**无**该窗格 ssh.exe |
| P1-3 连接写 compatible → ssh.exe | **PASS** | `direct=0 source=connection`；子进程 `ssh.exe -F …\snapshots\ssh-*-*.conf it2w-target` |
| P1-3 普通 mingw + direct 连接 | **PASS** | 覆盖层「此构建未包含直连 SSH。改用 ssh.exe: 在连接编辑器里把传输改为兼容。」 |
| P1-3 ITERM2WIN_SSH_DIRECT=0/1、settings auto/非法大小写等 | BLOCKED | 本轮未逐条跑 |
| P1-3 Direct 下 vim/top/stty | BLOCKED | 无 Linux / agent 登录失败 |
| P1-4 核对指纹对话框 | **PASS** | 按钮「核对指纹…」→ 对话框「核对主机指纹」（类型/SHA256/known_hosts 路径）；默认焦点在「取消」 |
| P1-4 取消不写 known_hosts | **PASS** | cancel 后文件仍空 |
| P1-4 确认写入并重连 | **PASS** | 写入 `127.0.0.1 ssh-ed25519 …`；diag 下一 attempt 过 hostkey 进 agent，因 pipe 失败（环境） |
| P1-4 HostKeyChanged | BLOCKED | 未改服务器主机密钥 |
| P1-4 文案误导 | **FAIL 文案** | → bug `2026110600000003`（功能按钮可用） |
| P1-5 Hyper-V | SKIP | |
| P1-6 假快照启动清理 | **PASS** | 植入 `ssh\snapshots\ssh-1-2-3.conf`，启动后不存在 |
| P1-6 多行粘贴预览 | **PASS** | 「将粘贴 3 行、29 字节」；确定/取消按钮；取消后未发送 |
| P1-6 四窗格工作区恢复 | BLOCKED | 未测 |

## P2 摘录

| 项 | 状态 | 备注 |
|---|---|---|
| P2-4 OSC52 写（本地） | **FAIL** | → bug `2026110600000002` |
| P2-4 OSC52 读（Direct） | BLOCKED | 信任后无 agent，无交互 shell；compat/本地读按 ADR-002 预期不弹框未专项复验 |

## 证据位置

- gavin：`C:\Users\gavin\qa-results-20261106\`（p1b/p1c/p1f/p1i 截图与 `p1*-run.log`）
- box：`/workspace/qa/shots/`（p1b-p1-3-direct、p1b-p1-3-nodirect-build、p1c-trust-dlg、p1f-osc52、p1f-after-scan 等）
- 配置：`%APPDATA%\it2w\connections.json`（qa-compat-ssh / qa-direct-ssh / qa-local-*）；`logs\pane-diag.log`

## 阻塞与下一步

1. 提供可登录 Linux SSH（agent 或密钥），补 Direct 认证成功后的 vim/top/stty、OSC52 读、P0 Linux 命令。
2. 修/复核 OSC52 本地写（0002）；澄清 HostKeyUnknown 文案（0003）。
3. 补 P1-3 环境变量与 `sshDefaultTransport` 矩阵；P1-6 工作区四窗格；可选 HostKeyChanged。
