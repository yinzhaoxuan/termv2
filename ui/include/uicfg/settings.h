// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <map>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/store.h"
#include "uishell/pane_backend.h"

namespace uicfg {

// 隐藏字段 osc52（不进界面）→ PaneBackendConfig.osc52（it2app::Osc52Config），新建窗格生效。
// 默认全关；maxBytes 原样传（0 = 核心默认，核心会钳到 768 KiB）
struct Osc52Settings {
  bool allowWrite = false;
  bool allowRead = false;
  std::uint32_t maxBytes = 0;
  bool operator==(const Osc52Settings&) const = default;
};

// ADR-006：没写 ssh.transport 的连接的全局默认（隐藏字段 sshDefaultTransport，不进界面）。
enum class SshDefaultTransport { Auto, Compatible, Direct };
// "auto" / "compatible" / "direct"
const char* sshDefaultTransportName(SshDefaultTransport);

// 字段可选，缺省时继承；rawObject 保留未知键；invalidField 记录首个错误字段，整个 Profile 忽略。
struct AppearanceProfile {
  std::optional<std::uint32_t> foreground, background, cursor;
  std::optional<std::array<std::uint32_t, 16>> ansi;
  std::optional<std::uint32_t> scrollbackLines, memoryLimitMiB;
  std::string rawObject = "{}";  // 保留未知字段；非法值也原样写回
  std::optional<std::string> invalidField;
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
  // 隐藏字段：缺省/类型不对按 true；仅在文件中存在时写回，非法原值保留
  std::optional<bool> confirmMultilinePaste;
  std::optional<std::string> confirmMultilinePasteRaw;
  bool confirmMultilinePasteOrDefault() const { return confirmMultilinePaste.value_or(true); }
  // DS-18 隐藏字段 fontLigatures：缺省或非布尔 = 关；仅在文件中存在时写回，非法原值保留。只影响此后新建的窗格
  std::optional<bool> fontLigatures;
  std::optional<std::string> fontLigaturesRaw;
  bool fontLigaturesOrDefault() const { return fontLigatures.value_or(false); }
  // 隐藏字段 sshDefaultTransport：缺省或非法 = nullopt（用内置默认）；仅在文件中存在时写回，非法原值保留
  std::optional<SshDefaultTransport> sshDefaultTransport;
  std::optional<std::string> sshDefaultTransportRaw;
  bool sshDefaultTransportInvalid() const { return sshDefaultTransportRaw.has_value(); }
  // DS-9：id → 外观 Profile；新建窗格应用全部字段，已打开窗格可实时换色。
  std::map<std::string, AppearanceProfile> appearanceProfiles;
  // 字段存在但不是对象时的原始 JSON（序列化文本）；保存时原样写回。
  std::optional<std::string> appearanceProfilesRaw;
  // 原文件有此字段时，即使为空也写回空对象；原本缺省且仍为空则不写。
  bool appearanceProfilesPresent = false;
};


struct SettingsLoadResult {
  LoadStatus status = LoadStatus::NotFound;
  SettingsFile data;
  bool readOnly = false;
  std::string detail;
};

uishell::PaneAppearance builtinAppearance();
struct AppearanceResolution {
  uishell::PaneAppearance appearance;
  std::optional<std::string> warning;
};
AppearanceResolution resolveAppearance(const SettingsFile&, const std::string& profileId);
std::string appearanceProfileIdFor(const ConnectionsFile&, const std::string& connectionId);
// 只修改颜色，保留资源上限与未知字段；返回剩余非法字段，nullopt 表示配色生效。
std::optional<std::string> applyImportedColors(SettingsFile&, const std::string& profileId, const uishell::PaneColors&);
void clearProfileColors(SettingsFile&, const std::string& profileId);

// 第 12.4 建议默认值
SettingsFile defaultSettings();

SettingsLoadResult loadSettings(const std::filesystem::path& file);
// 启动时可见的加载提示；正常加载/首次启动为空，不改变加载策略或有效权限。
std::string settingsLoadWarning(const SettingsLoadResult& result);
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
