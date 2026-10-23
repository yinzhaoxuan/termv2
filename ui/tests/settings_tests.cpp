#include <cstdio>
#include <fstream>
#include "uicfg/settings.h"
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  auto d = defaultSettings();
  CHECK(d.keybindings.at("newTab") == "Ctrl+Shift+T");
  CHECK(d.keybindings.at("copy") == "Ctrl+Shift+C");
  CHECK(validateSettings(d).empty());

  SettingsFile clash = d;
  clash.keybindings["paste"] = clash.keybindings["copy"];  // 冲突
  CHECK(!validateSettings(clash).empty());

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
