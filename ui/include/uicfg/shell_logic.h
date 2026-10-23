#pragma once
// T08 Win32 外壳的平台无关逻辑（Linux 单测）：快捷键解析与匹配、剪贴板粘贴换行、
// 链接确认文案、会话状态文案、标签栏 ↔ workspace 转换。
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/layout_ops.h"
#include "uicfg/link_policy.h"
#include "uicfg/tab_model.h"
#include "uishell/pane_backend.h"

namespace uicfg {

// 规范化按键：key 为 "A".."Z" "0".."9" "F1".."F24" "Tab" "PageUp" "PageDown" "Home" "End"
// "Up" "Down" "Left" "Right" "Plus" "Minus" "Enter" "Escape" "Insert" "Delete" "Space"，或 "Arrow"（任意方向，仅用于绑定）
struct Chord {
  bool ctrl = false, shift = false, alt = false;
  std::string key;
  bool operator==(const Chord&) const = default;
};
std::optional<Chord> parseChord(const std::string& text);  // "Ctrl+Shift+T"；大小写不敏感

struct KeyMatch {
  std::string action;                 // 空 = 未绑定，交给终端
  std::optional<Direction> direction; // focusPane（Alt+Arrow）时给出方向
};
// 按下的按键（key 不会是 "Arrow"）→ 动作
KeyMatch matchKey(const std::map<std::string, std::string>& bindings, const Chord& pressed);

// 剪贴板文本 → paste()：CRLF 与单独 LF 都转 CR（终端回车语义），NUL 截断后部
std::string normalizePasteText(const std::string& clipboardUtf8);

// 命令行：it2w_shell.exe --command "<命令行>"（也接受 --command=<命令行>）。
// 返回首个本地窗格要执行的命令；未给出返回 nullopt；--command 后缺值返回空串（调用方报错）。
std::optional<std::wstring> parseCommandArg(const std::vector<std::wstring>& argv);

// 点击链接：Allow 时给出确认文案（展示真实 URL），否则给出拒绝文案
struct LinkPrompt { bool canOpen; std::string message; std::string url; };
LinkPrompt linkPrompt(const std::string& url);

// 窗格覆盖层 / 状态栏文案；Failed 时 showReconnect=true
struct PaneOverlay { bool visible; bool showReconnect; std::string text; };
PaneOverlay overlayFor(const uishell::SessionStatus& st);
std::string stateLabel(uishell::SessionState s);

// 非字符按键 → VT 序列（xterm 风格，修饰键用 CSI 1;m）。可打印字符、Enter/Tab/Backspace/Esc
// 走 WM_CHAR，不在这里（Shift+Tab 例外，返回 CSI Z）。不认识的键返回空串。
// 说明：DECCKM（应用光标键）状态 UI 拿不到，统一发普通模式 CSI A..D。
std::string vtForKey(const std::string& key, bool ctrl, bool shift, bool alt);
// WM_CHAR 字符（已是 UTF-8）→ 发给 sendInput 的字节：Backspace 0x08 → 0x7f；alt 时前置 ESC
std::string vtForChar(const std::string& utf8, bool alt);

std::string pasteRejectedMessage(size_t bytes, size_t limit);

}  // namespace uicfg
