# UI 外壳与渲染宿主对齐（对照 `renderer/d2d/iswap_chain_host.h`）

- 渲染器构造时注入 `ISwapChainHost*`，不绑定具体窗口。
- 选型未定前：Win32 HWND 宿主实现 `CreateSwapChainForHwnd`。
- 若选 WinUI 3：同一接口改为 `CreateSwapChainForComposition` + SwapChainPanel。
- IME：UI 管焦点与组合状态；渲染提供 `caret_rect_px()` 定位候选窗，并画预编辑文字。
- 配置库 `uicfg` 与外壳无关：connections / workspace / settings 三文件均可独立使用。

## 退出顺序（c0bd7c7 起）
1. 关闭确认汇总通过后，销毁所有窗格的 PaneBackend。
2. 调用 `it2app::shutdownPaneBackends(3000)`，等待关闭线程；超时由 KILL_ON_JOB_CLOSE 收尾。
3. 再保存 workspace.json、释放 ConfigDirLock、退出消息循环。

## 历史滚动快捷键
设置里的 scroll* 动作经 `uicfg::scrollCommandFor()` 映射到 `scrollViewport(lines)` / `scrollToBottom()`。
