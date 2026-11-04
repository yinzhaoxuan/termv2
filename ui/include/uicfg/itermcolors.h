// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "uishell/pane_backend.h"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uicfg {
inline constexpr std::size_t kItermColorsMaxBytes = 1u << 20;
struct ItermColorsImport {
  std::optional<uishell::PaneColors> colors;
  std::string error;
  std::vector<std::string> missing, ignored;
  bool approximated = false;
};
ItermColorsImport importItermColors(std::string_view fileBytes);
std::string itermImportPrompt(const ItermColorsImport&, const std::string& fileName);
}  // namespace uicfg
