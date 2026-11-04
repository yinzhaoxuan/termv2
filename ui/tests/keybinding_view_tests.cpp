#include "uicfg/keybinding_view.h"
#include <algorithm>
#include <cstdio>

using namespace uicfg;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static KeybindingRow rowFor(const SettingsFile& settings, const std::string& action) {
  const auto rows = keybindingRows(settings);
  const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.action == action; });
  CHECK(it != rows.end());
  return it == rows.end() ? KeybindingRow{} : *it;
}

static void defaults() {
  const auto settings = defaultSettings();
  const auto original = settings.keybindings;
  const auto rows = keybindingRows(settings);
  const std::vector<std::string> order{
      "newTab", "nextTab", "prevTab", "moveTabLeft", "moveTabRight",
      "closePane", "splitVertical", "splitHorizontal", "focusPane",
      "copy", "paste", "pasteCtrlV", "pasteShiftInsert", "copyOrInterrupt",
      "searchHistory", "searchConnections", "chooseFont", "fontBigger", "fontSmaller", "fontReset",
      "scrollPageUp", "scrollPageDown", "scrollLineUp", "scrollLineDown", "scrollToTop", "scrollToBottom"};
  CHECK(rows.size() == order.size());
  size_t defaultCount = 0;
  for (size_t i = 0; i < rows.size() && i < order.size(); ++i) {
    const auto& row = rows[i];
    CHECK(row.action == order[i]);
    if (row.action == "chooseFont") {
      CHECK(row.state == BindingState::Unbound && row.chord.empty() && row.note == "未绑定");
    } else {
      CHECK(row.state == BindingState::Default && row.note == "默认");
      ++defaultCount;
    }
  }
  CHECK(defaultCount == 25);
  CHECK(settings.keybindings == original);  // 展示不修改设置，也不添加 chooseFont。
  CHECK(rowFor(settings, "focusPane").chord == "Alt+方向键");
  // 直接构造的稀疏设置同样补齐缺省动作。
  CHECK(keybindingRows(SettingsFile{}).size() == rows.size());
  CHECK(rowFor(SettingsFile{}, "newTab").state == BindingState::Default);
}

static void changed() {
  auto settings = defaultSettings();
  settings.keybindings["newTab"] = "Ctrl+T";
  const auto row = rowFor(settings, "newTab");
  CHECK(row.state == BindingState::Changed && row.chord == "Ctrl+T");
  CHECK(row.note == "已修改（默认 Ctrl+Shift+T）");
  CHECK(rowFor(settings, "closePane").state == BindingState::Default);
  settings.keybindings["focusPane"] = "Ctrl+Arrow";
  CHECK(rowFor(settings, "focusPane").note == "已修改（默认 Alt+方向键）");
  CHECK(rowFor(settings, "focusPane").chord == "Ctrl+Arrow");
  settings.keybindings["chooseFont"] = "Ctrl+F1";
  CHECK(rowFor(settings, "chooseFont").state == BindingState::Changed);
  CHECK(rowFor(settings, "chooseFont").note == "已修改（默认 未绑定）");
}

static void unbound() {
  auto settings = defaultSettings();
  for (const char* action : {"copy", "paste", "focusPane", "chooseFont"}) settings.keybindings[action] = "";
  for (const char* action : {"copy", "paste", "focusPane", "chooseFont"}) {
    const auto row = rowFor(settings, action);
    CHECK(row.state == BindingState::Unbound && row.chord.empty() && row.note == "未绑定");
  }
  CHECK(validateSettings(settings).empty());
}

static void invalid() {
  auto settings = defaultSettings();
  for (const char* chord : {"Ctrl+Bogus+", "Ctrl+Bogus", "   "}) {
    settings.keybindings["copy"] = chord;
    settings.keybindings["paste"] = chord;  // 无效优先于冲突。
    for (const char* action : {"copy", "paste"}) {
      const auto row = rowFor(settings, action);
      CHECK(row.state == BindingState::Invalid && row.chord == chord && row.note == "无法识别的快捷键");
    }
  }
}

static void conflicts() {
  auto settings = defaultSettings();
  settings.keybindings["paste"] = settings.keybindings.at("copy");
  CHECK(!validateSettings(settings).empty());
  CHECK(rowFor(settings, "copy").state == BindingState::Conflict);
  CHECK(rowFor(settings, "paste").state == BindingState::Conflict);
  CHECK(rowFor(settings, "copy").note == "与 粘贴 冲突");
  CHECK(rowFor(settings, "paste").note == "与 复制 冲突");
  settings.keybindings["newTab"] = settings.keybindings.at("copy");
  CHECK(rowFor(settings, "copy").note.find("新建标签") != std::string::npos);
  CHECK(rowFor(settings, "copy").note.find("粘贴") != std::string::npos);
  CHECK(rowFor(settings, "newTab").state == BindingState::Conflict);
  // 校验按原串比较，不把大小写、别名或方向通配符合并。
  settings = defaultSettings();
  settings.keybindings["paste"] = "control+shift+c";
  settings.keybindings["newTab"] = "Alt+Left";
  CHECK(validateSettings(settings).empty());
  CHECK(rowFor(settings, "copy").state == BindingState::Default);
  CHECK(rowFor(settings, "paste").state == BindingState::Changed);
  CHECK(rowFor(settings, "focusPane").state == BindingState::Default);
}

static void unknown() {
  auto settings = defaultSettings();
  settings.keybindings["zFuture"] = "";
  settings.keybindings["aFuture"] = "Ctrl+Bogus+";
  auto rows = keybindingRows(settings);
  CHECK(rows.size() == 28);
  CHECK(rows[rows.size() - 2].action == "aFuture" && rows.back().action == "zFuture");
  for (const char* action : {"aFuture", "zFuture"}) {
    const auto row = rowFor(settings, action);
    CHECK(row.state == BindingState::UnknownAction && row.label == action && row.note == "未知动作（忽略）");
    CHECK(row.chord == settings.keybindings.at(action));
  }
  settings.keybindings["aFuture"] = settings.keybindings.at("copy");
  CHECK(!validateSettings(settings).empty());
  CHECK(rowFor(settings, "aFuture").state == BindingState::UnknownAction);
  CHECK(rowFor(settings, "copy").state == BindingState::Conflict);
  CHECK(rowFor(settings, "copy").note == "与 aFuture 冲突");
}

static void coverage() {
  const auto settings = defaultSettings();
  const auto rows = keybindingRows(settings);
  for (const auto& [action, chord] : settings.keybindings) {
    const auto count = std::count_if(rows.begin(), rows.end(), [&](const auto& row) { return row.action == action; });
    CHECK(count == 1);
    const auto row = rowFor(settings, action);
    CHECK(!row.label.empty() && row.label != action);
    CHECK(row.state == BindingState::Default);
    CHECK(row.chord == (action == "focusPane" ? "Alt+方向键" : chord));
  }
}

static void fixed() {
  const auto& keys = fixedKeys();
  CHECK(!keys.empty());
  for (const char* key : {"F2", "Delete", "Enter / 双击", "Shift+F10 / 菜单键", "Shift+拖动"})
    CHECK(std::any_of(keys.begin(), keys.end(), [&](const auto& entry) { return entry.keys == key && !entry.label.empty(); }));
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "defaults") defaults();
  else if (mode == "changed") changed();
  else if (mode == "unbound") unbound();
  else if (mode == "invalid") invalid();
  else if (mode == "conflicts") conflicts();
  else if (mode == "unknown") unknown();
  else if (mode == "coverage") coverage();
  else if (mode == "fixed") fixed();
  else return 2;
  return fails != 0;
}
