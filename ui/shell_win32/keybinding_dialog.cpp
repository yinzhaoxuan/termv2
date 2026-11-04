// SPDX-License-Identifier: GPL-3.0-or-later
#include "keybinding_dialog.h"
#include "resource.h"
#include "win32_text.h"
#include <commctrl.h>
#include <shellapi.h>

namespace {
struct DialogState {
  const std::vector<uicfg::KeybindingRow>& rows;
  const std::vector<uicfg::FixedKey>& fixed;
  const std::filesystem::path& configDir;
};

void fillList(HWND list, const std::vector<uicfg::KeybindingRow>& rows) {
  // 在代码中设置列表样式，避免 .rc 引入 commctrl.h。
  auto style = GetWindowLongPtrW(list, GWL_STYLE);
  style = (style & ~LVS_TYPEMASK) | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS;
  SetWindowLongPtrW(list, GWL_STYLE, style);
  SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  const wchar_t* headers[] = {L"功能", L"快捷键", L"状态"};
  for (int col = 0; col < 3; ++col) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    column.pszText = const_cast<wchar_t*>(headers[col]);
    SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(col), reinterpret_cast<LPARAM>(&column));
  }
  int index = 0;
  for (const auto& row : rows) {
    auto label = shellwin::W(row.label);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index++;
    item.pszText = label.data();
    const auto inserted = SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    if (inserted == -1) continue;
    item.iItem = static_cast<int>(inserted);
    const std::string* cells[] = {&row.chord, &row.note};
    for (int col = 1; col < 3; ++col) {
      auto value = shellwin::W(*cells[col - 1]);
      item.iSubItem = col;
      item.pszText = value.data();
      SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(item.iItem), reinterpret_cast<LPARAM>(&item));
    }
  }
  // 使用控件实际像素宽度，随资源对话框的字体与 DPI 缩放。
  RECT rect{};
  GetClientRect(list, &rect);
  const int width = static_cast<int>(rect.right - rect.left);
  SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, static_cast<LPARAM>(width / 4));
  SendMessageW(list, LVM_SETCOLUMNWIDTH, 1, static_cast<LPARAM>(width / 4));
  SendMessageW(list, LVM_SETCOLUMNWIDTH, 2, LVSCW_AUTOSIZE_USEHEADER);
}

INT_PTR CALLBACK KeybindingDlgProc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    state = reinterpret_cast<DialogState*>(l);
    SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    fillList(GetDlgItem(dlg, IDC_KEYBINDINGS_LIST), state->rows);
    std::string text;
    for (const auto& key : state->fixed) {
      if (!text.empty()) text += "\r\n";
      text += key.keys + " — " + key.label;
    }
    SetDlgItemTextW(dlg, IDC_FIXED_KEYS, shellwin::W(text).c_str());
    return TRUE;
  }
  if (!state) return FALSE;
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(w) == IDC_OPEN_CONFIG_DIR) {
        const auto result = ShellExecuteW(nullptr, L"open", state->configDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
          MessageBoxW(dlg, L"无法打开配置目录", L"it2w", MB_OK | MB_ICONERROR);
        return TRUE;
      }
      if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
        EndDialog(dlg, LOWORD(w));
        return TRUE;
      }
      break;
    case WM_CLOSE: EndDialog(dlg, IDCANCEL); return TRUE;
  }
  return FALSE;
}
}  // namespace

void runKeybindingDialog(HWND owner, HINSTANCE inst,
                         const std::vector<uicfg::KeybindingRow>& rows,
                         const std::vector<uicfg::FixedKey>& fixed,
                         const std::filesystem::path& configDir) {
  DialogState state{rows, fixed, configDir};
  if (DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_KEYBINDINGS), owner,
                      KeybindingDlgProc, reinterpret_cast<LPARAM>(&state)) == -1)
    MessageBoxW(owner, L"无法打开快捷键列表", L"it2w", MB_OK | MB_ICONERROR);
}
