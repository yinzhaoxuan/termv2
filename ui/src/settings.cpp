#include "uicfg/settings.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace uicfg {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::pair<const char*, const char*> kDefaults[] = {
    {"newTab", "Ctrl+Shift+T"},
    {"closePane", "Ctrl+Shift+W"},
    {"splitVertical", "Ctrl+Shift+D"},
    {"splitHorizontal", "Ctrl+Shift+E"},
    {"nextTab", "Ctrl+Tab"},
    {"prevTab", "Ctrl+Shift+Tab"},
    {"focusPane", "Alt+Arrow"},  // Alt+方向键
    {"copy", "Ctrl+Shift+C"},
    {"paste", "Ctrl+Shift+V"},
    {"pasteCtrlV", "Ctrl+V"},          // 设为 "" 则 Ctrl+V 原样发 0x16（vim 块选择）
    {"pasteShiftInsert", "Shift+Insert"},
    {"copyOrInterrupt", "Ctrl+C"},     // 有选区复制并清除，无选区照常发 0x03
    {"searchHistory", "Ctrl+Shift+F"},
    {"searchConnections", "Ctrl+Shift+L"},
    {"fontBigger", "Ctrl+Plus"},
    {"fontSmaller", "Ctrl+Minus"},
    {"fontReset", "Ctrl+0"},
    {"scrollPageUp", "Shift+PageUp"},
    {"scrollPageDown", "Shift+PageDown"},
    {"scrollLineUp", "Ctrl+Shift+Up"},
    {"scrollLineDown", "Ctrl+Shift+Down"},
    {"scrollToTop", "Ctrl+Shift+Home"},
    {"scrollToBottom", "Ctrl+Shift+End"},
};

enum class Parse { Ok, Newer, Bad };

Parse parseSettings(const fs::path& p, SettingsFile& out, std::string& why) {
  try {
    std::ifstream in(p, std::ios::binary);
    json j = json::parse(in);
    int v = j.at("schemaVersion");
    if (v > kSchemaVersion) { why = "schemaVersion " + std::to_string(v); return Parse::Newer; }
    if (v < 1) { why = "无效 schemaVersion"; return Parse::Bad; }
    SettingsFile f; f.schemaVersion = v; f.revision = j.value("revision", 0LL);
    f.fontSize = j.value("fontSize", 12.0);
    f.fontFamily = j.value("fontFamily", "Cascadia Mono");
    if (j.contains("wordChars") && j["wordChars"].is_string()) f.wordChars = j["wordChars"].get<std::string>();
    if (j.contains("keybindings") && j.at("keybindings").is_object()) {
      for (auto it = j.at("keybindings").begin(); it != j.at("keybindings").end(); ++it)
        f.keybindings[it.key()] = it.value().get<std::string>();
    }
    // 缺省动作用默认值补齐，不覆盖用户已有绑定
    for (const auto& [action, chord] : kDefaults)
      if (!f.keybindings.count(action)) f.keybindings[action] = chord;
    out = std::move(f);
    return Parse::Ok;
  } catch (const std::exception& e) { why = e.what(); return Parse::Bad; }
}

}  // namespace

SettingsFile defaultSettings() {
  SettingsFile f;
  for (const auto& [action, chord] : kDefaults) f.keybindings[action] = chord;
  return f;
}

void resetKeybindings(SettingsFile& data) {
  data.keybindings.clear();
  for (const auto& [action, chord] : kDefaults) data.keybindings[action] = chord;
}

std::vector<ValidationError> validateSettings(const SettingsFile& data) {
  std::vector<ValidationError> errs;
  if (!(data.fontSize >= 6.0 && data.fontSize <= 72.0))
    errs.push_back({"fontSize", "字号必须在 6–72"});
  std::map<std::string, std::string> chordToAction;
  for (const auto& [action, chord] : data.keybindings) {
    if (chord.empty()) { errs.push_back({action, "快捷键不能为空"}); continue; }
    auto it = chordToAction.find(chord);
    if (it != chordToAction.end())
      errs.push_back({"keybindings", "快捷键冲突: " + chord + " 同时绑定 " + it->second + " 与 " + action});
    else chordToAction[chord] = action;
  }
  return errs;
}

SettingsLoadResult loadSettings(const fs::path& file) {
  SettingsLoadResult r;
  std::error_code ec;
  if (!fs::exists(file, ec)) {
    r.status = LoadStatus::NotFound;
    r.data = defaultSettings();
    return r;
  }
  std::string why;
  switch (parseSettings(file, r.data, why)) {
    case Parse::Ok: r.status = LoadStatus::Ok; return r;
    case Parse::Newer: r.status = LoadStatus::NewerSchema; r.readOnly = true; r.detail = why; return r;
    case Parse::Bad: break;
  }
  r.detail = "主文件无法解析: " + why;
  fs::path bak = file; bak += ".bak";
  std::string why2;
  if (fs::exists(bak, ec) && parseSettings(bak, r.data, why2) == Parse::Ok) {
    r.status = LoadStatus::RecoveredBackup;
    return r;
  }
  r.status = LoadStatus::Corrupt;
  r.readOnly = true;
  r.data = defaultSettings();
  return r;
}

bool saveSettings(const fs::path& file, SettingsFile& data, bool readOnly, std::string* error) {
  if (readOnly) { if (error) *error = "只读，拒绝写回"; return false; }
  if (auto errs = validateSettings(data); !errs.empty()) {
    if (error) *error = errs.front().message;
    return false;
  }
  json kb = json::object();
  for (const auto& [a, c] : data.keybindings) kb[a] = c;
  json j{{"schemaVersion", kSchemaVersion}, {"revision", data.revision + 1},
         {"fontSize", data.fontSize}, {"fontFamily", data.fontFamily},
         {"keybindings", kb}};
  if (data.wordChars) j["wordChars"] = *data.wordChars;
  fs::path tmp = file; tmp += ".tmp";
  if (!writeDurable(tmp, j.dump(2), error)) { std::error_code ec; fs::remove(tmp, ec); return false; }
  std::error_code ec;
  if (fs::exists(file, ec)) {
    SettingsFile probe; std::string why;
    if (parseSettings(file, probe, why) == Parse::Ok) {
      fs::path bak = file; bak += ".bak";
      fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
    }
  }
  if (!replaceFile(tmp, file, error)) { fs::remove(tmp, ec); return false; }
  data.revision += 1;
  return true;
}

}  // namespace uicfg

namespace uicfg {
ScrollCommand scrollCommandFor(const std::string& action, int visibleRows) {
  int page = visibleRows > 1 ? visibleRows - 1 : 1;  // keep one line of context
  if (action == "scrollPageUp") return {true, false, -page};
  if (action == "scrollPageDown") return {true, false, page};
  if (action == "scrollLineUp") return {true, false, -1};
  if (action == "scrollLineDown") return {true, false, 1};
  if (action == "scrollToTop") return {true, false, -(1 << 30)};  // backend clamps
  if (action == "scrollToBottom") return {true, true, 0};
  return {false, false, 0};
}
}  // namespace uicfg
