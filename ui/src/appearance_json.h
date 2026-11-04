// settings 与导入共用的 JSON 规则，不作为公共接口。
#pragma once
#include "uicfg/settings.h"
#include <nlohmann/json.hpp>

namespace uicfg::detail {
AppearanceProfile parseAppearanceProfile(const std::string& id, const nlohmann::json& value);
nlohmann::json appearanceProfileJson(const AppearanceProfile& profile);
}  // namespace uicfg::detail
