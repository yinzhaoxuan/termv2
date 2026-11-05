// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/diag_bundle.h"
#include "uicfg/memory_ledger.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace uicfg {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool alpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
bool word(char c) { return alpha(c) || digit(c) || c == '_'; }
bool hex(char c) { c = lower(c); return digit(c) || (c >= 'a' && c <= 'f'); }
bool slash(char c) { return c == '/' || c == '\\'; }
std::string canonical(std::string_view s, bool path = false) {
  std::string out(s);
  for (char& c : out) c = path && slash(c) ? '/' : lower(c);
  return out;
}
bool equalAt(std::string_view text, std::size_t pos, std::string_view value, bool path = false) {
  if (value.size() > text.size() - pos) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (path && slash(text[pos + i]) && slash(value[i])) continue;
    if (lower(text[pos + i]) != lower(value[i])) return false;
  }
  return true;
}
bool boundaries(std::string_view s, std::size_t begin, std::size_t end) {
  return (begin == 0 || !word(s[begin - 1])) && (end == s.size() || !word(s[end]));
}
// 每次只扫描尚未替换的片段，避免记号被后续规则再次匹配。
struct Piece {
  std::string text, category, key;
  bool protectedText = false;
};
struct Match {
  std::size_t length;
  Piece piece;
};
Match token(std::size_t length, const std::string& category, std::string key,
            std::string prefix = {}) {
  return {length, {std::move(prefix), category, std::move(key), true}};
}
template<class Rule>
void applyRule(std::vector<Piece>& pieces, Rule rule) {
  std::vector<Piece> out;
  for (auto& piece : pieces) {
    if (piece.protectedText) { out.push_back(std::move(piece)); continue; }
    const std::string_view text(piece.text);
    std::size_t start = 0;
    for (std::size_t pos = 0; pos < text.size();) {
      auto match = rule(text, pos);
      if (!match) { ++pos; continue; }
      if (pos > start) out.push_back({std::string(text.substr(start, pos - start)), {}, {}, false});
      out.push_back(std::move(match->piece));
      pos += match->length;
      start = pos;
    }
    if (start < text.size()) out.push_back({std::string(text.substr(start)), {}, {}, false});
  }
  pieces = std::move(out);
}
struct KnownValues {
  std::vector<std::pair<std::string, std::string>> values;
  std::array<bool, 256> firstByte{};
};
std::optional<Match> redactKnown(std::string_view s, std::size_t pos, const KnownValues& known) {
  if (!known.firstByte[static_cast<unsigned char>(lower(s[pos]))]) return std::nullopt;
  for (const auto& [category, value] : known.values) {
    const bool path = category == "path";
    if (!equalAt(s, pos, value, path)) continue;
    const auto end = pos + value.size();
    if (!path && !boundaries(s, pos, end)) continue;
    // hv- 前缀属于同一个 VM 身份，不留在记号外。
    if (category == "vm" && pos >= 3 && equalAt(s, pos - 3, "hv-")) continue;
    return token(value.size(), category, canonical(value, path));
  }
  return std::nullopt;
}
bool base64(char c) { return alpha(c) || digit(c) || c == '+' || c == '/'; }
std::optional<Match> redactFingerprint(std::string_view s, std::size_t pos) {
  constexpr std::string_view prefix = "SHA256:";
  if (s.substr(pos, prefix.size()) != prefix) return std::nullopt;
  auto end = pos + prefix.size();
  const auto begin = end;
  while (end < s.size() && base64(s[end])) ++end;
  if (end - begin != 43) return std::nullopt;
  if (end < s.size() && s[end] == '=') ++end;
  if (end < s.size() && (base64(s[end]) || s[end] == '=')) return std::nullopt;
  return token(end - pos, "fingerprint", std::string(s.substr(pos, end - pos)));
}
std::optional<std::size_t> guidEnd(std::string_view s, std::size_t pos) {
  constexpr std::size_t kGuidLength = 36;
  if (s.size() - pos < kGuidLength) return std::nullopt;
  for (std::size_t i = 0; i < kGuidLength; ++i) {
    const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
    if (hyphen ? s[pos + i] != '-' : !hex(s[pos + i])) return std::nullopt;
  }
  return pos + kGuidLength;
}
std::optional<Match> redactGuid(std::string_view s, std::size_t pos) {
  auto begin = pos;
  if (equalAt(s, begin, "hv-")) begin += 3;
  const bool braces = begin < s.size() && s[begin] == '{';
  if (braces) ++begin;
  auto end = guidEnd(s, begin);
  if (!end) return std::nullopt;
  if (braces) {
    if (*end == s.size() || s[*end] != '}') return std::nullopt;
    ++*end;
  }
  if (!boundaries(s, pos, *end)) return std::nullopt;
  return token(*end - pos, "guid", canonical(s.substr(pos, *end - pos)));
}
std::optional<Match> redactIpv4(std::string_view s, std::size_t pos) {
  if (!digit(s[pos]) || (pos && (digit(s[pos - 1]) || s[pos - 1] == '.'))) return std::nullopt;
  auto end = pos;
  for (int group = 0; group < 4; ++group) {
    const auto begin = end;
    unsigned value = 0;
    while (end < s.size() && digit(s[end])) {
      if (end - begin == 3) return std::nullopt;
      value = value * 10 + static_cast<unsigned>(s[end++] - '0');
    }
    if (end == begin || value > 255) return std::nullopt;
    if (group < 3 && (end == s.size() || s[end++] != '.')) return std::nullopt;
  }
  if (end < s.size() && (digit(s[end]) || s[end] == '.')) return std::nullopt;
  const auto value = s.substr(pos, end - pos);
  if (value == "127.0.0.1" || value == "0.0.0.0") return std::nullopt;
  return token(end - pos, "ipv4", std::string(value));
}
// 按 DS-15 识别十六进制片段；字母、:: 或至少四组用来排除时间。
bool validIpv6(std::string_view s) {
  std::size_t groups = 0, colons = 0;
  bool compressed = false, hasLetter = false;
  for (std::size_t pos = 0; pos < s.size();) {
    if (s[pos] == ':') {
      if (pos + 1 == s.size() || s[pos + 1] != ':' || compressed) return false;
      compressed = true; colons += 2; pos += 2;
      if (pos == s.size()) break;
    }
    const auto begin = pos;
    while (pos < s.size() && hex(s[pos])) {
      hasLetter = hasLetter || alpha(s[pos]); ++pos;
    }
    if (pos == begin || pos - begin > 4) return false;
    ++groups;
    if (pos < s.size()) {
      if (s[pos] != ':') return false;
      if (pos + 1 < s.size() && s[pos + 1] == ':') continue;
      ++colons; ++pos;
      if (pos == s.size()) return false;
    }
  }
  return colons >= 2 && (compressed ? groups < 8 : groups <= 8) &&
         (hasLetter || compressed || groups >= 4);
}
std::optional<Match> redactIpv6(std::string_view s, std::size_t pos) {
  if (!(hex(s[pos]) || s[pos] == ':' || s[pos] == '[')) return std::nullopt;
  if (pos && (word(s[pos - 1]) || s[pos - 1] == ':' || s[pos - 1] == '%' || s[pos - 1] == '[')) return std::nullopt;
  const bool bracket = s[pos] == '[';
  auto begin = pos + (bracket ? 1 : 0), end = begin;
  while (end < s.size() && (hex(s[end]) || s[end] == ':')) ++end;
  const auto address = s.substr(begin, end - begin);
  if (!validIpv6(address)) return std::nullopt;
  if (end < s.size() && s[end] == '%') {
    const auto scope = ++end;
    while (end < s.size() && (word(s[end]) || s[end] == '-' || s[end] == '.')) ++end;
    if (scope == end) return std::nullopt;
  }
  if (bracket) {
    if (end == s.size() || s[end] != ']') return std::nullopt;
    ++end;
  } else if (end < s.size() && (word(s[end]) || s[end] == ':')) return std::nullopt;
  if (address == "::1") return std::nullopt;
  return token(end - pos, "ipv6", canonical(s.substr(pos, end - pos)));
}
std::optional<Match> redactPipe(std::string_view s, std::size_t pos) {
  constexpr std::string_view prefix = R"(\\.\pipe\)";
  if (!equalAt(s, pos, prefix)) return std::nullopt;
  const auto begin = pos + prefix.size();
  auto end = begin;
  while (end < s.size()) {
    const auto c = static_cast<unsigned char>(s[end]);
    if (c <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '/' || c == '\\') break;
    ++end;
  }
  if (end == begin || canonical(s.substr(begin, end - begin)) == "openssh-ssh-agent") return std::nullopt;
  return token(end - pos, "pipe", std::string(s.substr(begin, end - begin)), std::string(prefix));
}
std::optional<Match> redactUserHome(std::string_view s, std::size_t pos) {
  constexpr std::string_view prefix = ":/Users/";
  if (!alpha(s[pos]) || !equalAt(s, pos + 1, prefix, true)) return std::nullopt;
  if (pos && word(s[pos - 1])) return std::nullopt;
  const auto begin = pos + 1 + prefix.size();
  auto end = begin;
  while (end < s.size() && !slash(s[end]) && static_cast<unsigned char>(s[end]) >= 0x20 &&
         s[end] != '"' && s[end] != '\'' && s[end] != '<' && s[end] != '>') ++end;
  if (end == begin || end == s.size() || !slash(s[end])) return std::nullopt;
  const auto name = canonical(s.substr(begin, end - begin));
  if (name == "public" || name == "default") return std::nullopt;
  return Match{end + 1 - pos, {std::string(s.substr(pos, 1)) + ":\\Users\\<user-home>\\", {}, {}, true}};
}
std::string loadStatus(LoadStatus status) {
  switch (status) {
    case LoadStatus::Ok: return "已读取";
    case LoadStatus::NotFound: return "缺失（使用默认值）";
    case LoadStatus::RecoveredBackup: return "已从备份恢复";
    case LoadStatus::NewerSchema: return "高版本（只读）";
    case LoadStatus::Corrupt: return "损坏（使用默认值）";
  }
  return "未知";
}
std::string normalizeNewlines(std::string_view text) {
  std::string out; out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
    out += text[i];
  }
  return out;
}
DiagLogSection readLog(const std::filesystem::path& dir, const std::string& name, std::size_t maxBytes) {
  DiagLogSection section{name, std::nullopt, "缺失"};
  const auto file = dir / name;
  std::error_code ec;
  const bool exists = std::filesystem::exists(file, ec);
  if (ec) { section.note = "读取失败：" + ec.message(); return section; }
  if (!exists) return section;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) { section.note = "读取失败：" + ec.message(); return section; }
  std::ifstream in(file, std::ios::binary);
  if (!in) { section.note = "读取失败：无法打开文件"; return section; }
  const bool truncated = size > maxBytes;
  const auto bytes = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxBytes));
  in.seekg(static_cast<std::streamoff>(size - bytes));
  std::string text(bytes, '\0');
  in.read(text.data(), static_cast<std::streamsize>(bytes));
  if (!in || static_cast<std::size_t>(in.gcount()) != bytes) {
    section.note = "读取失败：日志在读取时发生变化或 I/O 错误"; return section;
  }
  if (truncated) {
    const auto newline = text.find('\n');
    text.erase(0, newline == std::string::npos ? text.size() : newline + 1);
    section.note = "截断：只保留最后 " + std::to_string(maxBytes) + " 字节，从下一整行开始";
  } else section.note = std::to_string(size) + " 字节";
  section.text = normalizeNewlines(text);
  return section;
}
}  // namespace

