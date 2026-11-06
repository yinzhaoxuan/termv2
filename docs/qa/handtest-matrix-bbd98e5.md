# termv2 手测矩阵 — `bbd98e5853723d225306d4f1c760a4cefebc6155`

- 时间：2026-11-06（Asia/Shanghai (UTC+8)），SAC 关闭后重跑；本文件约 22:59 更新
- 机型：gavin (aa9722d9-c757-4d2c-9648-100bbe999731)
- OS：Windows 10 Home China 26H2 build 26300.9711；conhost 10.0.26100.9949
- 显示：AppliedDPI=96 (100%), Bounds 1920x1080；单屏
- IME：zh-Hans-CN + en-US
- exe(mingw)：`C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe` sha256 `81C0F6D3356622087738B91AC00B63134C4197D8421C50E3EE757D1A105838F3`
- exe(mingw-direct)：`C:\it2w\qa-bbd98e5-mingw-direct\it2w_shell.exe` sha256 `5A48C70D81FD4FA8B95601E31262C892DC7DBDB537A15B2A721C944A2DAACA73`（已分期）
- CI：https://github.com/yinzhaoxuan/termv2/actions/runs/42044779579
- SAC：`VerifiedAndReputablePolicyState=0`（用户关闭后可启动）
- 约定：只测不修；已知非 bug 未立项

## 总览

| 优先级 | PASS | FAIL | BLOCKED | SKIP | 备注 |
|---|---:|---:|---:|---:|---|
| P0 | 多数子项 | 0 | 需人眼/多屏/Linux 的项 | 0 | 见下表 |
| P1 | 部分（未知主机密钥） | 0 | Direct 全量 / 密码与密钥路径 / 本地程序编辑器细项 | P1-5 | Direct 已分期未切 exe 跑完 |
| P2 | 0 | 0 | 未系统跑 | 0 | |
| P3 | 0 | 0 | 未系统跑 | 0 | pane-diag / glyphrun 有顺带观察 |

## 准备项

| 项 | 状态 | 备注 |
|---|---|---|
| 取 mingw artifact | PASS | 已部署 |
| 取 mingw-direct artifact | PASS | 自 `C:\Users\gavin\qa-bbd98e5-mingw-direct.zip` 解压；hash 与 box 一致 |
| 备份 %APPDATA%\it2w | PASS | `it2w.bak-qa-20261106-212312` |
| 记录 winver/DPI/IME | PASS | 见上 |
| Linux SSH 服务器 | BLOCKED | 无可用 Linux；仅本机 Windows OpenSSH（sshd Running）作冒烟 |
| Hyper-V | SKIP | `Get-WindowsOptionalFeature` 需提升；按清单可跳 |

## P0 结果

| 项 | 状态 | 备注 |
|---|---|---|
| P0-1 启动与本地终端 | PASS | 启动后自动 cmd + headless conhost；`dir` / `echo` / 粘贴 `echo CN_PASTE_OK` 正常；状态「本地终端 · 运行中」 |
| P0-1a 外观深灰/蓝光标 | PASS | 背景采样 `#1E1E1E,#1E1E1E,#1E1E1E`；截图可见蓝色块光标 |
| P0-1b 拖动改大小 / mode con | PASS | `SetWindowPos` 后 pane-diag grid `104x34 → 82x31 → 59x21`；无崩溃 |
| P0-1c 中文输入法候选窗位置 | BLOCKED | 需人眼看候选窗相对光标；UI 已能显示中文路径/分组名；未做 IME 上屏位置核对 |
| P0-1d 100%/150% 跨 DPI | BLOCKED | 仅单屏 100%；无第二屏 150% |
| P0-1e 关闭确认与无残留 | PASS（确认框）/ BLOCKED（完整退出残留核验） | `WM_CLOSE` 弹出「确认关闭」479x169（有运行中 cmd）；Esc 取消后进程仍在。未点确定做残留核验以免中断后续测 |
| P0-2 新建 SSH 连接 | PASS | 右键树空白「新建连接…」→ 填名称/主机/用户/agent → 确定；树出现 `qa-local-ssh · 127.0.0.1`；状态栏「已保存连接」 |
| P0-2 双击连接 / ssh.exe | PASS | 双击后子进程 `ssh.exe -F …\ssh\snapshots\ssh-9252-2-1.conf it2w-target`（ppid=it2w）；pane-diag `ssh.transport direct=0 source=builtin` |
| P0-2 connections.json / .bak | PASS | 首次写入生成 `connections.json`；二次写（新建分组）后出现 `connections.json.bak` |
| P0-2 Linux `ls`/`vim`/`top` | BLOCKED | 无 Linux 目标；本机 sshd + agent 模式在写入 known_hosts 后 `Connection reset` → UI「会话失败」+「重连」（环境：ssh-agent Stopped / 无公钥） |
| P0-2 编辑连接改端口后新开标签 | BLOCKED | 连接项右键菜单自动化未稳定命中「编辑…」（易落到空白处「新建连接/新建分组」） |

## P1 摘录（本轮触及）

| 项 | 状态 | 备注 |
|---|---|---|
| P1-1 未知主机指纹询问 | PASS | 终端显示 fingerprint；输入 yes 后 known_hosts 写入 `127.0.0.1 ssh-ed25519 …`（92 bytes）；再连应不再询问（未复测第二次） |
| P1-1 其余错误分支 / 密码 / 密钥文件 | BLOCKED | 缺密码用户与密钥路径环境 |
| P1-2 本地程序连接 | BLOCKED | 未系统测 |
| P1-3 Direct 传输选择 | BLOCKED | mingw-direct 已分期；本轮未切 Direct exe 跑矩阵；mingw 路径已见 `direct=0 source=builtin` |
| P1-4 Direct 首次信任 | BLOCKED | 需 Direct exe |
| P1-5 Hyper-V | SKIP | 需提升/无 Hyper-V 确认 |
| P1-6 工作区/粘贴/快照 | PARTIAL | 单行粘贴进终端成功；多行预览确认未专门测；快照清理未测 |

## 顺带观察（非正式 FAIL）

- 渲染路径默认 `path=glyphrun`（pane-diag `render.stats`）。
- 关闭 SSH 失败会话：diag 有 `close.*` / `session.end state=Failed` 流水，进程树回到仅 cmd+conhost。
- 测试过程中误建分组名 `echo 中文_IME`（自动化点错菜单），属测试污染非产品缺陷。

## 证据位置

- gavin：`C:\Users\gavin\qa-results-20261106\`（截图 + ua*.log）
- box：`/workspace/qa/shots/`（部分截图副本）
- 配置：`%APPDATA%\it2w\connections.json` / `.bak` / `ssh\known_hosts` / `logs\pane-diag.log`

## 下一步（需用户/环境）

1. 提供可 agent/密钥登录的 **Linux SSH**，补完 P0-2 远端命令与 P1 认证分支。
2. 人眼：IME 候选窗贴光标；有条件加 150% 第二屏。
3. 切 `qa-bbd98e5-mingw-direct` 跑 P1-3 / P1-4 / P2-4 读。
4. 点「确认关闭」核验退出后无 it2w_shell/conhost 残留（可选）。
