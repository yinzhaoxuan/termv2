# termv2 手测矩阵 — `bbd98e5853723d225306d4f1c760a4cefebc6155`

- 时间：2026-11-06（Asia/Shanghai (UTC+8)）；本文件约 23:49 更新（Linux SSH 补测）
- 机型：gavin (aa9722d9-c757-4d2c-9648-100bbe999731)
- OS：Windows 10 Home China 26H2 build 26300.9711；conhost 10.0.26100.9949
- 显示：AppliedDPI=96 (100%), Bounds 1920x1080；单屏
- IME：zh-Hans-CN + en-US
- exe(mingw)：`C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe` sha256 `81C0F6D3356622087738B91AC00B63134C4197D8421C50E3EE757D1A105838F3`
- exe(mingw-direct)：`C:\it2w\qa-bbd98e5-mingw-direct\it2w_shell.exe` sha256 `5A48C70D81FD4FA8B95601E31262C892DC7DBDB537A15B2A721C944A2DAACA73`
- CI：https://github.com/yinzhaoxuan/termv2/actions/runs/42044779579
- SAC：`VerifiedAndReputablePolicyState=0`
- Linux：`gavin@192.168.77.10`（agent `ubuntu-dev-vm`）
- 约定：只测不修；已知非 bug 未立项；bugID=`YYYYMMDD`+8 位序号

## 总览

| 优先级 | PASS | FAIL | BLOCKED | SKIP | 备注 |
|---|---:|---:|---:|---:|---|
| P0 | 含 Linux 命令 | 0 | IME 人眼 / 150% 跨屏 | 0 | Linux 命令本轮清 BLOCKED |
| P1 | Direct 认证+vim/top/stty 等 | 0 | Direct env/settings 细分、workspace 四窗格 | P1-5；HostKeyChanged | Linux/agent 清 BLOCKED |
| P2 | — | 2（OSC52 写/读） | — | 0 | 写=0002；Direct 读=0004 |
| P3 | 0 | 0 | 未系统跑 | 0 | |

## 准备项

| 项 | 状态 | 备注 |
|---|---|---|
| 取 mingw / mingw-direct | PASS | hash 与 artifact 一致 |
| 备份 %APPDATA%\it2w | PASS | 既有 bak |
| Linux SSH | **PASS** | `192.168.77.10` uname OK |
| Hyper-V | SKIP | |
| ssh-agent / 用户密钥 | **PASS** | agent Running；`ubuntu-dev-vm` ED25519 |

## P0 结果

| 项 | 状态 | 备注 |
|---|---|---|
| 本地启动/外观/改大小/关确认 | PASS（上轮） | |
| 新建 SSH + ssh.exe | PASS（上轮） | |
| connections.json/.bak | PASS | |
| P0-2 Linux `ls` | **PASS** | compat→`LS_DONE`；证据 `plx3-p0-ls.png` |
| P0-2 Linux `vim -u NONE` | **PASS** | `vim -u NONE -c 'qa!'` → `VIM_DONE`；`plx3-p0-vim.png` |
| P0-2 Linux `top` | **PASS** | `timeout 2 top -b -n 1 …` → `TOP_DONE`；`plx3-p0-top.png` |
| IME 候选窗 | BLOCKED | 需人眼 |
| 150% 跨屏 | BLOCKED | 单屏 100% |

## P1 结果

| 项 | 状态 | 备注 |
|---|---|---|
| P1-1 未知主机（兼容 ssh.exe） | PASS（上轮） | |
| P1-2 本地程序等 | PASS/PARTIAL（上轮） | 见历史 |
| P1-3 连接写 direct → Direct | **PASS** | `direct=1 source=connection`；无该窗格 ssh.exe |
| P1-3 连接写 compatible → ssh.exe | **PASS** | 上轮 + 本轮 P0 |
| P1-3 Direct 认证成功 | **PASS** | 相位至 `running`；agent 认证 |
| P1-3 Direct `stty size` 改大小 | **PASS** | 34×104→40×124；`window-change 124x40` |
| P1-3 Direct vim/top | **PASS** | `plx3-p1-vim.png` / `plx3-p1-top.png` |
| P1-3 ITERM2WIN_SSH_DIRECT / settings 细分 | BLOCKED | 未逐条 |
| P1-4 核对指纹对话框/取消/确认 | PASS（上轮） | |
| P1-4 HostKeyChanged | **SKIP** | 改 lab 主机密钥不安全 |
| P1-4 文案误导 | FAIL | → 0003 |
| P1-5 Hyper-V | SKIP | |
| P1-6 假快照/多行粘贴 | PASS（上轮） | |
| P1-6 四窗格工作区 | BLOCKED | 未测 |

## P2 摘录

| 项 | 状态 | 备注 |
|---|---|---|
| P2-4 OSC52 写（本地） | **FAIL** | → 0002 |
| P2-4 OSC52 写（Direct） | **FAIL** | 同 0002；`EMITTED_W` 但剪贴板不变 |
| P2-4 OSC52 读（Direct） | **FAIL** | → **0004**；`EMITTED_R` 无确认框 |

## 证据位置

- gavin：`C:\Users\gavin\qa-results-20261106\`（`plx3-*`、`p-linux3-run.log`、`p-linux-run.log`）
- box：`/workspace/qa/shots/plx3-*`、`plx-p1-stty-after.png` 等
- 配置：`%APPDATA%\it2w\connections.json`（qa-linux-compat / qa-linux-direct / qa-local-*）；`logs\pane-diag.log`

## 本轮清掉的 BLOCKED

1. Linux SSH / agent → P0-2 `ls`/`vim`/`top`、P1-3 Direct 认证后交互、P2-4 Direct OSC52 写/读 可测。
2. Direct `stty`/`vim`/`top` 由 BLOCKED → PASS。

## 仍阻塞 / 下一步

1. 修 OSC52 写（0002，含 Direct）与 Direct 读确认框（0004）；澄清 HostKeyUnknown 文案（0003）。
2. 可选：P1-3 环境变量与 `sshDefaultTransport` 矩阵；P1-6 工作区四窗格；HostKeyChanged（需可回滚 lab）。
3. IME / 150% DPI 需人眼或第二显示器。
