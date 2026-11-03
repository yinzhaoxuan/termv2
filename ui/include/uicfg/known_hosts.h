#pragma once
#include "uishell/pane_backend.h"
#include <filesystem>
#include <optional>
#include <string>

namespace uicfg {

std::optional<std::string> knownHostsLine(const uishell::HostKeyOffer&);
// line 来自 knownHostsLine；整文件原子替换，完全相同的行不重复写入。
bool appendKnownHost(const std::filesystem::path& file, const std::string& line, std::string* error);

}  // namespace uicfg
