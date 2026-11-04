// SPDX-License-Identifier: GPL-3.0-or-later
// DS-16：快捷键只读展示，配置解释由 uicfg 提供。
#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>
#include "uicfg/keybinding_view.h"

void runKeybindingDialog(HWND owner, HINSTANCE inst,
                         const std::vector<uicfg::KeybindingRow>& rows,
                         const std::vector<uicfg::FixedKey>& fixed,
                         const std::filesystem::path& configDir);
