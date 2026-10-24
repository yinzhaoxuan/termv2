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
// 会话树标签来自连接名 + snaps 运行计数，不用 pane title。
// snaps：启动起算，直到 Exited/Failed 才 erase。仅当新旧状态在「计入运行」集合上不同时才需 refreshTree。
bool statusAffectsSessionTree(uishell::SessionState prev, uishell::SessionState next);

// 非字符按键 → VT 序列（xterm 风格，修饰键用 CSI 1;m）。可打印字符、Enter/Tab/Backspace/Esc
// 走 WM_CHAR，不在这里（Shift+Tab 例外，返回 CSI Z）。不认识的键返回空串。
// 说明：DECCKM（应用光标键）状态 UI 拿不到，统一发普通模式 CSI A..D。
std::string vtForKey(const std::string& key, bool ctrl, bool shift, bool alt);
// WM_CHAR 字符（已是 UTF-8）→ 发给 sendInput 的字节：Backspace 0x08 → 0x7f；alt 时前置 ESC
std::string vtForChar(const std::string& utf8, bool alt);

std::string pasteRejectedMessage(size_t bytes, size_t limit);

// ---- 滚动历史搜索条（searchHistory，Ctrl+Shift+F）----
// 状态文字：空 needle → ""；pending → "搜索中…"；hasNewOutput → 提示按 Enter 重搜；restarted → 同上（内容已变化）；
// found → "n/total"（n = index+1，total=-1 时 "n/?"）；否则 "无结果"
std::string searchStatusText(const std::string& needle, const uishell::SearchResult& r);

enum class SearchBarKey { Enter, ShiftEnter, Escape };
struct SearchBarAction {
  enum Kind { None, Find, Clear, Close } kind = None;
  bool backward = true;   // Find：true = 向上（更早）
  bool restart = false;   // Find：先 clearSearch() 再搜，从底部重新开始（needle 不变也算新一轮）
};
// 搜索条按键 / 输入状态机（平台无关；Win32 只负责把动作落到 PaneBackend）
class SearchBarModel {
 public:
  SearchBarAction onTextChanged(const std::string& needle);  // 空 → Clear；否则从头向上搜
  SearchBarAction onKey(SearchBarKey k);                     // Enter=向上，Shift+Enter=向下，Esc=Close
  // 搜索结果（findInScrollback 的返回或 onSearchUpdated）。restarted 时自动从头重搜一次；
  // 重搜的结果仍是 restarted 就停下，留给用户按 Enter（防循环）
  SearchBarAction onResult(const uishell::SearchResult& r);
  void reset();                                              // 关闭搜索条
  const std::string& needle() const { return needle_; }
  const uishell::SearchResult& last() const { return last_; }
  std::string statusText() const { return searchStatusText(needle_, last_); }
 private:
  bool needsRestart() const { return last_.hasNewOutput || last_.restarted; }
  std::string needle_;
  uishell::SearchResult last_;
  bool autoRestarted_ = false;
};

}  // namespace uicfg
