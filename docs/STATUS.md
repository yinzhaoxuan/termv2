# iterm2-win 进度汇总

更新时间：2026-10-23（Asia/Shanghai）

本文件跟踪从核心/传输到最小 Win32 窗的提交进度，以及**尚未在真实 Windows 机器上验证**的项。

## 最近提交（摘要）

| 提交 | 内容 |
|------|------|
| `9d7f716` | T05 屏幕模型 Grid/Cell/Scrollback；CSI 默认参数与 SGR；快照 ABI；CORE-03/04/05、MEM-01 子集 |
| `aab3c7e` | T06+T05 评审：`encode_*`、D2D/DWrite 原型、宽字孤儿与 `wrap_pending` 组合、快照 `generation`、ConPTY `failStart` 清 reader |
| `c8e9264` | T06 集成：`app/win32_term/` 最小 WIN32 实验窗串联 itcore + ConPTY + `itrender_d2d`；`docs/STATUS.md` |

## 模块状态

### 已落地（可在 Linux 单测 / mingw 交叉编译门闸）

- **itcore（ports/core）**
  - VT 解析、屏幕模型、SGR/网格、scrollback 预算
  - `itcore_encode_text/paste/key/mouse`（DECCKM、应用键区、1000/1002/1003+1006、bracketed paste ≤32KiB）
  - 快照 ABI：`revision` / `generation` / `mouse_mode` / grapheme
  - 宽字孤儿修复、`wrap_pending` 组合回写末非 CONTINUATION
- **平台层（T03A）**
  - `it2w_platform_common`：SSH 快照过滤、命令行/环境块、输入调度与输出水位（Linux 单测）
  - `ConPtyTransport`：start / enqueue / tryPop / resize(generation) / close；`failStart` 等待后 `CloseHandle(reader_)`
  - `t03a_probe`：最小读写驱动（仅 WIN32）
- **渲染（T06 原型）**
  - `GridMetrics`（DPI 取整、最小 20×5）
  - `ISwapChainHost` + `D2DRenderer`（脏行、caret_rect、预编辑覆盖、`resize_surface`）
  - 静态库不链 itcore；测试用 mock，实验窗链真实 itcore
- **app/win32_term（本提交）**
  - 单 HWND：键盘/`WM_CHAR` + IMM32 IME 提交 → encode → `enqueueUserInput`
  - 输出 `tryPopOutput` → `itcore_feed` → 定时/事件驱动快照 → `present_frame`
  - resize：`GridMetrics::grid_size` → **先** `itcore_resize(gen)` **再** `transport.resize(gen)` → `resize_surface`
  - 关闭：`close(User)` + `waitClosed` → 排空 → `finish_input` / `destroy`
  - CMake：`if(WIN32)`；Linux 配置跳过；尽量 mingw 交叉编译

### 有意未动

- **`ui/`**：本轮不覆盖、不改布局与既有代码（连接/工作区等 UI 路径保持原状）。

## 本轮（渲染评审 + T07）

- **渲染评审 5 点**（`renderer/d2d`）：颜色与脏行复用渲染的 `renderer/layout/cell_style.h`（`resolve_cell` / `DirtyRows`），持久画布只重画脏行、覆盖层（光标/预编辑）每帧画在后缓冲；粗体/斜体（4 个 TextFormat 变体）+ 下划线（`underline_color`）；设备丢失 → `recover_device()`；generation 不一致 → `frame_policy.h::LayoutKeeper` 保留旧布局、只画交集、其余填背景。单测 `renderer_frame_policy`。
- **SSH 快照 A4 修复**：Interactive 增加 `IdentityFile none` / `IdentitiesOnly yes` / `IdentityAgent none`；三种模式都输出 `CertificateFile none` / `ControlMaster no` / `ControlPath none`。
- **T07**：`platform/windows/common/ssh_session.*`（`ssh -G` 解析与生效值校验、255/远端退出码与错误映射、输出尾部去转义）；`platform/windows/ssh/ssh_transport.*`（落盘快照 → ConPTY 中 `ssh.exe -F <快照> <alias>` → `onFinished(SshExitInfo)`）。测试：`T07.ssh_map.unit`、`SSH-CFG-01.effective_G`、`T07.ssh_live`（本机 sshd 127.0.0.1:22422：未知主机 / 主机密钥变化 / 拒绝连接 / 错误密钥 / 交互对仅公钥服务器）。授权密钥、密码（22423）、带口令密钥用例在 `/home/it2wcli/lab` 对 box 用户不可读时自动 SKIP。
- **mingw 全量构建**：交叉编译时 uicfg 使用 `third_party/nlohmann_json/include`（独立路径，不再引入 `/usr/include`），工具链加 `CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY`；`cmake --build build-mingw` 全目标通过。
- 已知限制：ssh 退出瞬间仍在输出队列中、未被 UI 取走的错误文本不计入尾部，可能映射为 Unknown；后续改为 Transport 内部旁路复制。

## 未实机项（须 Win11 + 真 ConPTY / D3D 验证）

以下在 Linux 上只能做到**编译门闸**或纯逻辑单测，行为以规格为准，**不能**用交叉编译结果代替实机：

1. **ConPTY 全路径**：`CreatePseudoConsole` / Job 归属 / 管道读写 / `ResizePseudoConsole` / 关闭时序与超时 fault（见 `docs/T03A-Windows规格-v0.1.md`）。
2. **win32_term 端到端**：cmd.exe 启停、键盘与中文 IME（IMM32 提交吞 `WM_CHAR`）、帧延迟与脏行 Present、Per-Monitor V2 DPI 变化后行列收敛。
3. **D3D/D2D 设备丢失与重建**：`present_frame` 遇 `DEVICE_REMOVED/RESET/RECREATE_TARGET` 置位，下一帧 `recover_device()` 整链重建并经 `host->create_swap_chain` 新建交换链（已实现，未实机触发验证）。
7. **渲染评审 5 点的视觉效果**：画布脏行重绘、粗体/斜体/下划线、generation 不一致保留旧布局——逻辑已单测，像素结果须实机截图。
8. **T07 SshTransport**：Windows 自带 OpenSSH 在 ConPTY 中的启动、主机密钥/密码/口令提示交互、快照路径（盘符/UNC/中文）与退出码映射须实机；Linux 侧仅以 OpenSSH 10.0 验证映射与 `-G` 生效值。
4. **鼠标报告**：encode_mouse 已单测；窗体命中 → 模式开启时上报尚未接到 win32_term。
5. **粘贴事务**：`beginPaste` / bracketed / 32KiB 上限在传输与 encode 侧有测；窗体 Ctrl+V / 剪贴板未接。
6. **WinUI / 正式 UI 壳**：当前仅为 HWND 实验窗，非正式产品窗。

## 构建提示

```bash
# Linux 本机（跳过 WIN32 目标，跑 itcore / 布局单测）
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# mingw-w64 交叉（产出 t03a_probe、win32_term、itrender_d2d 等）
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-mingw -j   # 全量通过
```

实机：将 `win32_term.exe` 与依赖（若非 `-static`）拷到 Win11，直接运行；默认启动 `C:\Windows\System32\cmd.exe`。
