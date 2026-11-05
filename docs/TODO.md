# termv2 剩余工作

更新：2026-11-05，手测清单与文档回写之后。
读者：接手开发的人或 agent。先读 [HANDOFF.md](HANDOFF.md)（目录、构建、架构），再读本文决定做什么。现状与逐日进度在 [STATUS.md](STATUS.md)，与 iTerm2 的差距清单在根目录 [REVIEW.md](../REVIEW.md) §8（已回写到 2026-11-04）。

## 0. 先知道这些

- **构建与测试**：`cmake --preset dev && cmake --build --preset dev -j8 && ctest --preset dev -j8`。macOS 和 Linux 上能跑全部可移植测试（172 项）；`asan`、`tsan` 预设同理。见根 [README](../README.md)。
- **Windows 代码在本地编不了**（`ui/shell_win32/main.cpp`、`app/pane_backend/win32_pane_backend.cpp`、`platform/windows/{conpty,ssh,ssh_direct,hyperv}`）。开发机没有 MinGW。靠 GitHub Actions 的 `mingw` 任务做编译检查：每次推送到 main 自动跑。
- **Direct SSH 代码**只在 `IT2W_SSH_DIRECT=ON` 时编译。CI 的 `mingw-direct` 任务自 ADR-006 起每次推送都跑（OpenSSL / libssh2 有缓存），产物也是发布包用的构建。
- **写法约定**：平台无关的决策逻辑放在 `ui/src`（uicfg）、`app/pane_backend/pane_logic.*` 或 `platform/windows/common/`，并加单测；Win32 文件只做薄胶水。设计先写进 `docs/DS-*.md`，再写代码。提交信息用 `type(scope): 中文说明`，正文写清行为、测试结果和「未验证」项。改完回写 STATUS 顶部。
- **不要做的事**：Direct 改默认要先按 §2.1 写 ADR 并有实机记录，在那之前不要把 `IT2W_SSH_DIRECT` 或运行时默认改成 Direct。v0.1 的 A/B/C 没收口之前，F-0..F-5（ROADMAP）只写设计。

## 1. 立即要做（不写代码）

1. **确认 CI 的 `mingw-direct` 通过**（ADR-006 起自动跑）：DS-10、DS-12、DS-13、ADR-006 的 Direct 代码第一次编译。
2. **Windows 实机手测**：按 [WINDOWS-手测清单.md](WINDOWS-手测清单.md) 从上往下做（已把下面各处的手测项合并并排好优先级；CI 的 mingw / mingw-direct 任务会上传 `it2w_shell.exe` 供下载）。代码都在 main 上，只缺实机记录。结果写进 STATUS，并绑定提交号和 Windows / conhost 版本（REVIEW C6）：
   - `986cdf7` 的清单：STATUS 2026-11-04「A5、A1 余量、DS-2、B1–B5」一节末尾（PowerShell 本地连接、Hyper-V 各分支、恢复不自动连接、多行粘贴确认、启动清理快照）。
   - DS-17 的清单：[DS-17-内存账本.md](DS-17-内存账本.md) §6。
   - DS-10 的清单：[DS-10-OSC52读确认.md](DS-10-OSC52读确认.md) §7。
   - DS-16 的清单：[DS-16-快捷键展示.md](DS-16-快捷键展示.md) §4。
   - DS-15 的清单：[DS-15-诊断包导出.md](DS-15-诊断包导出.md) §7。实机手测时可直接用「导出诊断包…」附上提交号与 Windows 版本（REVIEW C6）。
   - DS-14 的清单：[DS-14-连接编辑器.md](DS-14-连接编辑器.md) §7（含 GAP-07 验收路径）。
   - DS-13 的清单：[DS-13-按连接选择传输.md](DS-13-按连接选择传输.md) §6。
   - DS-9 的清单：[DS-9-外观Profile.md](DS-9-外观Profile.md) §7。
   - `22ee722` 的清单：[DS-12-Direct首次信任.md](DS-12-Direct首次信任.md) §6。
   - 2026-11-05 实现审查与复审：[审查清单-2026-11-05.md](审查清单-2026-11-05.md)、[审查清单-2026-11-05-复审.md](审查清单-2026-11-05-复审.md) 验证记录末项（ConPTY 关闭异常路径、启动 / 关闭交接、事件创建失败的故障注入）。
   - 更早的：HANDOFF §4.6（DebugView 里的 ODS、`vim -u NONE` 的 `v:termresponse`、波浪下划线、洪峰关闭）以及 REVIEW §8 C1–C6。
3. **QA 证据入仓**（增补二 D-5 余项）：CAP-01 第 16–18 轮、S6 手测的结果表（不含原始 hex）。原始文件在旧 Linux 机器的 `/workspace/qa/`。
4. **CORE-01/02 向量**（E4 余项）：`core01_02.json` 也在那台机器的 `/workspace/qa/vectors/`。拷进仓库后可在 CI 打开 `-DIT2W_REQUIRE_VECTORS=ON`。

## 2. 下一步开发（按推荐顺序）

### 2.1 Direct 改成默认传输（ADR-006 阶段 2）

- 已决定并完成阶段 1（[ADR-006](ADR-006-SSH默认传输.md)）：`auto` 按认证方式选（agent → 直连）；settings.json `sshDefaultTransport` 与环境变量 `1` / `0`；失败文字说明来源；发布包用 `mingw-direct`；CI 每次推送编 Direct。内置默认仍是兼容。
- 阶段 2 只改 `kBuiltinSshDefault`（`app/pane_backend/pane_logic.h`）为 `Auto`。门槛见 ADR-006 §决策 4：`mingw-direct` 通过；手测清单 P1-3 / P1-4 / P1-5 / P2-4 的 Direct 项和 Direct 上的 P3-2；一个工作日的 `auto` 实际使用；两条退回路径验证过。

