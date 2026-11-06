# termv2 手测矩阵 — `bbd98e5853723d225306d4f1c760a4cefebc6155`

- 时间：2026-11-06（Asia/Shanghai (UTC+8)）
- 机型：gavin (aa9722d9-c757-4d2c-9648-100bbe999731)
- OS：Windows 10 Home China 26H2 build 26300.9711；conhost 10.0.26100.9949
- 显示：AppliedDPI=96 (100%), Bounds 1920x1080
- IME：zh-Hans-CN (Chinese IME present) + en-US
- exe(mingw)：`C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe` sha256 `81C0F6D3356622087738B91AC00B63134C4197D8421C50E3EE757D1A105838F3`
- exe(mingw-direct)：**未分期成功**
- CI：https://github.com/yinzhaoxuan/termv2/actions/runs/42044779579
- 约定：只测不修；已知非 bug（CAP-01 P06 OSC52 read / DCS Sixel / OSC133 / Direct 非默认）未立项

## 总览

| 优先级 | PASS | FAIL | BLOCKED | SKIP |
|---|---:|---:|---:|---:|
| P0 | 0 | 0 | 全部 | 0 |
| P1 | 0 | 0 | 除 P1-5 | P1-5 |
| P2 | 0 | 0 | 全部 | 0 |
| P3 | 0 | 0 | 全部 | 0 |

**阻断根因**：Smart App Control / Code Integrity（`VerifiedAndReputablePolicyState=1`）拒绝未签名的 `it2w_shell.exe`。次要阻断：mingw-direct 制品未能完整落到 gavin。


## 准备项

| 项 | 状态 | 备注 |
|---|---|---|
| 取 mingw artifact | PASS | run 42044779579 → `/workspace/qa/artifacts-bbd98e5/mingw/`；已部署 `C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe` sha256 `81C0F6D3356622087738B91AC00B63134C4197D8421C50E3EE757D1A105838F3` |
| 取 mingw-direct artifact | BLOCKED | blob SAS ~10min 过期且本机下行 ~40KB/s；并行 Range 未完成；CopyFromBox 大文件频繁 unreachable；`C:\\it2w\\qa-bbd98e5-mingw-direct` 未创建；残余 zip 仅 3600384 字节 |
| 备份 %APPDATA%\\it2w | PASS | `C:\Users\gavin\AppData\Roaming\it2w.bak-qa-20261106-212312` |
| 干净分期目录 | PARTIAL | mingw OK；mingw-direct 缺失 |
| 记录 winver/DPI/IME | PASS | Windows 10 Home China 26H2 build 26300.9711；AppliedDPI=96 (100%), Bounds 1920x1080；zh-Hans-CN (Chinese IME present) + en-US；conhost 10.0.26100.9949 |
| Linux SSH 服务器 | BLOCKED | 未测到需要；本机有 OpenSSH Client |
| Hyper-V | SKIP | 未提权确认；按清单可跳 |


## 清单结果

| 项 | 状态 | 备注 |
|---|---|---|
| P0-1 启动与本地终端 | BLOCKED | Smart App Control/WDAC 拦截未签名 it2w_shell.exe（Explorer/cmd/ps/wscript 均 3077）；无法验证 dir/中文/缩放/关闭 |
| P0-1a 外观深灰/蓝光标 | BLOCKED | 同上，窗口未出现 |
| P0-1b 拖动改大小/mode con | BLOCKED | 同上 |
| P0-1c 中文输入法候选窗 | BLOCKED | 同上；机上已装 zh-Hans-CN IME |
| P0-1d 100%/150% 跨 DPI | BLOCKED | 本机主屏 AppliedDPI=96（100%）；无第二屏自动化；且 exe 未启动 |
| P0-1e 关闭确认与无残留进程 | BLOCKED | 同上 |
| P0-2 新建 SSH 连接并连接 | BLOCKED | UI 未启动；connections.json 当前不存在（仅 settings/workspace）；ssh.exe 可用：C\\WINDOWS\\System32\\OpenSSH\\ssh.exe |
| P0-2 双击连接/ssh.exe/树与 .bak | BLOCKED | 同上 |
| P1-1 兼容 SSH 错误分支 | BLOCKED | 依赖 P0-2 / UI |
| P1-2 本地程序连接 | BLOCKED | 依赖 UI |
| P1-3 Direct SSH / 传输选择 | BLOCKED | 需 mingw-direct exe；本轮 direct 分期目录未就绪（制品下载慢/中断）+ SAC 拦截 |
| P1-4 Direct 首次信任 | BLOCKED | 同上 |
| P1-5 Hyper-V | SKIP | Get-WindowsOptionalFeature 需提升；策略上按清单可跳过无 Hyper-V；且 exe 未启动 |
| P1-6 工作区/粘贴/快照 | BLOCKED | 依赖 UI |
| P2-1 连接编辑器其余项 | BLOCKED | exe 未启动 / 或需 direct |
| P2-2 外观与配色 | BLOCKED | exe 未启动 / 或需 direct |
| P2-2a 字形运行渲染 | BLOCKED | exe 未启动 / 或需 direct |
| P2-3 快捷键/Ctrl+C/诊断包 | BLOCKED | exe 未启动 / 或需 direct |
| P2-4 OSC52（读需 direct） | BLOCKED | exe 未启动 / 或需 direct |
| P3-1 内存账本 | BLOCKED | exe 未启动 |
| P3-2 洪峰与关闭 | BLOCKED | exe 未启动 |
| P3-2a 文本渲染耗时 | BLOCKED | exe 未启动 |
| P3-3 诊断输出 pane-diag/DebugView | BLOCKED | exe 未启动 |

## 证据 / 操作记录

1. Artifact 已用 `gh run download 42044779579` 落到 box：`/workspace/qa/artifacts-bbd98e5/{mingw,mingw-direct}/`。
2. mingw zip 经 gavin `curl` SAS 下完并 `Expand-Archive` → `C:\it2w\qa-bbd98e5-mingw\it2w_shell.exe`，hash 与 box 一致。
3. `%APPDATA%\\it2w` 已备份到 `C:\Users\gavin\AppData\Roaming\it2w.bak-qa-20261106-212312`（当时仅有 settings.json / workspace.json[.bak]，无 connections.json）。
4. 启动尝试：Start-Process / cmd start / explorer / Invoke-Item / WScript.Shell.Run → 全部被策略拦截；CodeIntegrity 3077/3033。
5. 旧目录 `C:\\it2w\\rb-*\\…\\it2w_shell.exe` 同样 NotSigned 且同样被拦（非本提交独有）。
6. Direct：单线程 curl ~40KB/s，SAS 窗口不够；8 路 Range 任务未在超时内完成即中止；大文件 CopyFromBox 多次 temporarily unreachable。

## 下一步（需用户/父代理）

1. 在 gavin 上关闭 Smart App Control（设置 → 隐私和安全性 → Windows 安全中心 → 应用程序和浏览器控制），或为 CI/发布产物加 Authenticode 签名后再测。
2. 重新拉取 `it2w_shell-mingw-direct-bbd98e5853723d225306d4f1c760a4cefebc6155` 到 `C:\\it2w\\qa-bbd98e5-mingw-direct\\`（建议在局域网/更快链路上拷贝 box 上已有 zip：`/workspace/qa/qa-bbd98e5-mingw-direct.zip`）。
3. 从 P0-1 重新开跑；本文件状态可整体从 BLOCKED 改写。
