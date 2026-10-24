#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/store.h"

namespace uicfg {

// settings.json：快捷键与外观默认值（第 12.4 节）；允许覆盖和恢复默认
struct SettingsFile {
  int schemaVersion = kSchemaVersion;
  long long revision = 0;
  // action id → 快捷键描述（如 "Ctrl+Shift+T"）
  std::map<std::string, std::string> keybindings;
  double fontSize = 12.0;
  std::string fontFamily = "Cascadia Mono";
  // 双击选词的额外词字符（ASCII 可见标点）；空 = 后端默认 /-+\~_.；新建窗格生效
  std::optional<std::string> wordChars;
};

struct SettingsLoadResult {
  LoadStatus status = LoadStatus::NotFound;
  SettingsFile data;
  bool readOnly = false;
  std::string detail;
};

// 第 12.4 建议默认值
SettingsFile defaultSettings();

SettingsLoadResult loadSettings(const std::filesystem::path& file);
bool saveSettings(const std::filesystem::path& file, SettingsFile& data,
                  bool readOnly, std::string* error);

// 字体族：菜单里列出的常见等宽字体（顺序即显示顺序）
const std::vector<std::string>& presetFontFamilies();
// 用户输入的字体族 → 规范化（去首尾空白）；空、含控制字符或超过 31 个字符（LOGFONT 上限）返回 nullopt
std::optional<std::string> normalizeFontFamily(const std::string& input);

// 恢复全部快捷键为默认；外观字段不动
void resetKeybindings(SettingsFile& data);

// 应用内部冲突：同一快捷键绑了多个动作则报错（不含系统保留键）
std::vector<ValidationError> validateSettings(const SettingsFile& data);

// 历史滚动动作 → PaneBackend::scrollViewport(lines) / scrollToBottom()
struct ScrollCommand { bool isScroll; bool toBottom; int lines; };
ScrollCommand scrollCommandFor(const std::string& action, int visibleRows);

}  // namespace uicfg
