// SPDX-License-Identifier: GPL-3.0-or-later
// DS-14：资源对话框与异步 VM 列表；配置逻辑在 uicfg。
#include "connection_dialog.h"
#include "resource.h"
#include "win32_text.h"
#include <commdlg.h>
#include <initializer_list>
#include <iterator>
#include <memory>
#ifndef IT2W_SHELL_STUB_BACKEND
#include "hyperv_query.h"
#include <mutex>
#include <system_error>
#include <thread>
#endif

using namespace uicfg;
using shellwin::W;
using shellwin::U;
namespace {
std::string text(HWND dlg, int id) {
  HWND control = GetDlgItem(dlg, id);
  int n = GetWindowTextLengthW(control);
  std::wstring value(n + 1, L'\0');
  value.resize(GetWindowTextW(control, value.data(), n + 1));
  return U(value);
}
void setText(HWND dlg, int id, const std::string& value) { SetDlgItemTextW(dlg, id, W(value).c_str()); }
int selection(HWND dlg, int id) { return static_cast<int>(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }
void select(HWND dlg, int id, int index) { SendDlgItemMessageW(dlg, id, CB_SETCURSEL, index, 0); }
void addChoice(HWND dlg, int id, const std::string& label) {
  SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(label).c_str()));
}
void showControls(HWND dlg, std::initializer_list<int> ids, bool visible) {
  for (int id : ids) ShowWindow(GetDlgItem(dlg, id), visible ? SW_SHOW : SW_HIDE);
}
constexpr FormKind kKinds[] = {FormKind::SshManual, FormKind::SshHyperV, FormKind::Local};
constexpr AuthMode kAuthModes[] = {AuthMode::Agent, AuthMode::KeyFile, AuthMode::Interactive};
FormKind selectedKind(HWND dlg) {
  int index = selection(dlg, IDC_KIND);
  return index >= 0 && index < static_cast<int>(std::size(kKinds)) ? kKinds[index] : FormKind::SshManual;
}
AuthMode selectedAuth(HWND dlg) {
  int index = selection(dlg, IDC_AUTH);
  return index >= 0 && index < static_cast<int>(std::size(kAuthModes)) ? kAuthModes[index] : AuthMode::Agent;
}
void updateVisibility(HWND dlg) {
  auto kind = selectedKind(dlg);
  const bool ssh = kind != FormKind::Local, hv = kind == FormKind::SshHyperV;
  showControls(dlg, {IDC_SSH_BOX, IDC_PORT_LABEL, IDC_PORT, IDC_USER_LABEL, IDC_USER,
      IDC_AUTH_LABEL, IDC_AUTH, IDC_TRANSPORT_LABEL, IDC_TRANSPORT,
      IDC_TIMEOUT_LABEL, IDC_TIMEOUT, IDC_TIMEOUT_UNIT}, ssh);
  showControls(dlg, {IDC_HOST_LABEL, IDC_HOST}, ssh && !hv);
  showControls(dlg, {IDC_VM_LABEL, IDC_VM, IDC_VM_SELECT, IDC_ADAPTER_LABEL, IDC_ADAPTER,
      IDC_FALLBACK_LABEL, IDC_FALLBACK}, hv);
  showControls(dlg, {IDC_KEY_LABEL, IDC_KEY, IDC_KEY_BROWSE}, ssh && selectedAuth(dlg) == AuthMode::KeyFile);
  showControls(dlg, {IDC_LOCAL_BOX, IDC_EXECUTABLE_LABEL, IDC_EXECUTABLE, IDC_EXECUTABLE_BROWSE,
      IDC_ARGUMENTS_LABEL, IDC_ARGUMENTS, IDC_ARGUMENTS_NOTE, IDC_DIRECTORY_LABEL,
      IDC_DIRECTORY, IDC_DIRECTORY_NOTE, IDC_ENVIRONMENT_NOTE}, !ssh);
}
void browseFile(HWND dlg, int id, bool executable) {
  wchar_t path[32768]{};
  GetDlgItemTextW(dlg, id, path, static_cast<int>(std::size(path)));
  OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = dlg;
  ofn.lpstrFile = path; ofn.nMaxFile = static_cast<DWORD>(std::size(path));
  ofn.lpstrFilter = executable ? L"程序 (*.exe)\0*.exe\0所有文件\0*.*\0\0" : L"所有文件\0*.*\0\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) SetDlgItemTextW(dlg, id, path);
}
#ifndef IT2W_SHELL_STUB_BACKEND
constexpr UINT WM_APP_VMLIST = WM_APP + 33;
struct VmQuery {
  HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::mutex mutex;
  HWND target = nullptr;  // mutex 保护：关闭后不向可能复用的 HWND 投递。
  ~VmQuery() { if (cancel) CloseHandle(cancel); }
};
#endif
struct ConnectionState {
  ConnectionDialogInput& input;
  std::vector<std::string> adapters;
#ifndef IT2W_SHELL_STUB_BACKEND
  std::shared_ptr<VmQuery> query;
#endif
  explicit ConnectionState(ConnectionDialogInput& value) : input(value) {}
};
void initialAdapters(HWND dlg, ConnectionState& state) {
  state.adapters = {""}; addChoice(dlg, IDC_ADAPTER, "自动");
  const auto& id = state.input.form.adapterId;
  if (!id.empty()) { state.adapters.push_back(id); addChoice(dlg, IDC_ADAPTER, id); }
  select(dlg, IDC_ADAPTER, id.empty() ? 0 : 1);
}
void initialize(HWND dlg, ConnectionState& state) {
  const auto& input = state.input;
  const auto& f = input.form;
  SetWindowTextW(dlg, input.title.c_str());
  setText(dlg, IDC_NAME, f.name);
  int index = 0;
  for (const auto& group : input.groups) {
    addChoice(dlg, IDC_GROUP, group.second);
    if (group.first == f.groupId) select(dlg, IDC_GROUP, index);
    ++index;
  }
  index = 0;
  for (const auto& id : input.appearanceProfiles) {
    addChoice(dlg, IDC_APPEARANCE, id + (input.currentAppearanceUndefined && id == f.appearanceProfileId ? "（未定义）" : ""));
    if (id == f.appearanceProfileId) select(dlg, IDC_APPEARANCE, index);
    ++index;
  }
  for (const char* label : {"SSH（手动地址）", "SSH（Hyper-V 虚拟机）", "本地程序"}) addChoice(dlg, IDC_KIND, label);
  for (int i = 0; i < static_cast<int>(std::size(kKinds)); ++i) if (kKinds[i] == f.kind) select(dlg, IDC_KIND, i);
  setText(dlg, IDC_HOST, f.host); setText(dlg, IDC_PORT, f.port);
  setText(dlg, IDC_VM, f.vmId); setText(dlg, IDC_FALLBACK, f.fallbackHost);
  initialAdapters(dlg, state);
  setText(dlg, IDC_USER, f.user); setText(dlg, IDC_KEY, f.identityFile);
  for (const char* label : {"agent", "密钥文件", "交互（密码等）"}) addChoice(dlg, IDC_AUTH, label);
  for (int i = 0; i < static_cast<int>(std::size(kAuthModes)); ++i) if (kAuthModes[i] == f.auth) select(dlg, IDC_AUTH, i);
  for (const char* label : {"跟随全局默认", "兼容（ssh.exe）", "直连"}) addChoice(dlg, IDC_TRANSPORT, label);
  select(dlg, IDC_TRANSPORT, !f.transport ? 0 : (*f.transport == SshTransport::Compatible ? 1 : 2));
  setText(dlg, IDC_TIMEOUT, f.connectTimeout); setText(dlg, IDC_EXECUTABLE, f.executable);
  setText(dlg, IDC_ARGUMENTS, f.argumentsText); setText(dlg, IDC_DIRECTORY, f.workingDirectory);
  SendDlgItemMessageW(dlg, IDC_ARGUMENTS, EM_SETREADONLY, !f.argumentsEditable, 0);
  if (!f.argumentsEditable) setText(dlg, IDC_ARGUMENTS_NOTE, "参数含换行或空参数，请在 connections.json 中编辑");
#ifdef IT2W_SHELL_STUB_BACKEND
  EnableWindow(GetDlgItem(dlg, IDC_VM_SELECT), FALSE);
#else
  EnableWindow(GetDlgItem(dlg, IDC_VM_SELECT), input.canListVms);
#endif
  updateVisibility(dlg);
}
ConnectionForm readForm(HWND dlg, const ConnectionState& state) {
  auto f = state.input.form;
  f.name = text(dlg, IDC_NAME);
  int index = selection(dlg, IDC_GROUP);
  if (index >= 0 && static_cast<std::size_t>(index) < state.input.groups.size()) f.groupId = state.input.groups[index].first;
  index = selection(dlg, IDC_APPEARANCE);
  if (index >= 0 && static_cast<std::size_t>(index) < state.input.appearanceProfiles.size()) f.appearanceProfileId = state.input.appearanceProfiles[index];
  f.kind = selectedKind(dlg);
  if (f.kind == FormKind::Local) {
    f.executable = text(dlg, IDC_EXECUTABLE); f.argumentsText = text(dlg, IDC_ARGUMENTS);
    f.workingDirectory = text(dlg, IDC_DIRECTORY);
    return f;
  }
  f.auth = selectedAuth(dlg);
  f.port = text(dlg, IDC_PORT); f.user = text(dlg, IDC_USER);
  if (f.kind == FormKind::SshManual) f.host = text(dlg, IDC_HOST);
  else {
    f.vmId = text(dlg, IDC_VM); f.fallbackHost = text(dlg, IDC_FALLBACK);
    index = selection(dlg, IDC_ADAPTER);
    if (index >= 0 && static_cast<std::size_t>(index) < state.adapters.size()) f.adapterId = state.adapters[index];
  }
  if (f.auth == AuthMode::KeyFile) f.identityFile = text(dlg, IDC_KEY);
  f.connectTimeout = text(dlg, IDC_TIMEOUT);
  index = selection(dlg, IDC_TRANSPORT);
  f.transport = index == 1 ? std::optional<SshTransport>(SshTransport::Compatible) :
      index == 2 ? std::optional<SshTransport>(SshTransport::Direct) : std::nullopt;
  return f;
}
void showError(HWND dlg, const ConnectionState& state, const ValidationError& error) {
  setText(dlg, IDC_ERROR, error.message);
  auto found = state.input.fieldControls.find(error.field);
  if (found != state.input.fieldControls.end()) {
    HWND control = GetDlgItem(dlg, found->second);
    if (IsWindowVisible(control) && IsWindowEnabled(control)) SetFocus(control);
  }
}
#ifndef IT2W_SHELL_STUB_BACKEND
void startVmList(HWND dlg, ConnectionState& state) {
  if (!state.input.canListVms || !IsWindowEnabled(GetDlgItem(dlg, IDC_VM_SELECT))) return;
  auto query = std::make_shared<VmQuery>();
  if (!query->cancel) { setText(dlg, IDC_ERROR, "创建 Hyper-V 查询取消事件失败"); return; }
  query->target = dlg;
  setText(dlg, IDC_ERROR, ""); setText(dlg, IDC_VM_SELECT, "正在查询…");
  EnableWindow(GetDlgItem(dlg, IDC_VM_SELECT), FALSE);
  try {
    std::thread([query] {
      auto result = std::make_unique<it2w::HvQueryResult>(it2w::listHyperVVms(15000, query->cancel));
      std::lock_guard<std::mutex> lock(query->mutex);
      if (query->target && PostMessageW(query->target, WM_APP_VMLIST, 0, reinterpret_cast<LPARAM>(result.get())))
        result.release();
    }).detach();
    state.query = std::move(query);
  } catch (const std::system_error&) {
    setText(dlg, IDC_VM_SELECT, "选择…"); EnableWindow(GetDlgItem(dlg, IDC_VM_SELECT), TRUE);
    setText(dlg, IDC_ERROR, "无法启动 Hyper-V 查询线程");
  }
}
void receiveVmList(HWND dlg, ConnectionState& state, const it2w::HvQueryResult& result) {
  setText(dlg, IDC_VM_SELECT, "选择…"); EnableWindow(GetDlgItem(dlg, IDC_VM_SELECT), TRUE);
  if (result.err != it2w::HvQueryError::None) {
    setText(dlg, IDC_ERROR, hyperVListError(it2w::hvQueryErrorName(result.err), result.msg)); return;
  }
  if (result.vms.empty()) { setText(dlg, IDC_ERROR, "没有找到虚拟机"); return; }
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  for (std::size_t i = 0; i < result.vms.size(); ++i) {
    const auto& vm = result.vms[i];
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(i + 1), W(vm.name + "（" + vm.state + "）").c_str());
  }
  RECT rect{}; GetWindowRect(GetDlgItem(dlg, IDC_VM_SELECT), &rect);
  int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, rect.left, rect.bottom, 0, dlg, nullptr);
  DestroyMenu(menu);
  if (chosen <= 0 || static_cast<std::size_t>(chosen) > result.vms.size()) return;
  const auto& vm = result.vms[chosen - 1];
  setText(dlg, IDC_VM, vm.id);
  SendDlgItemMessageW(dlg, IDC_ADAPTER, CB_RESETCONTENT, 0, 0);
  state.adapters = {""}; addChoice(dlg, IDC_ADAPTER, "自动");
  for (const auto& adapter : vm.adapters) {
    state.adapters.push_back(adapter.id);
    addChoice(dlg, IDC_ADAPTER, adapter.name + " · " + adapter.mac);
  }
  select(dlg, IDC_ADAPTER, 0);
}
void cancelVmList(HWND dlg, ConnectionState& state) {
  if (!state.query) return;
  SetEvent(state.query->cancel);
  std::lock_guard<std::mutex> lock(state.query->mutex);
  state.query->target = nullptr;
  // PostMessage 成功但尚未派发的结果也归对话框释放。
  MSG message{};
  while (PeekMessageW(&message, dlg, WM_APP_VMLIST, WM_APP_VMLIST, PM_REMOVE))
    delete reinterpret_cast<it2w::HvQueryResult*>(message.lParam);
}
#endif
INT_PTR CALLBACK ConnectionDlgProc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<ConnectionState*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    state = reinterpret_cast<ConnectionState*>(l);
    SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    initialize(dlg, *state); return TRUE;
  }
  if (!state) return FALSE;
  switch (msg) {
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case IDC_KIND: case IDC_AUTH:
          if (HIWORD(w) == CBN_SELCHANGE) updateVisibility(dlg);
          return TRUE;
        case IDC_KEY_BROWSE: browseFile(dlg, IDC_KEY, false); return TRUE;
        case IDC_EXECUTABLE_BROWSE: browseFile(dlg, IDC_EXECUTABLE, true); return TRUE;
        case IDC_VM_SELECT:
#ifndef IT2W_SHELL_STUB_BACKEND
          startVmList(dlg, *state);
#endif
          return TRUE;
        case IDOK: {
          const auto form = readForm(dlg, *state);
          if (auto error = state->input.commit(form)) showError(dlg, *state, *error);
          else { state->input.form = form; EndDialog(dlg, IDOK); }
          return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
      }
      break;
    case WM_CLOSE: EndDialog(dlg, IDCANCEL); return TRUE;
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID(reinterpret_cast<HWND>(l)) == IDC_ERROR) {
        SetTextColor(reinterpret_cast<HDC>(w), RGB(190, 0, 0));
        SetBkColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_3DFACE));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
      }
      break;
