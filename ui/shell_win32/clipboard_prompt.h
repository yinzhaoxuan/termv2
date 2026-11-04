// DS-10：剪贴板读取确认；关闭或创建失败均按拒绝一次处理。
#pragma once
#include <windows.h>
#include <optional>
#include <string>
#include "uishell/pane_backend.h"

std::optional<uishell::ClipboardReadDecision> runClipboardReadPrompt(
    HWND owner, const std::string& mainText, const std::string& content);
