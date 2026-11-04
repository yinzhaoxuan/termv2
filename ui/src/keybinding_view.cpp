#include "uicfg/keybinding_view.h"
#include "uicfg/shell_logic.h"
#include <algorithm>
#include <iterator>

namespace uicfg {
namespace {

struct ActionLabel { const char* action; const char* label; };
constexpr ActionLabel kActions[] = {
    {"newTab", "新建标签"},
    {"nextTab", "下一个标签"},
    {"prevTab", "上一个标签"},
    {"moveTabLeft", "标签左移"},
    {"moveTabRight", "标签右移"},
    {"closePane", "关闭窗格"},
    {"splitVertical", "垂直分割"},
    {"splitHorizontal", "水平分割"},
    {"focusPane", "切换窗格"},
    {"copy", "复制"},
    {"paste", "粘贴"},
    {"pasteCtrlV", "粘贴（Ctrl+V）"},
    {"pasteShiftInsert", "粘贴（Shift+Insert）"},
    {"copyOrInterrupt", "复制或中断"},
    {"searchHistory", "搜索历史"},
    {"searchConnections", "搜索连接"},
    {"chooseFont", "选择字体"},
    {"fontBigger", "增大字号"},
    {"fontSmaller", "减小字号"},
    {"fontReset", "重置字号"},
    {"scrollPageUp", "向上翻页"},
    {"scrollPageDown", "向下翻页"},
    {"scrollLineUp", "向上滚动一行"},
    {"scrollLineDown", "向下滚动一行"},
    {"scrollToTop", "滚动到顶部"},
    {"scrollToBottom", "滚动到底部"},
};

const ActionLabel* findAction(const std::string& action) {
  const auto it = std::find_if(std::begin(kActions), std::end(kActions),
      [&](const ActionLabel& entry) { return action == entry.action; });
  return it == std::end(kActions) ? nullptr : it;
}

std::string actionLabel(const std::string& action) {
  const auto* entry = findAction(action);
  return entry ? entry->label : action;
}

std::string displayChord(const std::string& action, const std::string& chord) {
  return action == "focusPane" && chord == "Alt+Arrow" ? "Alt+方向键" : chord;
}

KeybindingRow knownRow(const std::string& action,
                      const std::map<std::string, std::string>& bindings,
                      const std::map<std::string, std::string>& defaults) {
  const auto& chord = bindings.at(action);
  KeybindingRow row{action, actionLabel(action), displayChord(action, chord), BindingState::Default, "默认"};
  if (chord.empty()) {
    row.state = BindingState::Unbound;
    row.note = "未绑定";
  } else if (!parseChord(chord)) {
    row.state = BindingState::Invalid;
    row.note = "无法识别的快捷键";
  } else {
    // 与 validateSettings 一致：只比较原始非空字符串，未知动作也参与冲突检查。
    std::string peers;
    for (const auto& [other, text] : bindings) {
      if (other == action || text != chord) continue;
      if (!peers.empty()) peers += "、";
      peers += actionLabel(other);
    }
    if (!peers.empty()) {
      row.state = BindingState::Conflict;
      row.note = "与 " + peers + " 冲突";
    } else {
      const auto it = defaults.find(action);
      if (it == defaults.end() || chord != it->second) {
        row.state = BindingState::Changed;
        row.note = "已修改（默认 " + (it == defaults.end() ? std::string("未绑定") : displayChord(action, it->second)) + "）";
      }
    }
  }
  return row;
}

}  // namespace

std::vector<KeybindingRow> keybindingRows(const SettingsFile& settings) {
  const auto defaults = defaultSettings().keybindings;
  auto bindings = defaults;
  for (const auto& [action, chord] : settings.keybindings) bindings[action] = chord;
  std::vector<KeybindingRow> rows;
  for (const auto& entry : kActions) {
    bindings.try_emplace(entry.action, "");  // 无默认绑定的动作仍需展示。
  }
  for (const auto& entry : kActions) rows.push_back(knownRow(entry.action, bindings, defaults));
  for (const auto& [action, chord] : settings.keybindings) {
    if (!findAction(action))
      rows.push_back({action, action, chord, BindingState::UnknownAction, "未知动作（忽略）"});
  }
  return rows;
}

const std::vector<FixedKey>& fixedKeys() {
  static const std::vector<FixedKey> keys{
      {"Enter / 双击", "会话树：连接"},
      {"F2", "会话树：编辑连接 / 重命名分组"},
      {"Delete", "会话树：删除连接 / 分组"},
      {"Shift+F10 / 菜单键", "会话树：打开右键菜单"},
      {"Shift+拖动", "终端：程序开启鼠标上报时强制本地选择"},
  };
  return keys;
}

}  // namespace uicfg
