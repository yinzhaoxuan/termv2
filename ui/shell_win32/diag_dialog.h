// SPDX-License-Identifier: GPL-3.0-or-later
// DS-15：只读预览；true 表示用户选择保存。
#pragma once
#include <windows.h>
#include <string>
bool runDiagPreview(HWND owner, HINSTANCE inst, const std::string& text);
