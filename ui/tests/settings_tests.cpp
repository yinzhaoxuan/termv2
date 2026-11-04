#include <cstdio>
#include <fstream>
#include "uicfg/settings.h"
#include "uicfg/shell_logic.h"
#include <nlohmann/json.hpp>
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  auto d = defaultSettings();
  CHECK(d.keybindings.at("newTab") == "Ctrl+Shift+T");
  CHECK(d.keybindings.at("copy") == "Ctrl+Shift+C");
  CHECK(validateSettings(d).empty());
  CHECK(d.keybindings.at("moveTabLeft") == "Ctrl+Shift+PageUp");
  CHECK(d.keybindings.at("moveTabRight") == "Ctrl+Shift+PageDown");
  {  // 规范化后也不能有两个默认绑定落在同一组合键上（validateSettings 只比字符串）
    std::vector<std::pair<Chord, std::string>> seen;
    for (auto& [a, c] : d.keybindings) {
      auto ch = parseChord(c); CHECK(ch.has_value()); if (!ch) continue;
      for (auto& [o, oa] : seen) if (o == *ch) { std::printf("FAIL chord clash %s / %s\n", a.c_str(), oa.c_str()); ++fails; }
      seen.push_back({*ch, a});
    }
    CHECK(matchKey(d.keybindings, {true, true, false, "PageUp"}).action == "moveTabLeft");
    CHECK(matchKey(d.keybindings, {true, true, false, "PageDown"}).action == "moveTabRight");
    CHECK(matchKey(d.keybindings, {false, true, false, "PageUp"}).action == "scrollPageUp");  // 滚动键不受影响
    CHECK(matchKey(d.keybindings, {false, true, false, "PageDown"}).action == "scrollPageDown");
  }

  SettingsFile clash = d;
  clash.keybindings["paste"] = clash.keybindings["copy"];  // 冲突
  auto conflicts = validateSettings(clash);
  CHECK(conflicts.size() == 1);
  if (!conflicts.empty()) {
    CHECK(conflicts.front().field == "keybindings");
    CHECK(conflicts.front().message == "快捷键冲突: Ctrl+Shift+C 同时绑定 copy 与 paste");
  }

  fs::path dir = fs::temp_directory_path() / "uicfg_settings_test";
  fs::remove_all(dir); fs::create_directories(dir);
  fs::path f = dir / "settings.json";
  CHECK(loadSettings(f).status == LoadStatus::NotFound);
  CHECK(loadSettings(f).data.keybindings.count("nextTab"));

  std::string err;
  SettingsFile s = defaultSettings();
  s.keybindings["newTab"] = "Ctrl+T";
  CHECK(saveSettings(f, s, false, &err) && s.revision == 1);
  auto r = loadSettings(f);
  CHECK(r.status == LoadStatus::Ok && r.data.keybindings.at("newTab") == "Ctrl+T");
  CHECK(r.data.keybindings.at("closePane") == "Ctrl+Shift+W");  // 缺省补齐

  CHECK(!r.data.wordChars);  // 未设置 = 后端默认
  r.data.wordChars = "/-_.";
  CHECK(saveSettings(f, r.data, false, &err));
  { auto r2 = loadSettings(f); CHECK(r2.data.wordChars && *r2.data.wordChars == "/-_."); }
  CHECK(r.data.keybindings.at("pasteCtrlV") == "Ctrl+V");

  {  // ADR-004：多个空绑定合法，保存和重载均保留，不补回默认快捷键
    SettingsFile unbound = r.data;
    unbound.keybindings["copyOrInterrupt"] = "";
    unbound.keybindings["pasteCtrlV"] = "";
    CHECK(validateSettings(unbound).empty());
    CHECK(saveSettings(f, unbound, false, &err));
    {
      std::ifstream in(f);
      auto saved = nlohmann::json::parse(in);
      CHECK(saved.at("keybindings").at("copyOrInterrupt") == "");
      CHECK(saved.at("keybindings").at("pasteCtrlV") == "");
    }
    auto reloaded = loadSettings(f);
    CHECK(reloaded.status == LoadStatus::Ok);
    CHECK(reloaded.data.keybindings.at("copyOrInterrupt").empty());
    CHECK(reloaded.data.keybindings.at("pasteCtrlV").empty());
  }

  // 字体族
  CHECK(presetFontFamilies().size() == 3 && presetFontFamilies()[0] == d.fontFamily);
  CHECK(normalizeFontFamily("  Consolas ") == std::string("Consolas"));
  CHECK(normalizeFontFamily("更纱黑体 Mono SC") == std::string("更纱黑体 Mono SC"));
  CHECK(!normalizeFontFamily("") && !normalizeFontFamily("   ") && !normalizeFontFamily("A\tB"));
  CHECK(normalizeFontFamily(std::string(31, 'x')) && !normalizeFontFamily(std::string(32, 'x')));
  {
    SettingsFile ff = r.data; ff.fontFamily = "Consolas";
    CHECK(saveSettings(f, ff, false, &err));
    CHECK(loadSettings(f).data.fontFamily == "Consolas");
    ff.fontFamily = "";
    CHECK(!validateSettings(ff).empty() && !saveSettings(f, ff, false, &err));
    CHECK(loadSettings(f).data.fontFamily == "Consolas");  // 非法值不写回
  }
  {  // ChooseFont 的小数磅值：校验通过、存取不丢小数
    SettingsFile fs = r.data; fs.fontFamily = "Consolas"; fs.fontSize = 10.5;
    CHECK(validateSettings(fs).empty() && saveSettings(f, fs, false, &err));
    CHECK(loadSettings(f).data.fontSize == 10.5);
  }

  {  // osc52 隐藏字段
    auto write = [&](const std::string& body) { std::ofstream o(f, std::ios::binary | std::ios::trunc); o << body; };
    auto fileHas = [&](const char* key) { std::ifstream i(f); std::string t((std::istreambuf_iterator<char>(i)), {}); return t.find(key) != std::string::npos; };
    CHECK(!d.osc52 && d.osc52OrDefault() == Osc52Settings{});  // 默认全关
    CHECK(!d.osc52OrDefault().allowWrite && !d.osc52OrDefault().allowRead && d.osc52OrDefault().maxBytes == 0);
    // 缺省：不读出、不写出（保持隐藏）
    SettingsFile s0 = defaultSettings();
    CHECK(saveSettings(f, s0, false, &err) && !fileHas("osc52"));
    CHECK(!loadSettings(f).data.osc52);
    // 读写往返
    write(R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":false,"maxBytes":100000}})");
    auto o = loadSettings(f);
    CHECK(o.status == LoadStatus::Ok && o.data.osc52 && o.data.osc52->allowWrite && !o.data.osc52->allowRead && o.data.osc52->maxBytes == 100000);
    CHECK(saveSettings(f, o.data, false, &err) && fileHas("osc52"));
    { auto o2 = loadSettings(f); CHECK(o2.data.osc52 && *o2.data.osc52 == *o.data.osc52); }
    // maxBytes 原样（超过 768 KiB 也不在 UI 钳）
    write(R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":true,"maxBytes":4000000000}})");
    { auto o3 = loadSettings(f); CHECK(o3.data.osc52 && o3.data.osc52->allowRead && o3.data.osc52->maxBytes == 4000000000u); }
    CHECK(!o.data.osc52Invalid());
    // 任一字段缺失 / 类型不对 → 默认全关；保存时原样写回（不覆盖手写内容）
    const char* bad[] = {
        R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":true}})",
        R"({"schemaVersion":1,"osc52":{"allowWrite":"yes","allowRead":true,"maxBytes":0}})",
        R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":true,"maxBytes":-1}})",
        R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":true,"maxBytes":1.5}})",
        R"({"schemaVersion":1,"osc52":{"allowWrite":true,"allowRead":true,"maxBytes":4294967296}})",
        R"({"schemaVersion":1,"osc52":true})",
        R"({"schemaVersion":1,"osc52":null})",
    };
    for (const char* b : bad) {
      write(b);
      auto rb = loadSettings(f);
      CHECK(rb.status == LoadStatus::Ok && rb.data.osc52 && *rb.data.osc52 == Osc52Settings{});
      CHECK(rb.data.osc52Invalid() && rb.data.osc52OrDefault() == Osc52Settings{});
      const std::string raw = nlohmann::json::parse(b)["osc52"].dump();
      CHECK(rb.data.osc52Raw && *rb.data.osc52Raw == raw);
      CHECK(saveSettings(f, rb.data, false, &err));                     // 往返
      std::ifstream in(f); auto saved = nlohmann::json::parse(in);
      CHECK(saved.contains("osc52") && saved["osc52"] == nlohmann::json::parse(raw));  // 原样
      auto rb2 = loadSettings(f);
      CHECK(rb2.data.osc52Raw && *rb2.data.osc52Raw == raw && rb2.data.osc52OrDefault() == Osc52Settings{});
    }
  }

  {  // 多行粘贴确认：缺省启用，隐藏字段仅在存在时写回
    auto write = [&](const nlohmann::json& j) { std::ofstream o(f, std::ios::binary | std::ios::trunc); o << j; };
    auto read = [&] { std::ifstream in(f); return nlohmann::json::parse(in); };
    CHECK(!d.confirmMultilinePaste && !d.confirmMultilinePasteRaw && d.confirmMultilinePasteOrDefault());
    write({{"schemaVersion", 1}});
    auto missing = loadSettings(f);
    CHECK(missing.status == LoadStatus::Ok && !missing.data.confirmMultilinePaste && missing.data.confirmMultilinePasteOrDefault());
    CHECK(saveSettings(f, missing.data, false, &err) && !read().contains("confirmMultilinePaste"));
    for (bool enabled : {true, false}) {
      write({{"schemaVersion", 1}, {"confirmMultilinePaste", enabled}});
      auto loaded = loadSettings(f);
      CHECK(loaded.status == LoadStatus::Ok && loaded.data.confirmMultilinePaste == std::optional<bool>(enabled));
      CHECK(!loaded.data.confirmMultilinePasteRaw && loaded.data.confirmMultilinePasteOrDefault() == enabled);
      CHECK(saveSettings(f, loaded.data, false, &err) && read().at("confirmMultilinePaste") == enabled);
      CHECK(loadSettings(f).data.confirmMultilinePasteOrDefault() == enabled);
    }
    for (const char* raw : {"null", "0", "1.5", "\"false\"", "[]", "{\"enabled\":false}"}) {
      const auto value = nlohmann::json::parse(raw);
      write({{"schemaVersion", 1}, {"confirmMultilinePaste", value}});
      auto loaded = loadSettings(f);
      CHECK(loaded.status == LoadStatus::Ok && loaded.data.confirmMultilinePaste == std::optional<bool>(true));
      CHECK(loaded.data.confirmMultilinePasteOrDefault() && loaded.data.confirmMultilinePasteRaw == std::optional<std::string>(value.dump()));
      CHECK(saveSettings(f, loaded.data, false, &err) && read().at("confirmMultilinePaste") == value);
      auto again = loadSettings(f);
      CHECK(again.data.confirmMultilinePasteOrDefault() && again.data.confirmMultilinePasteRaw == loaded.data.confirmMultilinePasteRaw);
    }
  }

  resetKeybindings(r.data);
  CHECK(r.data.keybindings.at("newTab") == "Ctrl+Shift+T");

  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  { auto c=uicfg::scrollCommandFor("scrollPageUp",24); if(!(c.isScroll&&c.lines==-23)){std::puts("FAIL scroll page");return 1;}
  auto b=uicfg::scrollCommandFor("scrollToBottom",24); if(!(b.isScroll&&b.toBottom)){std::puts("FAIL scroll bottom");return 1;}
  if(uicfg::scrollCommandFor("copy",24).isScroll){std::puts("FAIL scroll copy");return 1;}
  if(!uicfg::validateSettings(uicfg::defaultSettings()).empty()){std::puts("FAIL default conflicts");return 1;} }
  return fails != 0;
}
