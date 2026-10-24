#pragma once
#include <filesystem>
#include <map>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/store.h"

namespace uicfg {

// 隐藏字段 osc52（不进界面）→ PaneBackendConfig.osc52（it2app::Osc52Config），新建窗格生效。
// 默认全关；maxBytes 原样传（0 = 核心默认，核心会钳到 768 KiB）
struct Osc52Settings {
  bool allowWrite = false;
  bool allowRead = false;
  std::uint32_t maxBytes = 0;
  bool operator==(const Osc52Settings&) const = default;
};

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
  // 文件里有 osc52 字段才有值（且只在有值时写回，保持隐藏）。字段缺失、任一子字段缺失或类型不对 → 默认全关
  std::optional<Osc52Settings> osc52;
  // osc52 不合法时：原样的 JSON 值（序列化文本），osc52 按全关；保存时原样写回，不覆盖用户手写内容
  std::optional<std::string> osc52Raw;
  bool osc52Invalid() const { return osc52Raw.has_value(); }
  Osc52Settings osc52OrDefault() const { return osc52.value_or(Osc52Settings{}); }
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
