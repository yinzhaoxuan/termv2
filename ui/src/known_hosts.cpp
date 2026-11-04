// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/known_hosts.h"
#include "uicfg/store.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

namespace uicfg {
namespace fs = std::filesystem;
namespace {
bool validKeyType(std::string_view type) {
  return type == "ssh-ed25519" || type == "ecdsa-sha2-nistp256" ||
         type == "ecdsa-sha2-nistp384" || type == "ecdsa-sha2-nistp521" || type == "ssh-rsa";
}
bool validBase64(std::string_view text) {
  if (text.empty() || text.size() % 4) return false;
  const auto padding = text.find('=');
  if (padding != std::string_view::npos) {
    const auto tail = text.substr(padding);
    if (tail != "=" && tail != "==") return false;
    text = text.substr(0, padding);
  }
  return std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
  });
}
bool containsLine(const std::string& text, const std::string& line) {
  std::istringstream lines(text);
  std::string existing;
  const auto record = std::string_view(line).substr(0, line.size() - 1);
  while (std::getline(lines, existing)) {
    if (!existing.empty() && existing.back() == '\r') existing.pop_back();
    if (existing == record) return true;
  }
  return false;
}
}  // namespace

std::optional<std::string> knownHostsLine(const uishell::HostKeyOffer& key) {
  const auto& host = key.knownHostsName;
  if (host.empty() || host.front() == '@' || host.front() == '|') return std::nullopt;
  for (unsigned char c : host)
    if (c <= 0x20 || c == 0x7f || c == ',') return std::nullopt;
  if (!validKeyType(key.keyType) || !validBase64(key.keyBase64)) return std::nullopt;
  return host + " " + key.keyType + " " + key.keyBase64 + "\n";
}

bool appendKnownHost(const fs::path& file, const std::string& line, std::string* error) {
  if (error) error->clear();
  auto fail = [&](const std::string& reason) { if (error) *error = reason; return false; };
  if (line.empty() || line.back() != '\n' || line.find('\n') != line.size() - 1)
    return fail("known_hosts 条目必须是完整的一行");
  std::error_code ec;
  const bool exists = fs::exists(file, ec);
  if (ec) return fail("读取 known_hosts 失败：" + ec.message());
  std::string text;
  if (exists) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("打开 known_hosts 失败");
    char buffer[64 * 1024];
    while (in) {
      in.read(buffer, sizeof buffer);
      text.append(buffer, static_cast<size_t>(in.gcount()));
    }
    if (!in.eof()) return fail("读取 known_hosts 失败");
  }
  if (containsLine(text, line)) return true;
  if (!text.empty() && text.back() != '\n') text += '\n';
  text += line;
  if (!file.parent_path().empty()) {
    fs::create_directories(file.parent_path(), ec);
    if (ec) return fail("创建 SSH 目录失败：" + ec.message());
  }
  fs::path tmp = file; tmp += ".tmp";
  if (writeDurable(tmp, text, error) && replaceFile(tmp, file, error)) return true;
  if (fs::is_regular_file(tmp, ec)) fs::remove(tmp, ec);
  return false;
}

}  // namespace uicfg
