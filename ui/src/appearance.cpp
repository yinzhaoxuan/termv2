#include "uicfg/settings.h"
#include "appearance_json.h"
#include <algorithm>
#include <charconv>
#include <cstdio>

namespace uicfg {
namespace {
using nlohmann::json;
constexpr const char* kDefaultId = "appearance-default";

bool validId(const std::string& id) {
  return !id.empty() && id.size() <= 64 &&
      std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::optional<std::uint32_t> parseColor(const json& value) {
  if (!value.is_string()) return std::nullopt;
  const auto& s = value.get_ref<const std::string&>();
  if (s.size() != 7 || s[0] != '#') return std::nullopt;
  std::uint32_t color = 0;
  const auto [end, ec] = std::from_chars(s.data() + 1, s.data() + s.size(), color, 16);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return color;
}

std::string colorText(std::uint32_t value) {
  char text[8];
  std::snprintf(text, sizeof text, "#%06X", unsigned(value & 0xffffff));
  return text;
}

void overlay(uishell::PaneAppearance& out, const AppearanceProfile& p) {
  if (p.foreground) out.colors.foreground = *p.foreground;
  if (p.background) out.colors.background = *p.background;
  if (p.cursor) out.colors.cursor = *p.cursor;
  if (p.ansi) out.colors.ansi = *p.ansi;
  if (p.scrollbackLines) out.scrollbackLines = *p.scrollbackLines;
  if (p.memoryLimitMiB) out.memoryLimitBytes = std::uint64_t(*p.memoryLimitMiB) << 20;
}

std::string invalidWarning(const std::string& id, const std::string& field) {
  return "外观 Profile「" + id + "」无效：" + field + "，已用默认";
}
}  // namespace

namespace detail {
AppearanceProfile parseAppearanceProfile(const std::string& id, const json& value) {
  AppearanceProfile p;
  p.rawObject = value.dump();
  auto invalid = [&](const char* field) { if (!p.invalidField) p.invalidField = field; };
  if (!validId(id)) invalid("id");
  if (!value.is_object()) { invalid("Profile"); return p; }
  for (auto [key, dest] : {std::pair{"foreground", &p.foreground}, {"background", &p.background}, {"cursor", &p.cursor}}) {
    if (!value.contains(key)) continue;
    *dest = parseColor(value.at(key));
    if (!*dest) invalid(key);
  }
  if (value.contains("ansi")) {
    const auto& a = value.at("ansi");
    if (!a.is_array() || a.size() != 16) invalid("ansi");
    else {
      std::array<std::uint32_t, 16> colors{};
      bool ok = true;
      for (size_t i = 0; i < colors.size(); ++i) {
        if (auto c = parseColor(a[i])) colors[i] = *c;
        else ok = false;
      }
      if (ok) p.ansi = colors;
      else invalid("ansi");
    }
  }
  auto range = [&](const char* key, std::uint32_t lo, std::uint32_t hi, auto& dest) {
    if (!value.contains(key)) return;
    const auto& n = value.at(key);
    if (!n.is_number_integer()) { invalid(key); return; }
    if (n.is_number_unsigned()) {
      const auto v = n.get<std::uint64_t>();
      if (v < lo || v > hi) { invalid(key); return; }
    } else {
      const auto v = n.get<std::int64_t>();
      if (v < lo || v > hi) { invalid(key); return; }
    }
    dest = n.get<std::uint32_t>();
  };
  range("scrollbackLines", 100, 1000000, p.scrollbackLines);
  range("memoryLimitMiB", 8, 1024, p.memoryLimitMiB);
  return p;
}

json appearanceProfileJson(const AppearanceProfile& p) {
  json value = json::parse(p.rawObject);
  if (p.invalidField) return value;
  if (!value.is_object()) value = json::object();
  auto color = [&](const char* key, const auto& v) { if (v) value[key] = colorText(*v); else value.erase(key); };
  color("foreground", p.foreground); color("background", p.background); color("cursor", p.cursor);
  if (p.ansi) {
    value["ansi"] = json::array();
    for (auto c : *p.ansi) value["ansi"].push_back(colorText(c));
  } else value.erase("ansi");
  if (p.scrollbackLines) value["scrollbackLines"] = *p.scrollbackLines;
  else value.erase("scrollbackLines");
  if (p.memoryLimitMiB) value["memoryLimitMiB"] = *p.memoryLimitMiB;
  else value.erase("memoryLimitMiB");
  return value;
}
}  // namespace detail

uishell::PaneAppearance builtinAppearance() {
  return {{0xD4D4D4, 0x1E1E1E, 0x3399FF,
           {0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
            0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF}},
          10000, 64ull << 20};
}

AppearanceResolution resolveAppearance(const SettingsFile& settings, const std::string& id) {
  AppearanceResolution out{builtinAppearance(), std::nullopt};
  if (settings.appearanceProfilesRaw) {
    out.warning = "外观 Profile 配置无效：appearanceProfiles，已用默认";
    return out;
  }
  const auto& profiles = settings.appearanceProfiles;
  if (auto d = profiles.find(kDefaultId); d != profiles.end()) {
    if (d->second.invalidField) out.warning = invalidWarning(kDefaultId, *d->second.invalidField);
    else overlay(out.appearance, d->second);
  }
  if (id == kDefaultId) return out;
  auto it = profiles.find(id);
  if (it == profiles.end()) out.warning = "外观 Profile「" + id + "」不存在，已用默认";
  else if (it->second.invalidField) out.warning = invalidWarning(id, *it->second.invalidField);
  else overlay(out.appearance, it->second);
  return out;
}

std::string appearanceProfileIdFor(const ConnectionsFile& file, const std::string& connectionId) {
  if (!connectionId.empty()) for (const auto& c : file.connections)
    if (c.id == connectionId) return c.appearanceProfileId.empty() ? kDefaultId : c.appearanceProfileId;
  return kDefaultId;
}

std::optional<std::string> applyImportedColors(SettingsFile& settings, const std::string& id, const uishell::PaneColors& colors) {
  auto& p = settings.appearanceProfiles[id];
  auto value = detail::appearanceProfileJson(p);
  if (!value.is_object()) value = json::object();
  value["foreground"] = colorText(colors.foreground);
  value["background"] = colorText(colors.background);
  value["cursor"] = colorText(colors.cursor);
  value["ansi"] = json::array();
  for (auto c : colors.ansi) value["ansi"].push_back(colorText(c));
  p = detail::parseAppearanceProfile(id, value);
  // 导入有意替换非对象的 appearanceProfiles 原始值。
  settings.appearanceProfilesRaw.reset();
  return p.invalidField;
}

void clearProfileColors(SettingsFile& settings, const std::string& id) {
  auto it = settings.appearanceProfiles.find(id);
  if (it == settings.appearanceProfiles.end()) return;
  auto value = detail::appearanceProfileJson(it->second);
  if (!value.is_object()) return;
  for (const auto* key : {"foreground", "background", "cursor", "ansi"}) value.erase(key);
  if (value.empty()) settings.appearanceProfiles.erase(it);
  else it->second = detail::parseAppearanceProfile(id, value);
}
}  // namespace uicfg
