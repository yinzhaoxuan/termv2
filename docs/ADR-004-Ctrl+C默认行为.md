# ADR-004：Ctrl+C 默认「有选区复制，否则中断」

- **状态**：Accepted（2026-11-04，用户选择保留现状）
- **出处**：`REVIEW.md` GAP-07 末段、§8 D6；§9 第 2 条「登记明确的偏离」；HANDOFF §4（D6）。
- **范围**：`settings.json` 的默认快捷键表（`ui/src/settings.cpp` 的 `kDefaults`）与 `runAction` 里的 `copyOrInterrupt`（`ui/shell_win32/main.cpp`）。

## 背景

原设计：Ctrl+C 永远把 0x03 发给终端（中断），复制只用 Ctrl+Shift+C。

实现：默认绑定 `"copyOrInterrupt": "Ctrl+C"`。

- 当前窗格**有选区**：复制选中文本并清除选区，按键不发给终端。
- **没有选区**：不处理，按键照常交给后端编码，发出 0x03。

`"copy": "Ctrl+Shift+C"` 仍然存在，任何时候都只复制、不中断。

## 决策

接受这个偏离，保留 `copyOrInterrupt` 为 Ctrl+C 的默认。

理由：

1. Windows Terminal、VS Code 终端、新版 conhost 默认都是这样。Windows 用户的肌肉记忆是「选中 → Ctrl+C」，原设计的默认会让复制看起来「没反应」，还会意外中断前台程序。
2. 选区是用户刚刚主动做出的，按 Ctrl+C 时想复制的概率远高于想中断。
3. 没有选区时行为与原设计完全一致，`Ctrl+Shift+C` 也保留，原设计的用户习惯不受影响。

## 代价与缓解

- **选区残留时按 Ctrl+C 不会中断。** 例如选中一段输出后忘了取消，再想用 Ctrl+C 停掉一个刷屏的程序，第一次按下只会复制并清掉选区，第二次才中断。缓解：复制后**立即清除选区**（已实现），所以最多多按一次；状态栏不额外提示。
- **全屏程序**（vim、less 等开启鼠标上报时）：普通拖动交给程序（`handleMouse` 里 `mode && !MK_SHIFT` 分支），终端本地不产生选区，Ctrl+C 照常发给程序。只有按住 Shift 拖选时才会有本地选区，此时 Ctrl+C 复制。

## 退路

在 `settings.json` 里把绑定设为空字符串，Ctrl+C 就恢复为原设计（永远中断）：

```json
"keybindings": { "copyOrInterrupt": "" }
```

空字符串表示「不绑定」，运行时 `matchKey` 跳过它，按键原样交给终端。`pasteCtrlV` 设为 `""`（让 vim 收到 Ctrl+V 做块选择）用的是同一机制。

本 ADR 同时修正一个与此相关的缺陷：`validateSettings` 原来把空快捷键当错误，而 `saveSettings` 保存前会校验，导致按上面方法退回的用户之后改字号、导入配色时 settings.json 保存失败。修正后空字符串是合法的「不绑定」，也不参与快捷键冲突检查。

## 不做

- 不提供界面开关（快捷键编辑在 v0.2+，见 REVIEW §8 D5）。
- 不在有选区时给 Ctrl+C 加「第二次按才复制」之类的变体。
