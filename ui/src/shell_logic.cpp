#include "uicfg/shell_logic.h"
#include <cctype>
namespace uicfg {
static std::string up(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
static std::optional<std::string> normKey(const std::string& k) {
  static const char* named[] = {"Tab", "PageUp", "PageDown", "Home", "End", "Up", "Down", "Left", "Right",
                                "Plus", "Minus", "Enter", "Escape", "Insert", "Delete", "Space", "Arrow"};
  std::string u = up(k);
  if (u == "PGUP") u = "PAGEUP"; if (u == "PGDN") u = "PAGEDOWN"; if (u == "ESC") u = "ESCAPE";
  if (u == "=" ) u = "PLUS"; if (u == "-") u = "MINUS"; if (u == "DEL") u = "DELETE";
  for (auto* n : named) if (up(n) == u) return std::string(n);
  if (u.size() == 1 && std::isalnum(static_cast<unsigned char>(u[0]))) return u;
  if (u.size() >= 2 && u[0] == 'F') {
    try { int n = std::stoi(u.substr(1)); if (n >= 1 && n <= 24 && std::to_string(n) == u.substr(1)) return u; } catch (...) {}
  }
  return std::nullopt;
}
std::optional<Chord> parseChord(const std::string& text) {
  Chord c; std::vector<std::string> parts; std::string cur;
  for (char ch : text) { if (ch == '+' && !cur.empty()) { parts.push_back(cur); cur.clear(); } else if (ch != ' ') cur += ch; }
  if (cur.empty()) return std::nullopt;
  parts.push_back(cur);
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    auto m = up(parts[i]);
    if (m == "CTRL" || m == "CONTROL") c.ctrl = true; else if (m == "SHIFT") c.shift = true; else if (m == "ALT") c.alt = true;
    else return std::nullopt;
  }
  auto k = normKey(parts.back()); if (!k) return std::nullopt;
  c.key = *k; return c;
}
KeyMatch matchKey(const std::map<std::string, std::string>& b, const Chord& p) {
  for (auto& [action, text] : b) {
    auto c = parseChord(text); if (!c) continue;
    if (c->ctrl != p.ctrl || c->shift != p.shift || c->alt != p.alt) continue;
    if (c->key == p.key) return {action, std::nullopt};
    if (c->key == "Arrow") {
      if (p.key == "Left") return {action, Direction::Left};
      if (p.key == "Right") return {action, Direction::Right};
      if (p.key == "Up") return {action, Direction::Up};
      if (p.key == "Down") return {action, Direction::Down};
    }
  }
  return {};
}
std::string normalizePasteText(const std::string& in) {
  std::string out; out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char ch = in[i];
    if (ch == '\0') break;
    if (ch == '\r') { out += '\r'; if (i + 1 < in.size() && in[i + 1] == '\n') ++i; }
    else if (ch == '\n') out += '\r';
    else out += ch;
  }
  return out;
}
LinkPrompt linkPrompt(const std::string& url) {
  auto c = checkLink(url);
  if (c.decision == LinkDecision::Allow)
    return {true, "要在浏览器中打开以下链接吗？\n\n" + c.displayUrl, c.displayUrl};
  if (c.decision == LinkDecision::RejectScheme)
    return {false, "已阻止打开链接：只允许 http/https，不支持 \"" + c.scheme + "\"。\n\n" + c.displayUrl, c.displayUrl};
  return {false, "已阻止打开链接：地址格式无效。\n\n" + c.displayUrl, c.displayUrl};
}
std::string stateLabel(uishell::SessionState s) {
  using S = uishell::SessionState;
  switch (s) {
    case S::Starting: return "正在启动"; case S::Running: return "运行中"; case S::Reconnecting: return "正在重连";
    case S::Exited: return "已退出"; case S::Failed: return "失败";
  }
  return "";
}
PaneOverlay overlayFor(const uishell::SessionStatus& st) {
  using S = uishell::SessionState;
  if (st.state == S::Failed) return {true, true, "会话失败：" + (st.errorText.empty() ? std::string("未知错误") : st.errorText)};
  if (st.state == S::Exited) return {true, true, "会话已退出（退出码 " + std::to_string(st.exitCode) + "）"};
  return {false, false, {}};
}

