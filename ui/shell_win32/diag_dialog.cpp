// SPDX-License-Identifier: GPL-3.0-or-later
#include "diag_dialog.h"
#include "resource.h"
#include "win32_text.h"

namespace {
struct PreviewState {
  const std::string& text;
  HFONT font = nullptr;
};
INT_PTR CALLBACK DiagDlgProc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<PreviewState*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    state = reinterpret_cast<PreviewState*>(l);
    SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    HWND edit = GetDlgItem(dlg, IDC_DIAG_TEXT);
    SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
    const int height = -MulDiv(10, static_cast<int>(GetDpiForWindow(dlg)), 72);
    state->font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    if (state->font) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    // 标准多行 EDIT 以 CRLF 分行；保存仍使用原始 UTF-8 文本。
    std::string display; display.reserve(state->text.size());
    for (char c : state->text) { if (c == '\n') display += '\r'; display += c; }
    SetWindowTextW(edit, shellwin::W(display).c_str());
    return TRUE;
  }
  if (!state) return FALSE;
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
        EndDialog(dlg, LOWORD(w)); return TRUE;
      }
      break;
    case WM_CLOSE: EndDialog(dlg, IDCANCEL); return TRUE;
  }
  return FALSE;
}
}  // namespace
bool runDiagPreview(HWND owner, HINSTANCE inst, const std::string& text) {
  PreviewState state{text};
  const auto result = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_DIAG_PREVIEW), owner,
                                    DiagDlgProc, reinterpret_cast<LPARAM>(&state));
  if (state.font) DeleteObject(state.font);
  if (result == -1) MessageBoxW(owner, L"无法打开诊断包预览", L"it2w", MB_OK | MB_ICONERROR);
  return result == IDOK;
}
