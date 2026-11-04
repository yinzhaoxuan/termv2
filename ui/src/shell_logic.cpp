// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/shell_logic.h"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
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
static std::string normalizeNewlines(std::string_view in) {
  std::string out; out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char ch = in[i];
    if (ch == '\r') { out += '\r'; if (i + 1 < in.size() && in[i + 1] == '\n') ++i; }
    else if (ch == '\n') out += '\r';
    else out += ch;
  }
  return out;
}

std::string normalizePasteText(const std::string& in) {
  return normalizeNewlines(std::string_view(in).substr(0, in.find('\0')));
}

constexpr size_t kPastePreviewMaxLines = 10, kPastePreviewMaxCodePoints = 120;
constexpr size_t kClipboardPreviewMaxLines = 3, kClipboardPreviewMaxCodePoints = 60;

static size_t utf8SequenceBytes(std::string_view text, size_t i) {
  size_t end = i + 1;
  while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
  return end - i;
}

static std::string renderControlByte(unsigned char ch) {
  return {'^', ch == 0x7f ? '?' : static_cast<char>(ch + '@')};
}

static std::string previewLine(std::string_view line, size_t maxCodePoints) {
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < line.size();) {
    auto ch = static_cast<unsigned char>(line[i]);
    bool control = ch < 0x20 || ch == 0x7f;
    size_t width = control ? 2 : 1;  // 上限按转义后的可见码点计算
    if (chars + width > maxCodePoints) break;
    if (control) {
      out += renderControlByte(ch);
      ++i;
    } else {
      size_t bytes = utf8SequenceBytes(line, i);
      out.append(line.substr(i, bytes));
      i += bytes;
    }
    chars += width;
  }
  return out;
}

// 按 CR 分行，末尾 CR 不额外算空行；空串为 0 行。
static size_t countPastedLines(std::string_view text) {
  size_t lines = std::count(text.begin(), text.end(), '\r');
  if (!text.empty() && text.back() != '\r') ++lines;
  return lines;
}

static std::string textPreview(const std::string& text, size_t maxLines, size_t maxCodePoints) {
  const size_t lines = countPastedLines(text);
  std::string message;
  size_t start = 0, shown = 0;
  while (start < text.size() && shown < maxLines) {
    size_t end = text.find('\r', start);
    if (end == std::string::npos) end = text.size();
    message += '\n';
    message += previewLine(std::string_view(text).substr(start, end - start), maxCodePoints);
    start = end + 1;
    ++shown;
  }
  if (lines > shown) message += "\n…（其余 " + std::to_string(lines - shown) + " 行未显示）";
  return message;
}

PastePreview pastePreview(const std::string& text, bool confirmEnabled) {
  if (!confirmEnabled || text.find('\r') == std::string::npos) return {false, {}};
  std::string message = "将粘贴 " + std::to_string(countPastedLines(text)) + " 行、" + std::to_string(text.size()) + " 字节：";
  message += textPreview(text, kPastePreviewMaxLines, kPastePreviewMaxCodePoints);
  return {true, std::move(message)};
}

ClipboardReadPrompt clipboardReadPrompt(const std::string& connectionName, const std::string& clipboardText) {
  ClipboardReadPrompt out;
  const auto name = connectionName.empty() ? std::string("本地终端") : connectionName;
  out.mainText = "「" + name + "」中的程序请求读取本机剪贴板";
  if (clipboardText.empty()) {
    out.content = "剪贴板为空";
    return out;
  }
  size_t chars = 0;
  for (size_t i = 0; i < clipboardText.size(); i += utf8SequenceBytes(clipboardText, i)) ++chars;
  out.content = "剪贴板当前有 " + std::to_string(chars) + " 个字符。允许后，该程序会得到全部内容。";
  out.content += textPreview(normalizeNewlines(clipboardText), kClipboardPreviewMaxLines, kClipboardPreviewMaxCodePoints);
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
static std::string recoveryButton(const uishell::SessionStatus& st) {
  if (st.state != uishell::SessionState::Failed) return "重连";
  switch (st.recovery.kind) {
    case uishell::SessionRecovery::StartVmAndConnect: return "启动并连接";
    case uishell::SessionRecovery::ChooseAddress: return "选择地址…";
    case uishell::SessionRecovery::TrustHostKey: return "核对指纹…";
    case uishell::SessionRecovery::None: return "重连";
  }
  return "重连";
}
PaneOverlay paneOverlay(bool missingRef, bool notStarted, const std::string& connName, const uishell::SessionStatus& st) {
  using S = uishell::SessionState;
  auto button = recoveryButton(st);
  if (missingRef) return {true, false, "连接配置已删除（引用缺失）。请在左侧选择连接后新建会话。", button};
  if (notStarted) return {true, true, "未连接：" + connName + "。点击「连接」或按 Enter 开始", "连接"};
  if (st.state == S::Failed) {
    std::string text = "会话失败：" + (st.errorText.empty() ? std::string("未知错误") : st.errorText);
    if (st.hostKeyChanged)
      text += "\n主机密钥与已信任的不同。确认服务器确实更换过密钥后，手动删除 known_hosts 中的旧条目再连接。";
    return {true, true, text, button};
  }
  if (st.state == S::Exited) return {true, true, "会话已退出（退出码 " + std::to_string(st.exitCode) + "）", button};
  return {false, false, {}, button};
}

std::string hostKeyTrustPrompt(const uishell::HostKeyOffer& key, const std::string& knownHostsPath) {
  const std::string shortType = key.keyType == "ssh-ed25519" ? "ed25519" :
                                key.keyType == "ssh-rsa" ? "rsa" : "ecdsa";
  return "无法验证主机「" + key.knownHostsName + "」的身份。\n密钥类型：" + key.keyType +
         "\n指纹：" + key.fingerprint + "\n\n请通过可信渠道核对指纹，例如在服务器控制台运行：\n" +
         "  ssh-keygen -lf /etc/ssh/ssh_host_" + shortType + "_key.pub\n" +
         "一致后再信任。信任后会写入：\n" + knownHostsPath + "\n并重新连接。";
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