bool statusAffectsSessionTree(uishell::SessionState prev, uishell::SessionState next) {
  using S = uishell::SessionState;
  auto inRunningSet = [](S s) { return s != S::Exited && s != S::Failed; };  // 与 SessionSnapshots 生命周期一致
  return inRunningSet(prev) != inRunningSet(next);
}
std::string pasteRejectedMessage(size_t bytes, size_t limit) {
  return "粘贴内容过大（" + std::to_string(bytes / 1024) + " KiB），上限 " + std::to_string(limit / 1024) + " KiB，未发送。";
}
}  // namespace uicfg
namespace uicfg {
std::string vtForKey(const std::string& key, bool ctrl, bool shift, bool alt) {
  int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
  auto csiLetter = [&](char c) { return m == 1 ? std::string("\x1b[") + c : "\x1b[1;" + std::to_string(m) + c; };
  auto csiTilde = [&](int n) { return m == 1 ? "\x1b[" + std::to_string(n) + "~" : "\x1b[" + std::to_string(n) + ";" + std::to_string(m) + "~"; };
  if (key == "Up") return csiLetter('A'); if (key == "Down") return csiLetter('B');
  if (key == "Right") return csiLetter('C'); if (key == "Left") return csiLetter('D');
  if (key == "Home") return csiLetter('H'); if (key == "End") return csiLetter('F');
  if (key == "Insert") return csiTilde(2); if (key == "Delete") return csiTilde(3);
  if (key == "PageUp") return csiTilde(5); if (key == "PageDown") return csiTilde(6);
  if (key == "Tab" && shift && !ctrl && !alt) return "\x1b[Z";
  if (key.size() >= 2 && key[0] == 'F') {
    int n = 0; try { n = std::stoi(key.substr(1)); } catch (...) { return {}; }
    if (n >= 1 && n <= 4) {
      char c = "PQRS"[n - 1];
      return m == 1 ? std::string("\x1bO") + c : "\x1b[1;" + std::to_string(m) + c;
    }
    static const int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};  // F5..F12
    if (n >= 5 && n <= 12) return csiTilde(codes[n - 5]);
  }
  return {};
}
std::string vtForChar(const std::string& utf8, bool alt) {
  std::string s = utf8 == "\b" ? std::string("\x7f") : utf8;
  return alt ? "\x1b" + s : s;
}
}  // namespace uicfg

std::optional<std::wstring> uicfg::parseCommandArg(const std::vector<std::wstring>& argv) {
  const std::wstring k = L"--command", ke = L"--command=";
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == k) return i + 1 < argv.size() ? argv[i + 1] : std::wstring();
    if (argv[i].rfind(ke, 0) == 0) return argv[i].substr(ke.size());
  }
  return std::nullopt;
}

namespace uicfg {

std::string searchStatusText(const std::string& needle, const uishell::SearchResult& r) {
  if (needle.empty()) return "";
  if (r.hasNewOutput) return "有新输出，按 Enter 重新搜索";
  if (r.restarted) return "内容已变化，按 Enter 重新搜索";
  if (r.pending) return "搜索中…";
  if (!r.found) return "无结果";
  return std::to_string(r.index + 1) + "/" + (r.total < 0 ? std::string("?") : std::to_string(r.total));
}

SearchBarAction SearchBarModel::onTextChanged(const std::string& needle) {
  needle_ = needle;
  last_ = {};
  autoRestarted_ = false;
  if (needle_.empty()) return {SearchBarAction::Clear, true, false};
  return {SearchBarAction::Find, true, true};
}

SearchBarAction SearchBarModel::onKey(SearchBarKey k) {
  if (k == SearchBarKey::Escape) return {SearchBarAction::Close, true, false};
  if (needle_.empty()) return {};
  if (needsRestart()) { autoRestarted_ = false; return {SearchBarAction::Find, true, true}; }  // 从头（底部）向上重搜
  return {SearchBarAction::Find, k == SearchBarKey::Enter, false};
}

SearchBarAction SearchBarModel::onResult(const uishell::SearchResult& r) {
  last_ = r;
  if (needle_.empty()) return {};
  if (r.restarted && !autoRestarted_) { autoRestarted_ = true; return {SearchBarAction::Find, true, true}; }
  if (!r.restarted) autoRestarted_ = false;
  return {};
}

void SearchBarModel::reset() { needle_.clear(); last_ = {}; autoRestarted_ = false; }

}  // namespace uicfg
