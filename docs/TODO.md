# termv2 剩余工作

更新：2026-11-04，main `22ee722` 之后。
读者：接手开发的人或 agent。先读 [HANDOFF.md](HANDOFF.md)（目录、构建、架构），再读本文决定做什么。逐日进度在 [STATUS.md](STATUS.md)，与 iTerm2 的差距清单在根目录 [REVIEW.md](../REVIEW.md) §8（已回写到 2026-11-04）。

## 0. 先知道这些

- **构建与测试**：`cmake --preset dev && cmake --build --preset dev -j8 && ctest --preset dev -j8`。macOS 和 Linux 上能跑全部可移植测试（113 项）；`asan`、`tsan` 预设同理。见根 [README](../README.md)。
- **Windows 代码在本地编不了**（`ui/shell_win32/main.cpp`、`app/pane_backend/win32_pane_backend.cpp`、`platform/windows/{conpty,ssh,ssh_direct,hyperv}`）。开发机没有 MinGW。靠 GitHub Actions 的 `mingw` 任务做编译检查：每次推送到 main 自动跑。
- **Direct SSH 代码**只在 `IT2W_SSH_DIRECT=ON` 时编译。CI 的 `mingw-direct` 任务**只能手动触发**（Actions → CI → Run workflow），改了 `platform/windows/ssh_direct/` 就要手动跑一次。
- **写法约定**：平台无关的决策逻辑放在 `ui/src`（uicfg）、`app/pane_backend/pane_logic.*` 或 `platform/windows/common/`，并加单测；Win32 文件只做薄胶水。设计先写进 `docs/DS-*.md`，再写代码。提交信息用 `type(scope): 中文说明`，正文写清行为、测试结果和「未验证」项。改完回写 STATUS 顶部。
- **不要做的事**：在首次信任界面之外，Direct 改默认还差「按连接选择传输」（§2.2），在那之前不要把 `IT2W_SSH_DIRECT` 或运行时默认改成 Direct。v0.1 的 A/B/C 没收口之前，F-0..F-5（ROADMAP）只写设计。

## 1. 立即要做（不写代码）

1. **手动触发一次 CI 的 `mingw-direct`**，验证 `22ee722`（DS-12）的 Direct 代码能编译链接。
2. **Windows 实机手测**。代码都在 main 上，只缺实机记录。结果写进 STATUS，并绑定提交号和 Windows / conhost 版本（REVIEW C6）：
   - `986cdf7` 的清单：STATUS 2026-11-04「A5、A1 余量、DS-2、B1–B5」一节末尾（PowerShell 本地连接、Hyper-V 各分支、恢复不自动连接、多行粘贴确认、启动清理快照）。
   - `22ee722` 的清单：[DS-12-Direct首次信任.md](DS-12-Direct首次信任.md) §6。
   - 更早的：HANDOFF §4.6（DebugView 里的 ODS、`vim -u NONE` 的 `v:termresponse`、波浪下划线、洪峰关闭）以及 REVIEW §8 C1–C6。
3. **QA 证据入仓**（增补二 D-5 余项）：CAP-01 第 16–18 轮、S6 手测的结果表（不含原始 hex）。原始文件在旧 Linux 机器的 `/workspace/qa/`。
4. **CORE-01/02 向量**（E4 余项）：`core01_02.json` 也在那台机器的 `/workspace/qa/vectors/`。拷进仓库后可在 CI 打开 `-DIT2W_REQUIRE_VECTORS=ON`。

## 2. 下一步开发（按推荐顺序）

### 2.1 D4 + DS-9：回滚与最小外观 Profile（推荐先做）

- 现状：后端把核心默认的 10000 行 / 64 MiB 覆盖成 5000 行 / 32 MiB（`app/pane_backend/win32_pane_backend.h` 的 `PaneBackendConfig`），用户改不了。`PaneBackendConfig.colors` 从未在 `makeFactory` 里设置；`appearanceProfileId` 只存不用。
- 要做：先写 DS-9 设计（settings / connection 里配色、回滚行数、内存上限的 schema；与核心默认值统一；`.itermcolors` 导入映射），再实现。逻辑大多可在 uicfg 单测。
- 出处：REVIEW GAP-09、§8 D3/D4，增补 10-31 DS-9，ROADMAP F-1。

### 2.2 按连接选择传输（Direct 转默认前的最后门槛）

- 现状：Direct 由全进程环境变量 `ITERM2WIN_SSH_DIRECT=1` 选择。
- 要做：在 DS-2 的启动描述里加「传输：兼容 / 直连」字段（connections.json schema、校验、`describeLaunch`、后端选择），环境变量降级为全局默认或调试开关。做完再单独决定是否改默认。
- 出处：增补二 D-5，ADR-003 §1，HANDOFF §4.5。

### 2.3 D1 连接编辑器

- 新建、编辑、删除、分组。几乎全是 Win32 界面；校验已有（`uicfg::validate` / `validateInFile`），保存路径已有（`saveConnections`）。
- 出处：REVIEW GAP-07、§8 D1。

### 2.4 小项

- **D6**：Ctrl+C 默认 `copyOrInterrupt`（`ui/src/settings.cpp`）要么写成设计决策（ADR），要么改回设计默认。
- **D8**：诊断包导出与脱敏（日志里有主机名、指纹、agent 管道路径）。
- **D5**：快捷键展示。
- **NEW-04 / DS-10**：OSC 52 读的逐次确认界面。做完之前 ConPTY 与 Direct 都保持 `supportsOsc52Read=false`。
- **`droppedOutputBytes`**：丢弃字节目前只在 errorText 里，UI 头文件没有这个字段（NEW-05 余项，ADR-001 §9.5）。

### 2.5 交付与资源

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
