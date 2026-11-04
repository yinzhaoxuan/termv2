#pragma once
#include <string>
#include <vector>
#include "uicfg/settings.h"

namespace uicfg {

enum class BindingState { Default, Changed, Unbound, Conflict, Invalid, UnknownAction };
struct KeybindingRow {
  std::string action;
  std::string label;
  std::string chord;  // 显示文本；未绑定为空
  BindingState state;
  std::string note;
};

// 已知动作按标签、窗格、剪贴板、搜索、字体、滚动排序；未知动作按 ID 排在最后。
std::vector<KeybindingRow> keybindingRows(const SettingsFile&);

struct FixedKey { std::string keys, label; };
const std::vector<FixedKey>& fixedKeys();

}  // namespace uicfg
