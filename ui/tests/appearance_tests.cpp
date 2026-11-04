// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/settings.h"
#include "cell_style.h"
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace uicfg;
using nlohmann::json;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static void settings() {
  const auto dir = fs::temp_directory_path() / "uicfg_appearance_test";
  fs::remove_all(dir); fs::create_directories(dir);
  const auto file = dir / "settings.json";
  auto load = [&](const json& profiles) {
    { std::ofstream out(file); out << json{{"schemaVersion", 1}, {"appearanceProfiles", profiles}}; }
    auto result = loadSettings(file);
    CHECK(result.status == LoadStatus::Ok && !result.readOnly);
    return result.data;
  };
  auto saved = [&] { std::ifstream in(file); return json::parse(in); };
  std::string error;
  auto d = defaultSettings();
  CHECK(saveSettings(file, d, false, &error));
  CHECK(!saved().contains("appearanceProfiles"));
  CHECK(loadSettings(file).data.appearanceProfiles.empty());
  auto empty = load(json::object());
  CHECK(empty.appearanceProfilesPresent);
  CHECK(saveSettings(file, empty, false, &error) && saved()["appearanceProfiles"] == json::object());
  json ansi = json::array();
  for (int i = 0; i < 16; ++i) ansi.push_back("#aBcDeF");
  const json good{{"foreground", "#aabbcc"}, {"background", "#010203"}, {"cursor", "#FeDcBa"}, {"ansi", ansi},
                  {"scrollbackLines", 200}, {"memoryLimitMiB", 8}, {"future", {{"nested", true}}}};
  auto s = load({{"appearance-default", good}, {"partial", {{"background", "#aAbBcC"}}}});
  auto& p = s.appearanceProfiles.at("appearance-default");
  CHECK(!p.invalidField && p.foreground == 0xaabbcc && p.background == 0x010203 && p.cursor == 0xfedcba);
  CHECK(p.ansi && (*p.ansi)[15] == 0xabcdef && p.scrollbackLines == 200 && p.memoryLimitMiB == 8);
  CHECK(s.appearanceProfiles.at("partial").background == 0xaabbcc && !s.appearanceProfiles.at("partial").ansi);
  CHECK(saveSettings(file, s, false, &error));
  auto output = saved()["appearanceProfiles"]["appearance-default"];
  CHECK(output["foreground"] == "#AABBCC" && output["cursor"] == "#FEDCBA" && output["ansi"][0] == "#ABCDEF");
  CHECK(output["future"] == good["future"]);
  CHECK(loadSettings(file).data.appearanceProfiles.at("appearance-default").ansi == p.ansi);

  const std::vector<std::pair<std::string, json>> bad{
      {"foreground", "112233"}, {"foreground", "#GG2233"}, {"foreground", "#123"}, {"foreground", 0},
      {"background", false}, {"cursor", nullptr}, {"ansi", json::array({"#112233"})}, {"ansi", "bad"},
      {"scrollbackLines", 99}, {"scrollbackLines", 1000001}, {"scrollbackLines", -1}, {"scrollbackLines", 1.5},
      {"scrollbackLines", "200"}, {"scrollbackLines", true}, {"scrollbackLines", UINT64_MAX},
      {"memoryLimitMiB", 7}, {"memoryLimitMiB", 1025}, {"memoryLimitMiB", -1}, {"memoryLimitMiB", 64.0},
      {"memoryLimitMiB", "64"}, {"memoryLimitMiB", false}, {"memoryLimitMiB", UINT64_MAX}};
  for (const auto& [field, value] : bad) {
    auto invalid = good; invalid[field] = value;
    auto loaded = load({{"bad", invalid}, {"good", good}});
    CHECK(loaded.appearanceProfiles.at("bad").invalidField == field);
    CHECK(!loaded.appearanceProfiles.at("good").invalidField && validateSettings(loaded).empty());
    CHECK(saveSettings(file, loaded, false, &error));
    CHECK(saved()["appearanceProfiles"]["bad"] == invalid);
  }
  for (size_t length : {size_t(15), size_t(17)}) {
    auto a = ansi;
    while (a.size() > length) a.erase(a.size() - 1);
    while (a.size() < length) a.push_back("#112233");
    auto invalid = load({{"bad", {{"ansi", a}}}});
    CHECK(invalid.appearanceProfiles.at("bad").invalidField == "ansi");
  }
  auto a = ansi; a[5] = 7;
  CHECK(load({{"bad", {{"ansi", a}}}}).appearanceProfiles.at("bad").invalidField == "ansi");
  for (const auto& value : std::vector<json>{nullptr, true, 1, "bad", json::array()}) {
    auto raw = load(value);
    CHECK(raw.appearanceProfilesRaw == value.dump() && raw.appearanceProfiles.empty());
    CHECK(resolveAppearance(raw, "prod").warning);
    CHECK(saveSettings(file, raw, false, &error) && saved()["appearanceProfiles"] == value);
    auto profile = load({{"bad", value}});
    CHECK(profile.appearanceProfiles.at("bad").invalidField == "Profile");
    CHECK(saveSettings(file, profile, false, &error) && saved()["appearanceProfiles"]["bad"] == value);
  }
  for (const std::string& id : {std::string{}, std::string(65, 'a'), std::string("a\nb"), std::string("a\x7f")}) {
    auto invalid = load({{id, good}});
    CHECK(invalid.appearanceProfiles.at(id).invalidField == "id");
    CHECK(resolveAppearance(invalid, id).warning && resolveAppearance(invalid, id).warning->find("无效：id") != std::string::npos);
    CHECK(saveSettings(file, invalid, false, &error) && saved()["appearanceProfiles"][id] == good);
  }
  auto bounds = load({{std::string(64, 'a'), {{"scrollbackLines", 100}, {"memoryLimitMiB", 8}}},
                      {"上界", {{"scrollbackLines", 1000000}, {"memoryLimitMiB", 1024}}}});
  for (const auto& [id, profile] : bounds.appearanceProfiles) { (void)id; CHECK(!profile.invalidField); }
  // 原本不存在的隐藏字段：本次导入后再清除，保存时仍可省略。
  auto fresh = defaultSettings();
  applyImportedColors(fresh, "appearance-default", builtinAppearance().colors);
  CHECK(saveSettings(file, fresh, false, &error) && saved().contains("appearanceProfiles"));
  auto existed = loadSettings(file).data;
  clearProfileColors(fresh, "appearance-default");
  CHECK(saveSettings(file, fresh, false, &error) && !saved().contains("appearanceProfiles"));
  // 原文件有这个字段时，即使清空也保留空对象。
  clearProfileColors(existed, "appearance-default");
  CHECK(saveSettings(file, existed, false, &error) && saved()["appearanceProfiles"] == json::object());
  auto malformed = load("bad");
  applyImportedColors(malformed, "appearance-default", builtinAppearance().colors);
  CHECK(!malformed.appearanceProfilesRaw && !resolveAppearance(malformed, "appearance-default").warning);
  CHECK(saveSettings(file, malformed, false, &error) && saved()["appearanceProfiles"].is_object());
  fs::remove_all(dir);
}

