// DS-15：诊断包收集与脱敏；不改变原始配置和日志。
#pragma once
#include "uicfg/settings.h"
#include <cstddef>
#include <string_view>
#include <utility>

namespace uicfg {
struct DiagEnvironment {
  std::string generatedAt, commit, build, windowsVersion;
  std::vector<std::pair<std::string, std::string>> envFlags;
};
std::vector<std::string> configSummaryLines(const ConnectionsFile&, LoadStatus, const FileValidation&,
                                          const SettingsFile&, LoadStatus);
struct DiagLogSection {
  std::string name;
  std::optional<std::string> text;
  std::string note;
};
inline constexpr std::size_t kDiagLogTailBytes = 5u << 20;
std::vector<DiagLogSection> readDiagLogs(const std::filesystem::path& logsDir,
                                         std::size_t maxBytes = kDiagLogTailBytes);
struct RedactionContext {
  std::vector<std::pair<std::string, std::string>> known;  // 类别 host / user / vm / path，原值
};
RedactionContext redactionContextFrom(const ConnectionsFile&,
    const std::vector<std::pair<std::string, std::string>>& extra);
// 少于 3 个字符的已知值不精确替换；模式规则仍生效，发送前需预览检查。
std::string redactDiagnosticText(std::string_view, const RedactionContext&);
std::string buildDiagnosticBundle(const DiagEnvironment&, const std::vector<std::string>& summary,
                                const std::vector<DiagLogSection>&, const RedactionContext&);
// 同目录临时文件落盘后原子替换；失败时清理临时文件。
bool saveDiagnosticBundle(const std::filesystem::path&, const std::string& text, std::string* error);
std::string diagnosticFileName(const std::string& stamp);
}  // namespace uicfg
