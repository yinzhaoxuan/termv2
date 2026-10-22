# T06 渲染与输入方案 v0.1（渲染 → 架构师实现）

基于 `ports/core/include/itcore.h` v0、设计文档 v0.3 第 14、15、17、18 节，宿主按 UI 倾向的 WinUI 3 SwapChainPanel（原生 HWND 子窗口同样适用）。

## 1. 已交付代码（可在 Linux/Windows 编译，已过 CTest）
- `renderer/mock/itcore_mock.*`：实现 itcore.h 全部快照访问器，`itmock_snapshot_from_utf8` 由文本生成快照（宽字符尾格、软换行/wide-wrap、光标）。T05 完成前渲染原型和测试用它驱动，不与真实 itcore 同时链接。
- `renderer/layout/grid_metrics.h`：格坐标与像素换算的唯一实现，渲染、鼠标命中、IME 候选窗、resize 都用它；按物理像素取整，最小 20×5。
- `tests/renderer/test_mock_layout.cpp`：宽字符、贴边换行、100/150/200% DPI 命中往返。

## 1.1 宿主接口（与 UI 对齐）
渲染器不绑定窗口。外壳实现 `ISwapChainHost`：`create_swap_chain(IDXGIFactory2*, ID3D11Device*, const DXGI_SWAP_CHAIN_DESC1&) -> IDXGISwapChain1*`、`client_size_px()`、`dpi_scale()`。原型用 `CreateSwapChainForHwnd`，WinUI 3 用 `CreateSwapChainForComposition` 并挂到 SwapChainPanel，渲染核心不改。设备丢失重建时再次调用 create_swap_chain。
IME：UI 负责焦点和组合状态，渲染提供 `caret_rect_px()`（基于 GridMetrics 与快照光标），并绘制预编辑覆盖层。

## 2. 线程与快照
- 渲染线程只读快照；每 Pane 一个在读、一个最新待读（15.4），新快照替换未读的待读快照，读完必须 `itcore_snapshot_release`。
- 收到 `RENDER_INVALIDATED` 后由会话执行器 acquire，投递给渲染线程；渲染不调用任何修改 core 的接口，也不等 ConPTY。
- resize：UI 线程用 `GridMetrics::grid_size` 算行列 → 投递执行器（带 generation，未执行的可合并，但不跨已排队输入/鼠标事件）→ 渲染在收到对应 generation 快照前保持旧布局，多出区域填背景色。

## 3. 绘制管线
1. D3D11 设备（BGRA 支持）→ DXGI flip-model 交换链（`DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`，帧延迟对象控制 1 帧）→ D2D1 DeviceContext。
2. 每帧按行比较 row_revision，只重绘脏行；整帧 revision 不变且光标未闪烁则不 Present。
3. 每行三遍：背景矩形（相同 bg 合并成段）→ 文本 → 下划线/光标/选择。
4. 文本按样式分段，每段用 `IDWriteTextAnalyzer`/`IDWriteFontFallback` 做字形映射，但每个字素簇强制放在 `cell_rect(col,row,width)` 起点；字形 advance 不参与定位，因此回退字体不改变列数（14.2）。宽于格的字形居中裁剪，不挤占下一格。
5. 字形缓存：键为（字素 UTF-8、字体 face、样式、DPI），值为 D2D 位图图集中的位置；DPI 或字号变化时整体失效。上限计入渲染独立显存统计，不计入核心账本。
6. 颜色：`0xTTRRGGBB`，TT=0 用主题默认，1 查 256 色调色板，2 直接真彩；INVERSE 交换前后景，粗体不自动变亮（可设置）。
7. 设备丢失（`DXGI_ERROR_DEVICE_REMOVED/RESET`）：丢弃所有设备资源、重建交换链和图集，用当前快照重画，报告 `RenderDeviceLost`，不碰终端状态（17）。

## 4. DPI（DPI-01）
- 进程 Per-Monitor V2；`WM_DPICHANGED`（WinUI 用 XamlRoot.RasterizationScale 变化）时：重算 GridMetrics → 清字形缓存 → 按新行列走 resize 流程。
- 字号以 pt 存配置，换算 DIP 后再乘缩放，保证跨屏移动视觉大小一致。

## 5. IME 中文输入（WIN-01）
- 首选 TSF（WinUI 下由 CoreTextEditContext 封装 TSF）；HWND 宿主退回 IMM32（`WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION`）。
- 预编辑文本只画在本地光标位置的覆盖层（带下划线），不写入快照、不发送远端。
- 只在提交（Result string / TextUpdated 确认）时调用 `itcore_encode_text`，同时吞掉同一次提交产生的 `WM_CHAR`，防止重复发送。
- 候选窗位置：`cell_rect(cursor)` 转屏幕坐标，经 `NotifyLayoutChanged`（TSF）或 `ImmSetCandidateWindow`（IMM32）上报；光标移动、滚动、DPI 变化时重报。与 UI 一起做验证实验。
- 预编辑期间方向键、回车、Esc 交给输入法，不编码成终端按键。

## 6. 键盘与鼠标（需补 ABI）
- `itcore.h` v0 缺 `itcore_encode_key` 和 `itcore_encode_mouse`（14.1 草案有）。建议结构：
  - key_event：`struct_size, vk, scan, modifiers(shift/ctrl/alt/win 位), is_repeat, unicode(可为 0)`，由核心按普通/应用光标、应用数字键盘模式选 CSI/SS3。
  - mouse_event：`struct_size, col, row, button, action(press/release/move/wheel), wheel_delta, modifiers`。
  - 另需快照查询 `itcore_snapshot_mouse_mode()`，渲染据此决定鼠标给远端还是本地选择。
- Ctrl+Shift+C/V 等应用快捷键在编码前拦截（与 UI 的快捷键实验对齐）。

## 7. 选择与复制
- 选择锚点存（行身份、revision、列），不存像素或数组下标；行被淘汰或 revision 变化时按 14.2 判定失效。
- 点到尾格时归并到首格，宽字符整体选中；跨软换行拼接不加换行，硬换行输出 `\r\n`。
- 复制写 Win32 剪贴板（`CF_UNICODETEXT`），与 OSC 52 无关；粘贴走 `itcore_encode_paste`，超过 32 KiB 提示缩小。

## 8. 验收映射与性能
| 用例 | 渲染侧负责点 |
|---|---|
| DPI-01 | GridMetrics 取整、跨屏重算、命中不漂移（已有单测） |
| WIN-01 | 预编辑不外发、提交去重、候选窗定位 |
| IO-01 | 渲染不阻塞核心，只消费最新快照 |
| CORE-03 | 宽字符与贴边换行显示正确 |
| 18.2 | 输入到帧提交 P95 ≤50 ms：脏行重绘 + 1 帧延迟交换链；resize 停止后 200 ms 收敛 |

## 9. 待架构师确认
1. 补 `itcore_encode_key`、`itcore_encode_mouse`、鼠标模式和选择所需的行身份查询（目前只有 row_revision）。
2. 快照是否携带光标形状/闪烁和主备用屏标记。
3. 渲染线程与执行器之间的快照投递队列放在 app 层还是 renderer 层。