static void resolution() {
  auto s = defaultSettings();
  const auto builtin = builtinAppearance();
  auto r = resolveAppearance(s, "appearance-default");
  CHECK(r.appearance.colors == builtin.colors && !r.warning);
  CHECK(builtin.scrollbackLines == 10000 && builtin.memoryLimitBytes == (64ull << 20));
  // renderer/layout/cell_style.h 的无快照兜底必须与唯一的内置配置色一致。
  const itren::Theme theme;
  CHECK(theme.fg == builtin.colors.foreground && theme.bg == builtin.colors.background && theme.cursor == builtin.colors.cursor);
  for (size_t i = 0; i < 16; ++i) CHECK(theme.palette[i] == builtin.colors.ansi[i]);
  r = resolveAppearance(s, "missing");
  CHECK(r.appearance.colors == builtin.colors && r.warning && r.warning->find("missing」不存在") != std::string::npos);
  auto colors = builtin.colors; colors.ansi.fill(0x123456); colors.foreground = 0xabcdef;
  applyImportedColors(s, "appearance-default", colors);
  auto& prod = s.appearanceProfiles["prod"];
  prod.background = 0x2b0000; prod.scrollbackLines = 200; prod.memoryLimitMiB = 8;
  r = resolveAppearance(s, "prod");
  CHECK(!r.warning && r.appearance.colors.background == 0x2b0000 && r.appearance.colors.ansi == colors.ansi);
  CHECK(r.appearance.colors.foreground == colors.foreground && r.appearance.scrollbackLines == 200 && r.appearance.memoryLimitBytes == (8ull << 20));
  auto fallback = resolveAppearance(s, "absent");
  CHECK(fallback.warning && fallback.appearance.colors == colors);
  prod.invalidField = "memoryLimitMiB";
  r = resolveAppearance(s, "prod");
  CHECK(r.appearance.colors == colors && r.appearance.scrollbackLines == builtin.scrollbackLines && r.warning);
  CHECK(r.warning->find("memoryLimitMiB") != std::string::npos);
  s.appearanceProfiles["appearance-default"].invalidField = "ansi";
  r = resolveAppearance(s, "appearance-default");
  CHECK(r.appearance.colors == builtin.colors && r.warning);
  prod.invalidField.reset();
  r = resolveAppearance(s, "prod");
  CHECK(r.appearance.colors.background == *prod.background && r.appearance.colors.ansi == builtin.colors.ansi);
  ConnectionsFile conns;
  Connection c; c.id = "ssh"; c.appearanceProfileId = "prod"; conns.connections.push_back(c);
  CHECK(appearanceProfileIdFor(conns, "") == "appearance-default");
  CHECK(appearanceProfileIdFor(conns, "absent") == "appearance-default");
  CHECK(appearanceProfileIdFor(conns, "ssh") == "prod");
  conns.connections.front().appearanceProfileId.clear();
  CHECK(appearanceProfileIdFor(conns, "ssh") == "appearance-default");
}

