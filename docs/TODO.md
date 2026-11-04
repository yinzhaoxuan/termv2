# termv2 剩余工作

更新：2026-11-04，ADR-004（D6）之后。
读者：接手开发的人或 agent。先读 [HANDOFF.md](HANDOFF.md)（目录、构建、架构），再读本文决定做什么。逐日进度在 [STATUS.md](STATUS.md)，与 iTerm2 的差距清单在根目录 [REVIEW.md](../REVIEW.md) §8（已回写到 2026-11-04）。

## 0. 先知道这些

- **构建与测试**：`cmake --preset dev && cmake --build --preset dev -j8 && ctest --preset dev -j8`。macOS 和 Linux 上能跑全部可移植测试（132 项）；`asan`、`tsan` 预设同理。见根 [README](../README.md)。
- **Windows 代码在本地编不了**（`ui/shell_win32/main.cpp`、`app/pane_backend/win32_pane_backend.cpp`、`platform/windows/{conpty,ssh,ssh_direct,hyperv}`）。开发机没有 MinGW。靠 GitHub Actions 的 `mingw` 任务做编译检查：每次推送到 main 自动跑。
- **Direct SSH 代码**只在 `IT2W_SSH_DIRECT=ON` 时编译。CI 的 `mingw-direct` 任务**只能手动触发**（Actions → CI → Run workflow），改了 `platform/windows/ssh_direct/` 就要手动跑一次。
- **写法约定**：平台无关的决策逻辑放在 `ui/src`（uicfg）、`app/pane_backend/pane_logic.*` 或 `platform/windows/common/`，并加单测；Win32 文件只做薄胶水。设计先写进 `docs/DS-*.md`，再写代码。提交信息用 `type(scope): 中文说明`，正文写清行为、测试结果和「未验证」项。改完回写 STATUS 顶部。
- **不要做的事**：Direct 改默认要先按 §2.1 写 ADR 并有实机记录，在那之前不要把 `IT2W_SSH_DIRECT` 或运行时默认改成 Direct。v0.1 的 A/B/C 没收口之前，F-0..F-5（ROADMAP）只写设计。

## 1. 立即要做（不写代码）

1. **手动触发一次 CI 的 `mingw-direct`**，验证 DS-12、DS-13 的 Direct 构建能编译链接。
2. **Windows 实机手测**。代码都在 main 上，只缺实机记录。结果写进 STATUS，并绑定提交号和 Windows / conhost 版本（REVIEW C6）：
   - `986cdf7` 的清单：STATUS 2026-11-04「A5、A1 余量、DS-2、B1–B5」一节末尾（PowerShell 本地连接、Hyper-V 各分支、恢复不自动连接、多行粘贴确认、启动清理快照）。
   - DS-14 的清单：[DS-14-连接编辑器.md](DS-14-连接编辑器.md) §7（含 GAP-07 验收路径）。
   - DS-13 的清单：[DS-13-按连接选择传输.md](DS-13-按连接选择传输.md) §6。
   - DS-9 的清单：[DS-9-外观Profile.md](DS-9-外观Profile.md) §7。
   - `22ee722` 的清单：[DS-12-Direct首次信任.md](DS-12-Direct首次信任.md) §6。
   - 更早的：HANDOFF §4.6（DebugView 里的 ODS、`vim -u NONE` 的 `v:termresponse`、波浪下划线、洪峰关闭）以及 REVIEW §8 C1–C6。
3. **QA 证据入仓**（增补二 D-5 余项）：CAP-01 第 16–18 轮、S6 手测的结果表（不含原始 hex）。原始文件在旧 Linux 机器的 `/workspace/qa/`。
4. **CORE-01/02 向量**（E4 余项）：`core01_02.json` 也在那台机器的 `/workspace/qa/vectors/`。拷进仓库后可在 CI 打开 `-DIT2W_REQUIRE_VECTORS=ON`。

## 2. 下一步开发（按推荐顺序）

### 2.1 决定是否把 Direct 改成默认传输

- 前置条件都已在 main：首次信任界面（DS-12）、按连接选择传输（DS-13）。缺的是 Windows 实机记录（DS-12 §6、DS-13 §6、HANDOFF §4.6）和 `mingw-direct` 编译。
- 要决定：发布包是否 `IT2W_SSH_DIRECT=ON`；没写 `ssh.transport` 的连接默认走哪种；全局默认放在 settings.json 还是继续用环境变量（ADR-003 §决策 1 需要改写）。
- 不写代码，先写 ADR。

### 2.2 小项

- **D8**：诊断包导出与脱敏（日志里有主机名、指纹、agent 管道路径）。
- **D5**：快捷键展示。
- **NEW-04 / DS-10**：OSC 52 读的逐次确认界面。做完之前 ConPTY 与 Direct 都保持 `supportsOsc52Read=false`。
- **`droppedOutputBytes`**：丢弃字节目前只在 errorText 里，UI 头文件没有这个字段（NEW-05 余项，ADR-001 §9.5）。

### 2.3 交付与资源

- **E7**：STATUS 后半段与 `ports/core/SOURCE_MAP.md` 混着 10 月的陈旧条目，整理成单一现状，历史单独存放。
- **E6**：根 LICENSE、SPDX、上游文件哈希清单、依赖审计（GAP-12）。
- **E1**：会话执行器，或基于实测把 UI 线程模型写成 ADR（ADR-001 备选 A2）。
- **E3**：会话与应用两级内存账本。
- **E8**：GlyphCache 图集与 glyph run（连字的前提）。
- **DS-7 余项**：TERM、terminfo 与 DA 身份三者对齐，需要实机（GAP-10）。
- **DS-8 余项**：定下 `ITCORE_ABI_VERSION` 的递增规则（现在仍是 0）。

## 3. v0.2 以后（只写设计）

[ROADMAP-iTerm2-parity.md](ROADMAP-iTerm2-parity.md) 的 F-0..F-5 与 REVIEW §8 F1–F13：Shell Integration、通知、Kitty 键盘、内联图片与 Sixel、tmux -CC、完整 Profile、触发器、广播输入、WSL 等。依赖 DCS / Sixel 的功能只能放在 Direct 上，本地 ConPTY 会吞掉这些序列（ADR-002）。

## 4. 本轮已完成（2026-11-04，供核对）

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