RedactionContext redactionContextFrom(const ConnectionsFile& conns,
    const std::vector<std::pair<std::string, std::string>>& extra) {
  RedactionContext ctx{extra};
  for (const auto& c : conns.connections) {
    if (c.endpoint) {
      const auto& ep = *c.endpoint;
      if (ep.mode == EndpointMode::Manual && ep.host) ctx.known.emplace_back("host", *ep.host);
      if (ep.hyperv) {
        const auto& hv = *ep.hyperv;
        if (hv.fallbackHost) ctx.known.emplace_back("host", *hv.fallbackHost);
        ctx.known.emplace_back("vm", hv.vmId);
        auto id = canonical(hv.vmId);
        if (id.size() >= 2 && id.front() == '{' && id.back() == '}') id = id.substr(1, id.size() - 2);
        if (guidEnd(id, 0) == id.size()) {
          ctx.known.emplace_back("vm", id);
          ctx.known.emplace_back("vm", "{" + id + "}");
          ctx.known.emplace_back("vm", "hv-{" + id + "}");
          ctx.known.emplace_back("vm", "hv-" + id);
        }
      }
    }
    if (c.ssh) {
      ctx.known.emplace_back("user", c.ssh->user);
      if (c.ssh->identityFile) ctx.known.emplace_back("path", *c.ssh->identityFile);
    }
    if (c.local) {
      ctx.known.emplace_back("path", c.local->executable);
      if (c.local->workingDirectory) ctx.known.emplace_back("path", *c.local->workingDirectory);
    }
  }
  return ctx;
}
std::string redactDiagnosticText(std::string_view text, const RedactionContext& ctx) {
  KnownValues known{ctx.known};
  std::erase_if(known.values, [](const auto& value) {
    // 按 UTF-8 字符计数，中文短名也遵守最短三字符的约定。
    const auto characters = std::count_if(value.second.begin(), value.second.end(), [](unsigned char c) {
      return (c & 0xc0) != 0x80;
    });
    return characters < 3 || (value.first != "host" && value.first != "user" &&
           value.first != "vm" && value.first != "path");
  });
  std::stable_sort(known.values.begin(), known.values.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
  // 首字节不可能匹配时直接跳过；路径的两种分隔符等价。
  for (const auto& [category, value] : known.values) {
    known.firstByte[static_cast<unsigned char>(lower(value.front()))] = true;
    if (category == "path" && slash(value.front())) {
      known.firstByte[static_cast<unsigned char>('/')] = true;
      known.firstByte[static_cast<unsigned char>('\\')] = true;
    }
  }
  std::vector<Piece> pieces{{std::string(text), {}, {}, false}};
  applyRule(pieces, [&](std::string_view s, std::size_t pos) { return redactKnown(s, pos, known); });
  applyRule(pieces, redactFingerprint);
  applyRule(pieces, redactGuid);
  applyRule(pieces, redactIpv4);
  applyRule(pieces, redactIpv6);
  applyRule(pieces, redactPipe);
  applyRule(pieces, redactUserHome);
  // 最后按全文位置编号，不受已知值的长度排序影响。
  std::map<std::string, std::map<std::string, std::size_t>> numbers;
  std::string out;
  for (const auto& piece : pieces) {
    out += piece.text;
    if (!piece.category.empty()) {
      auto& category = numbers[piece.category];
      auto [it, inserted] = category.emplace(piece.key, category.size() + 1);
      (void)inserted;
      out += "<" + piece.category + "-" + std::to_string(it->second) + ">";
    }
  }
  return out;
}
std::vector<std::string> configSummaryLines(const ConnectionsFile& conns, LoadStatus cs,
    const FileValidation& validation, const SettingsFile& settings, LoadStatus ss) {
  std::size_t manual = 0, hv = 0, local = 0, follow = 0, compatible = 0, direct = 0, invalidTransport = 0;
  for (const auto& c : conns.connections) {
    if (c.kind == ConnectionKind::Local) { ++local; continue; }
    if (c.endpoint && c.endpoint->mode == EndpointMode::HyperV) ++hv; else ++manual;
    if (!c.ssh || !c.ssh->transport) ++follow;
    else if (const auto t = parseSshTransport(*c.ssh)) {
      if (*t == SshTransport::Compatible) ++compatible; else ++direct;
    } else ++invalidTransport;
  }
  std::ostringstream counts;
  counts << "connections.json = " << loadStatus(cs) << "，revision " << conns.revision << "，"
         << conns.connections.size() << " 个连接（SSH 手动 " << manual << " / Hyper-V " << hv
         << " / 本地 " << local << "），校验未通过 " << validation.byConnection.size() << " 个";
  if (!validation.fileErrors.empty()) counts << "，文件错误 " << validation.fileErrors.size() << " 个";
  std::string transport = "传输 = 跟随默认 " + std::to_string(follow) + " / 兼容 " + std::to_string(compatible) + " / 直连 " + std::to_string(direct);
  if (invalidTransport) transport += " / 无效 " + std::to_string(invalidTransport);
  const auto osc = settings.osc52OrDefault();
  std::ostringstream switches;
  switches << "settings.json = " << loadStatus(ss) << "；字体 " << settings.fontFamily << " " << settings.fontSize
           << "；osc52 = 写" << (osc.allowWrite ? "开" : "关") << " / 读" << (osc.allowRead ? "开" : "关") << " / " << osc.maxBytes;
  if (settings.osc52Invalid()) switches << "（无效，按全关）";
  switches << "；confirmMultilinePaste = " << (settings.confirmMultilinePasteRaw ? "无效（默认）" :
      !settings.confirmMultilinePaste ? "默认" : (*settings.confirmMultilinePaste ? "开" : "关"));
  switches << "；sshDefaultTransport = " << (settings.sshDefaultTransportInvalid() ? "无效（默认）" :
      !settings.sshDefaultTransport ? "默认" : sshDefaultTransportName(*settings.sshDefaultTransport));
  std::size_t invalidProfiles = 0;
  for (const auto& [id, p] : settings.appearanceProfiles) if (p.invalidField) ++invalidProfiles;
  std::string profiles = "外观 Profile = " + std::to_string(settings.appearanceProfiles.size()) + " 个（无效 " + std::to_string(invalidProfiles) + "）";
  if (settings.appearanceProfilesRaw) profiles += "，字段无效";
  // 只列已知动作的 ID，不泄露未知键和快捷键原文。
  std::string changed;
  for (const auto& [action, chord] : defaultSettings().keybindings) {
    const auto found = settings.keybindings.find(action);
    if (found != settings.keybindings.end() && found->second != chord) {
      if (!changed.empty()) changed += ", ";
      changed += action;
    }
  }
  return {counts.str(), transport, switches.str(), profiles, "改过的快捷键 = " + (changed.empty() ? "无" : changed)};
}
std::vector<DiagLogSection> readDiagLogs(const std::filesystem::path& logsDir, std::size_t maxBytes) {
  std::vector<DiagLogSection> sections;
  for (const char* name : {"pane-diag.log.2", "pane-diag.log.1", "pane-diag.log"}) sections.push_back(readLog(logsDir, name, maxBytes));
  return sections;
}
std::string buildDiagnosticBundle(const DiagEnvironment& env, const std::vector<std::string>& summary,
    const std::vector<DiagLogSection>& logs, const RedactionContext& ctx) {
  std::string text = "# it2w 诊断包\n# 已脱敏：主机、用户、IP、指纹、GUID、管道名与用户目录已替换为 <类别-序号>。发送前请检查。\n"
      "# 少于 3 个字符的已知名字不会精确替换。\n\n[构建与环境]\n生成时间 = " + env.generatedAt +
      "\n提交（配置时） = " + env.commit + "\n构建 = " + env.build + "\nWindows = " + env.windowsVersion + "\n";
  for (const auto& [name, value] : env.envFlags) text += name + " = " + value + "\n";
  text += "内存（账本）= 合计 " + memoryLine(env.memoryTotal) + " / " + (std::to_string(LedgerLimits{}.app >> 20) + " MiB") +
      "，" + std::to_string(env.memorySessions) + " 个会话；最大 " + memoryLine(env.memoryMax) + "\n";
  text += "账本不含传输队列、显存与 DWrite 缓存\n";
  text += "\n[配置摘要]\n";
  for (const auto& line : summary) text += line + "\n";
  for (const auto& log : logs) {
    text += "\n[日志 " + log.name + "]（" + log.note + "）\n";
    if (log.text) { text += *log.text; if (!log.text->empty() && log.text->back() != '\n') text += '\n'; }
  }
  return redactDiagnosticText(text, ctx);
}
bool saveDiagnosticBundle(const std::filesystem::path& file, const std::string& text, std::string* error) {
  auto tmp = file; tmp += ".tmp";
  if (writeDurable(tmp, text, error) && replaceFile(tmp, file, error)) return true;
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  return false;
}
std::string diagnosticFileName(const std::string& stamp) { return "it2w-diag-" + stamp + ".txt"; }
}  // namespace uicfg