static void mutation() {
  auto s = defaultSettings();
  auto colors = builtinAppearance().colors;
  colors.foreground = 0x123456;
  CHECK(!applyImportedColors(s, "new", colors));
  CHECK(s.appearanceProfiles.count("new") && resolveAppearance(s, "new").appearance.colors == colors);
  clearProfileColors(s, "absent");
  clearProfileColors(s, "new");
  CHECK(s.appearanceProfiles.empty() && !s.appearanceProfilesPresent);
  auto& p = s.appearanceProfiles["new"];
  p.scrollbackLines = 200; p.memoryLimitMiB = 16; p.rawObject = R"({"future":{"enabled":true}})";
  CHECK(!applyImportedColors(s, "new", colors));
  CHECK(s.appearanceProfiles.at("new").scrollbackLines == 200 && s.appearanceProfiles.at("new").memoryLimitMiB == 16);
  clearProfileColors(s, "new");
  CHECK(!s.appearanceProfiles.at("new").foreground && !s.appearanceProfiles.at("new").ansi);
  CHECK(resolveAppearance(s, "new").appearance.scrollbackLines == 200);
  CHECK(json::parse(s.appearanceProfiles.at("new").rawObject)["future"]["enabled"] == true);
  auto& unknown = s.appearanceProfiles["unknown"];
  unknown.rawObject = R"({"future":[]})";
  applyImportedColors(s, "unknown", colors); clearProfileColors(s, "unknown");
  CHECK((s.appearanceProfiles.count("unknown") && json::parse(s.appearanceProfiles.at("unknown").rawObject) == json{{"future", json::array()}}));
  // 清颜色可修复颜色字段损坏，未知字段仍留着。
  auto& invalid = s.appearanceProfiles["invalid"];
  invalid.rawObject = R"({"foreground":false,"future":3})"; invalid.invalidField = "foreground";
  clearProfileColors(s, "invalid");
  CHECK(!s.appearanceProfiles.at("invalid").invalidField);
  CHECK(json::parse(s.appearanceProfiles.at("invalid").rawObject)["future"] == 3);
  // 非颜色字段仍非法：颜色写入并保留原值，但整个 Profile 继续回退。
  for (const auto& [field, value] : std::vector<std::pair<std::string, json>>{
           {"scrollbackLines", 5}, {"memoryLimitMiB", "unknown"}}) {
    auto invalidSettings = defaultSettings();
    auto& profile = invalidSettings.appearanceProfiles["appearance-default"];
    profile.rawObject = json{{field, value}, {"future", 3}}.dump();
    profile.invalidField = field;
    CHECK(applyImportedColors(invalidSettings, "appearance-default", colors) == field);
    CHECK(profile.invalidField == field && profile.foreground == colors.foreground && profile.ansi == colors.ansi);
    const auto raw = json::parse(profile.rawObject);
    CHECK(raw[field] == value && raw["future"] == 3 && raw["foreground"] == "#123456");
    const auto resolved = resolveAppearance(invalidSettings, "appearance-default");
    CHECK(resolved.appearance.colors == builtinAppearance().colors);
    CHECK(resolved.warning && resolved.warning->find(field) != std::string::npos);
  }
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "settings") settings();
  else if (mode == "resolution") resolution();
  else if (mode == "mutation") mutation();
  else return 2;
  return fails != 0;
}