### 2.2 交付与资源

- **E6 余项**：MinGW-w64 winpthreads / CRT 许可文本打包进 `licenses/`（[LICENSE-AUDIT](LICENSE-AUDIT.md) §5）；`case_fold_table.inc` 补生成脚本。
- **E1**：会话执行器，或基于实测把 UI 线程模型写成 ADR（ADR-001 备选 A2）。
- **E8**：已按 [DS-18](DS-18-字形运行渲染.md) 实现（按段塑形、按格定位、可选连字、彩色 emoji、塑形缓存、`render.stats`），只差 Windows 手测（手测清单 P2-2a、P3-2a）。新路径确认无回归后，下个版本删掉旧路径（`ITERM2WIN_TEXT_PATH=drawtext`）。
- **DS-7 余项**：TERM、terminfo 与 DA 身份三者对齐，需要实机（GAP-10）。

## 3. v0.2 以后（只写设计）

[ROADMAP-iTerm2-parity.md](ROADMAP-iTerm2-parity.md) 的 F-0..F-5 与 REVIEW §8 F1–F13：Shell Integration、通知、Kitty 键盘、内联图片与 Sixel、tmux -CC、完整 Profile、触发器、广播输入、WSL 等。依赖 DCS / Sixel 的功能只能放在 Direct 上，本地 ConPTY 会吞掉这些序列（ADR-002）。

## 4. 本轮已完成（2026-11-04 至 11-05，供核对）

| 提交 | 内容 | 设计 / 说明 |
|---|---|---|
| `986cdf7` | A5 加载校验、A1 启动清理快照、DS-2 启动描述、B1–B5 | [DS-2-启动描述与B批.md](DS-2-启动描述与B批.md) |
| `299b3c1` | E4 nlohmann 默认仓库内、E5 预设 / README / CI | 根 README |
| `c69bc48` | NEW-02 历史显示行数按组增量计数 | STATUS 2026-11-04 |
| `2fcaac3` | NEW-03 abs_row 约束写明 + 淘汰后高亮回归 | `ports/core/include/itcore.h` |
| `22ee722` | DS-12 Direct 首次信任 | [DS-12-Direct首次信任.md](DS-12-Direct首次信任.md) |
| `6db9448` | DS-9 外观 Profile：配色单一来源、appearanceProfiles、回滚/内存、.itermcolors 导入（D3、D4） | [DS-9-外观Profile.md](DS-9-外观Profile.md) |
| `26a7618` | DS-13 按连接选择 SSH 传输（增补二 D-5） | [DS-13-按连接选择传输.md](DS-13-按连接选择传输.md) |
| `1fdee54` | DS-14 连接编辑器（D1） | [DS-14-连接编辑器.md](DS-14-连接编辑器.md) |
| `0b72f82` | ADR-004 Ctrl+C 默认（D6）+ 空快捷键保存修复 | [ADR-004-Ctrl+C默认行为.md](ADR-004-Ctrl+C默认行为.md) |
| `96f8abd` | DS-15 诊断包导出与脱敏（D8） | [DS-15-诊断包导出.md](DS-15-诊断包导出.md) |
| `65f5d0d` | DS-16 快捷键展示（D5） | [DS-16-快捷键展示.md](DS-16-快捷键展示.md) |
| `1356bb6` | DS-10 OSC 52 读逐次确认（NEW-04） | [DS-10-OSC52读确认.md](DS-10-OSC52读确认.md) |
| `f36ac44` | NEW-05 余项：`SessionStatus.droppedOutputBytes` 与状态栏提示 | ADR-001 §2.8 |
| `2d00cab` | E7：STATUS 拆出「当前状态」，10 月旧快照移到 HISTORY-2026-10；SOURCE_MAP 改正过时说法 | [HISTORY-2026-10.md](HISTORY-2026-10.md) |
| `376408e` | E6：GPL-3.0-or-later、LICENSE、SPDX、上游文件清单、依赖审计、所有构建附带许可 | [LICENSE-AUDIT.md](LICENSE-AUDIT.md) |
| `398f799` | DS-8 余项：ABI 版本规则 + ABI-02 布局测试 | [ADR-005-itcore-ABI版本规则.md](ADR-005-itcore-ABI版本规则.md) |
| `ce2c79d` | DS-17 会话 / 应用两级内存账本（E3） | [DS-17-内存账本.md](DS-17-内存账本.md) |
| `89e5f52` | Windows 实机手测清单；CI 上传 `it2w_shell.exe` | [WINDOWS-手测清单.md](WINDOWS-手测清单.md) |
| `bf0adc1` | 文档回写：HANDOFF、README、REVIEW、ROADMAP | STATUS 2026-11-05 |
| `08e36f2` | 实现审查修复：ConPTY 常驻 closer、写线程 join、Ctrl 符号键 | [审查清单-2026-11-05.md](审查清单-2026-11-05.md) |
| 本轮末 | ADR-006 阶段 1：SSH 默认传输的来源与优先级、`sshDefaultTransport`、CI 每次编 Direct | [ADR-006-SSH默认传输.md](ADR-006-SSH默认传输.md) |
