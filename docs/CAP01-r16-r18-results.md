# CAP-01 第 16–18 轮结果表（入仓用，不含原始 hex）

来源：本机 `/workspace/qa/realmachine-results-r16|r17|r18/` 与 `验收记录-真机.md`。
用途：回写 termv2 `docs/`（TODO §1.3）。原始 hex / bin 不入仓。

## 第 16 轮（ConPTY 序列探针，gavin，2026-10-31）

Harness 与产品对齐：`CreatePseudoConsole flags=0`，80×24，子进程 `cmd.exe`。

| Probe | 输入摘要 | 判定 |
|---|---|---|
| C-write | OSC 52 写 | 完整出现（与 R15 一致） |
| C-read | OSC 52 读查询 | 完全消失（CAP-01 P06） |
| S1 | OSC 1337 SetMark | 完整出现 |
| S2 | OSC 133;A | 完整出现 |
| S3 | OSC 8 超链接 | 被改写（加 id=，BEL→ST） |
| S4 / S4' | DCS（有/无 ST） | 完全消失 |
| S5 | Sixel | 完全消失 |

结论：经 ConPTY 的会话不支持 OSC 52 读；tmux -CC DCS / Sixel 被 conhost 吞掉。直连 SSH 须重评。

## 第 17 轮（OSC 133;A 顺序，gavin，2026-10-31）

同 R16 harness；`PROMPT` 带 `]133;A`；5 回车 + `dir`；未启动 it2w_shell。

| # | 触发 | 判定 |
|---|---|---|
| 1–7 | startup / Enter×5 / after dir | 全部同位（标记紧挨提示符，无重排） |

开放风险：未测 `cls` / resize / 滚出底边整屏重绘时是否提前。

## 第 18 轮（纯管道 ssh.exe，DESKTOP-HOODQAJ → UbuntuDev，2026-10-31）

| 项 | 结果 | 说明 |
|---|---|---|
| R18-1 DCS/Sixel/OSC52 | PASS（字节级一致） | 相对 R16 ConPTY 被吞形成对照 |
| R18-2 stty size / resize | FAIL（0×0） | 纯管道无法窗口变更；需 scheme A（进程内 libssh） |
| R18-3 交互 host-key/密码 | 需控制台 TTY | 管道上提示不出现 |

## CAP-01 P06 定案（Gavin，2026-10-31）

经 ConPTY 的会话（本地 / 兼容 SSH / Hyper-V）一律不支持 OSC 52 读；写正常并由策略控制。能力门见 ADR-002。Direct 上的读走 DS-10 逐次确认。