#ifndef IT2W_SHELL_STUB_BACKEND
    case WM_APP_VMLIST: {
      std::unique_ptr<it2w::HvQueryResult> result(reinterpret_cast<it2w::HvQueryResult*>(l));
      receiveVmList(dlg, *state, *result); return TRUE;
    }
    case WM_DESTROY: cancelVmList(dlg, *state); return TRUE;
#endif
  }
  return FALSE;
}
struct TextState {
  const std::wstring& title;
  const std::wstring& label;
  const std::string& initial;
  std::function<std::string(const std::string&)> commit;
  std::optional<std::string> result;
};
INT_PTR CALLBACK TextDlgProc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<TextState*>(GetWindowLongPtrW(dlg, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    state = reinterpret_cast<TextState*>(l);
    SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dlg, state->title.c_str()); SetDlgItemTextW(dlg, IDC_PROMPT_LABEL, state->label.c_str());
    setText(dlg, IDC_PROMPT_TEXT, state->initial);
    SendDlgItemMessageW(dlg, IDC_PROMPT_TEXT, EM_SETSEL, 0, -1);
    return TRUE;
  }
  if (!state) return FALSE;
  if (msg == WM_COMMAND && LOWORD(w) == IDOK) {
    auto value = text(dlg, IDC_PROMPT_TEXT);
    auto error = state->commit(value);
    if (error.empty()) { state->result = std::move(value); EndDialog(dlg, IDOK); }
    else { setText(dlg, IDC_PROMPT_LABEL, error); SetFocus(GetDlgItem(dlg, IDC_PROMPT_TEXT)); }
    return TRUE;
  }
  if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL)) {
    EndDialog(dlg, IDCANCEL); return TRUE;
  }
  return FALSE;
}
}  // namespace

bool runConnectionDialog(HWND owner, HINSTANCE inst, ConnectionDialogInput& input) {
  ConnectionState state(input);
  return DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CONNECTION), owner, ConnectionDlgProc,
                         reinterpret_cast<LPARAM>(&state)) == IDOK;
}
std::optional<std::string> runTextPrompt(HWND owner, HINSTANCE inst, const std::wstring& title,
    const std::wstring& label, const std::string& initial,
    std::function<std::string(const std::string&)> commit) {
  TextState state{title, label, initial, std::move(commit), std::nullopt};
  DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_TEXT_PROMPT), owner, TextDlgProc, reinterpret_cast<LPARAM>(&state));
  return state.result;
}
