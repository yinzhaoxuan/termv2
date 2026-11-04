#include "clipboard_prompt.h"
#include "win32_text.h"
#include <commctrl.h>

std::optional<uishell::ClipboardReadDecision> runClipboardReadPrompt(
    HWND owner, const std::string& mainText, const std::string& content) {
  const auto main = shellwin::W(mainText);
  const auto body = shellwin::W(content);
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, L"允许"}, {IDNO, L"拒绝"}};
  TASKDIALOGCONFIG config{};
  config.cbSize = static_cast<UINT>(sizeof(config));
  config.hwndParent = owner;
  config.hInstance = GetModuleHandleW(nullptr);
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.pszWindowTitle = L"剪贴板读取请求";
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction = main.c_str();
  config.pszContent = body.c_str();
  config.cButtons = 2;
  config.pButtons = buttons;
  config.nDefaultButton = IDNO;
  config.pszVerificationText = L"本会话记住此选择";
  int button = IDCANCEL;
  BOOL remember = FALSE;
  if (FAILED(TaskDialogIndirect(&config, &button, nullptr, &remember))) return std::nullopt;
  using Decision = uishell::ClipboardReadDecision;
  if (button == IDYES) return remember ? Decision::AllowSession : Decision::AllowOnce;
  if (button == IDNO) return remember ? Decision::DenySession : Decision::DenyOnce;
  return std::nullopt;
}
