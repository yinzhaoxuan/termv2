# Direct SSH S6 手测结果表（入仓用）

对照 STATUS「2026-11-02：Direct SSH S6 产品手测收口」。证据原件在本机 QA 目录，原始日志不入仓。

## ssh_direct_accept.exe

此前 gavin：预期 PASS 项通过；dns-cancel / server-only-ecdsa 等条件用例 SKIP。

## it2w_shell Direct（基线 fde8e99）

| 轮次 | 环境 | 项 | 结果 |
|---|---|---|---|
| round1 | lab 关机 | pane-diag 含 ssh.direct.* | PASS |
| round1 | lab 关机 | 无 ssh.exe 子进程 | PASS |
| round1 | lab 关机 | 连接 | Timeout（环境） |
| round1 | lab 关机 | exit / resize / vim | SKIP |
| round2 | UbuntuDev 已开 | resize（tput/stty + window-change） | PASS |
| round2 | UbuntuDev 已开 | vim 重绘 | PASS |
| round2 | UbuntuDev 已开 | exit（协议 close，退出码 0） | PASS |
| round2 | UbuntuDev 已开 | 清理 | CLEAN_OK |

结论：Direct SSH v0 产品手测（含诊断落盘）收口；此表入仓后即可从 HANDOFF/TODO 划掉「S6 证据未入仓」。
