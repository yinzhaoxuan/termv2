// T08 Win32 主外壳：左侧会话树、标签栏、窗格区（每窗格一个容器 HWND，后端在其上建子窗口与交换链）、状态栏。
// 状态全部放在 uicfg 模型里（tab_model / session_tree / close_confirm / reconnect / shell_logic），
// 这里只做 Win32 胶水。UTF-8 ↔ UTF-16 只在 Win32 边界转换。未实机验证。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "uicfg/close_confirm.h"
#include "uicfg/layout_ops.h"
#include "uicfg/reconnect.h"
#include "uicfg/session_tree.h"
#include "uicfg/settings.h"
#include "uicfg/shell_logic.h"
#include "uicfg/store.h"
#include "uicfg/tab_model.h"
#include "uicfg/workspace.h"
#include "uishell/pane_backend.h"

#ifdef IT2W_SHELL_STUB_BACKEND
#include "stub_backend.h"
#else
#include "win32_pane_backend.h"
#endif

namespace fs = std::filesystem;
using namespace uicfg;
using uishell::SessionState;

namespace {

// ---------- UTF-8 / UTF-16 ----------
std::wstring W(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string U(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

constexpr wchar_t kMainClass[] = L"it2w_shell_main";
constexpr wchar_t kPaneHostClass[] = L"it2w_shell_pane";
constexpr UINT_PTR kIdSearch = 101, kIdList = 102, kIdTabs = 103, kIdStatus = 104, kIdReconnect = 200;

struct PaneHost {
  std::string paneId, connectionId, title;
  HWND host = nullptr, overlay = nullptr, reconnectBtn = nullptr;
  std::unique_ptr<uishell::PaneBackend> backend;
  uishell::SessionStatus status{SessionState::Starting, 0, false, "", ""};
  PaneAttempts attempts;
  bool missingRef = false;  // 恢复布局时连接已删除
};

struct App {
  HINSTANCE inst{};
  HWND main{}, search{}, list{}, tabs{}, statusBar{};
  fs::path configDir;
  ConfigDirLock lock;
  ConnectionsFile conns;
  bool connsReadOnly = false;
  SettingsFile settings;
  WorkspaceFile ws;
  bool wsReadOnly = false;
  int paneSeq = 0;
  TabBar tabBar{[this] { return "pane-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++paneSeq); }};
  std::map<std::string, std::unique_ptr<PaneHost>> panes;
  SessionSnapshots snaps;
  std::vector<std::optional<std::string>> listIds;  // 列表行 → 连接 id（nullopt = 分组标题；"" = 本地 shell）
  uishell::PaneBackendFactory factory;
  bool skipChar = false;
  bool quitting = false;
  UINT dpi = 96;
};
App* g = nullptr;

float scale() { return g->dpi / 96.f; }
int px(int v) { return (int)std::lround(v * scale()); }

const Connection* findConn(const std::string& id) {
  for (auto& c : g->conns.connections) if (c.id == id) return &c;
  return nullptr;
}
std::string connName(const std::string& id) {
  if (id.empty()) return "本地终端";
  if (auto* c = findConn(id)) return c->name;
  if (auto* s = g->snaps.snapshot(id)) return s->name;
  return id;
}
PaneHost* paneById(const std::string& id) {
  auto it = g->panes.find(id); return it == g->panes.end() ? nullptr : it->second.get();
}
PaneHost* activePane() {
  Tab* t = g->tabBar.activeTab();
  return t ? paneById(t->activePaneId) : nullptr;
}

// ---------- 布局 ----------
RECT paneArea() {
  RECT rc; GetClientRect(g->main, &rc);
  RECT r{px(220), px(30), rc.right, rc.bottom - px(22)};
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}
Rect toRect(const RECT& r) { return {0, 0, double(r.right - r.left), double(r.bottom - r.top)}; }
int estimateRows(const PaneHost& p) {
  RECT rc; GetClientRect(p.host, &rc);
  double cellH = g->settings.fontSize * 96.0 / 72.0 * 1.2 * scale();  // 近似：后端未暴露网格行数
  return std::max(1, (int)((rc.bottom - rc.top) / std::max(1.0, cellH)));
}

void updateStatusBar();
void layoutPanes();

void positionOverlay(PaneHost& p) {
  RECT rc; GetClientRect(p.host, &rc);
  auto ov = p.missingRef ? PaneOverlay{true, false, "连接配置已删除（引用缺失）。请在左侧选择连接后新建会话。"}
                         : overlayFor(p.status);
  ShowWindow(p.overlay, ov.visible ? SW_SHOW : SW_HIDE);
  ShowWindow(p.reconnectBtn, ov.showReconnect ? SW_SHOW : SW_HIDE);
  if (!ov.visible) return;
  SetWindowTextW(p.overlay, W(ov.text).c_str());
  SetWindowPos(p.overlay, HWND_TOP, px(16), px(16), std::max(0L, rc.right - px(32)), px(60), 0);
  SetWindowPos(p.reconnectBtn, HWND_TOP, px(16), px(84), px(96), px(28), 0);
}

void layoutPanes() {
  RECT area = paneArea();
  std::map<std::string, bool> visible;
  if (Tab* t = g->tabBar.activeTab()) {
    for (auto& pr : computeRects(t->root, toRect(area))) {
      PaneHost* p = paneById(pr.paneId); if (!p) continue;
      visible[p->paneId] = true;
      int x = area.left + (int)pr.rect.x, y = area.top + (int)pr.rect.y;
      int w = std::max(1, (int)pr.rect.w - 1), h = std::max(1, (int)pr.rect.h - 1);  // 1px 分隔
      SetWindowPos(p->host, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      ShowWindow(p->host, SW_SHOW);
      if (p->backend) p->backend->setBounds({0, 0, w, h}, scale());
      positionOverlay(*p);
    }
  }
  for (auto& [id, p] : g->panes) if (!visible.count(id)) ShowWindow(p->host, SW_HIDE);
}

void layoutAll() {
  RECT rc; GetClientRect(g->main, &rc);
  int side = px(220);
  MoveWindow(g->search, px(4), px(4), side - px(8), px(24), TRUE);
  MoveWindow(g->list, 0, px(32), side, std::max(0L, rc.bottom - px(32) - px(22)), TRUE);
  MoveWindow(g->tabs, side, 0, std::max(0L, rc.right - side), px(30), TRUE);
  MoveWindow(g->statusBar, 0, rc.bottom - px(22), rc.right, px(22), TRUE);
  layoutPanes();
}

// ---------- 标签栏 ----------
void refreshTabs() {
  TabCtrl_DeleteAllItems(g->tabs);
  for (size_t i = 0; i < g->tabBar.count(); ++i) {
    std::wstring t = W(g->tabBar.titleFor(i, [](const std::string& pid) -> std::optional<std::string> {
      if (auto* p = paneById(pid)) if (!p->status.title.empty()) return p->status.title;
      return std::nullopt;
    }));
    TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = t.data();
    TabCtrl_InsertItem(g->tabs, (int)i, &it);
  }
  if (auto a = g->tabBar.activeIndex()) TabCtrl_SetCurSel(g->tabs, (int)*a);
}

// ---------- 会话树 ----------
void refreshTree() {
  wchar_t buf[256]; GetWindowTextW(g->search, buf, 256);
  auto groups = buildSessionTree(g->conns, U(buf), g->snaps.runningByConnection());
  SendMessageW(g->list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g->list, LB_RESETCONTENT, 0, 0);
  g->listIds.clear();
  auto add = [](const std::string& text, std::optional<std::string> id) {
    SendMessageW(g->list, LB_ADDSTRING, 0, (LPARAM)W(text).c_str());
    g->listIds.push_back(std::move(id));
  };
  add("＋ 本地终端", std::string());
  for (auto& gr : groups) {
    add("▾ " + gr.name, std::nullopt);
    for (auto& it : gr.items)
      add(std::string(it.hasRunning() ? "  ● " : "    ") + it.name + (it.host.empty() ? "" : "  · " + it.host), it.connectionId);
  }
  SendMessageW(g->list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g->list, nullptr, TRUE);
}

void updateStatusBar() {
  std::string s = "无活动窗格";
  if (PaneHost* p = activePane()) {
    s = "当前窗格：" + connName(p->connectionId) + " · " + (p->missingRef ? "引用缺失" : stateLabel(p->status.state));
    if (auto* c = findConn(p->connectionId); c && c->endpoint && c->endpoint->host) s += " · " + *c->endpoint->host;
    s += " · attempt #" + std::to_string(p->attempts.attemptId());
  }
  SetWindowTextW(g->statusBar, W(s).c_str());
}

// ---------- 窗格 ----------
void focusPane(const std::string& paneId) {
  Tab* t = g->tabBar.activeTab(); if (!t) return;
  t->activePaneId = paneId;
  for (auto& [id, p] : g->panes) if (p->backend) p->backend->setFocused(id == paneId);
  if (PaneHost* p = paneById(paneId)) if (!p->backend) SetFocus(p->host);
  refreshTabs();
  updateStatusBar();
}

void onPaneStatus(const std::string& paneId, const uishell::SessionStatus& st) {
  PaneHost* p = paneById(paneId); if (!p) return;
  p->status = st;
  if (st.state == SessionState::Exited || st.state == SessionState::Failed) g->snaps.onSessionEnded(paneId);
  positionOverlay(*p);
  refreshTabs(); refreshTree(); updateStatusBar();
}

void startSession(PaneHost& p) {
  if (auto* c = findConn(p.connectionId)) g->snaps.onSessionStarted(p.paneId, *c);  // 启动时快照
  else if (p.connectionId.empty()) { Connection local; local.name = "本地终端"; local.kind = ConnectionKind::Local; g->snaps.onSessionStarted(p.paneId, local); }
  uishell::PaneEvents ev;
  std::string id = p.paneId;  // 回调只捕获 id，按 id 查找（窗格可能已销毁）
  ev.onStatus = [id](const uishell::SessionStatus& st) { onPaneStatus(id, st); };
  ev.onBell = [] { FLASHWINFO fi{sizeof fi, g->main, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0}; FlashWindowEx(&fi); };
  ev.onLinkActivated = [](std::string url) {
    auto lp = linkPrompt(url);
    if (!lp.canOpen) { MessageBoxW(g->main, W(lp.message).c_str(), L"链接", MB_OK | MB_ICONWARNING); return; }
    if (MessageBoxW(g->main, W(lp.message).c_str(), L"打开链接", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK)
      ShellExecuteW(g->main, L"open", W(lp.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  };
  ev.onViewportChanged = [](int, int) {};
  ev.onPasteRejected = [](size_t bytes, size_t limit) {
    MessageBoxW(g->main, W(pasteRejectedMessage(bytes, limit)).c_str(), L"粘贴", MB_OK | MB_ICONWARNING);
  };
  ev.onSelectionChanged = [](bool) {};
  p.status = {SessionState::Starting, 0, false, "", ""};
  p.backend = g->factory(p.connectionId, std::move(ev));
  RECT rc; GetClientRect(p.host, &rc);
  if (p.backend) p.backend->attach(p.host, {0, 0, std::max(1L, rc.right), std::max(1L, rc.bottom)}, scale());
  positionOverlay(p);
}

LRESULT CALLBACK PaneHostProc(HWND, UINT, WPARAM, LPARAM);

PaneHost& createPane(const std::string& paneId, const std::string& connectionId, bool missingRef) {
  auto p = std::make_unique<PaneHost>();
  p->paneId = paneId; p->connectionId = connectionId; p->missingRef = missingRef;
  p->host = CreateWindowExW(0, kPaneHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 10, 10, g->main, nullptr, g->inst, nullptr);
  SetWindowLongPtrW(p->host, GWLP_USERDATA, (LONG_PTR)new std::string(paneId));
  p->overlay = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 10, 10, p->host, nullptr, g->inst, nullptr);
  p->reconnectBtn = CreateWindowExW(0, L"BUTTON", L"重连", WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, p->host,
                                    (HMENU)kIdReconnect, g->inst, nullptr);
  SendMessageW(p->overlay, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
  SendMessageW(p->reconnectBtn, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
  PaneHost& ref = *p;
  g->panes[paneId] = std::move(p);
  if (!missingRef) startSession(ref);
  return ref;
}

// 已确认后调用：只发起关闭（不阻塞），立刻销毁窗格；关闭线程由 shutdownPaneBackends 收尾
void destroyPane(const std::string& paneId) {
  auto it = g->panes.find(paneId); if (it == g->panes.end()) return;
  PaneHost& p = *it->second;
  if (p.backend) { if (stateNeedsConfirm(p.status.state)) p.backend->requestClose(); p.backend.reset(); }
  g->snaps.onSessionEnded(paneId);
  delete reinterpret_cast<std::string*>(GetWindowLongPtrW(p.host, GWLP_USERDATA));
  SetWindowLongPtrW(p.host, GWLP_USERDATA, 0);
  DestroyWindow(p.host);
  g->panes.erase(it);
}

void reconnectPane(PaneHost& p) {
  if (p.missingRef) return;
  if (p.backend) { if (stateNeedsConfirm(p.status.state)) p.backend->requestClose(); p.backend.reset(); }
  p.attempts.reconnect("");  // 后端未暴露旧 attempt 文本：只记录 attempt 号
  startSession(p);
  layoutPanes();
  focusPane(p.paneId);
}

PaneSessionInfo infoFor(const PaneHost& p) {
  return {p.paneId, p.status.title.empty() ? connName(p.connectionId) : p.status.title, connName(p.connectionId),
          p.missingRef ? SessionState::Exited : p.status.state};
}
std::optional<PaneSessionInfo> lookupInfo(const std::string& id) {
  if (auto* p = paneById(id)) return infoFor(*p);
  return std::nullopt;
}
bool confirm(const CloseSummary& s) {
  if (!s.needsConfirm) return true;
  return MessageBoxW(g->main, W(s.message).c_str(), L"确认关闭", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

// ---------- 标签 / 窗格操作 ----------
void openTab(const std::string& connectionId) {
  size_t i = g->tabBar.newTab(connectionId, connName(connectionId));  // 双击连接：总是新标签
  Tab& t = g->tabBar.tabs()[i];
  createPane(t.activePaneId, connectionId, false);
  refreshTabs(); layoutPanes(); focusPane(t.activePaneId); refreshTree();
}

void closeTabAt(size_t i, bool ask) {
  if (i >= g->tabBar.count()) return;
  Tab& t = g->tabBar.tabs()[i];
  if (ask && !confirm(summarizeCloseTab(t, lookupInfo))) return;
  for (auto& id : paneIds(t.root)) destroyPane(id);
  g->tabBar.closeTab(i);
  refreshTabs(); layoutPanes();
  if (Tab* a = g->tabBar.activeTab()) focusPane(a->activePaneId); else updateStatusBar();
  refreshTree();
}

void closeActivePane() {
  Tab* t = g->tabBar.activeTab(); PaneHost* p = activePane();
  if (!t || !p) return;
  if (!confirm(summarizeClosePane(infoFor(*p)))) return;
  std::string id = p->paneId;
  if (!closePane(*t, id)) { closeTabAt(*g->tabBar.activeIndex(), false); return; }  // 最后一个窗格 → 关标签
  destroyPane(id);
  layoutPanes(); focusPane(t->activePaneId); refreshTree();
}

void splitActive(SplitAxis axis) {
  Tab* t = g->tabBar.activeTab(); PaneHost* p = activePane();
  if (!t || !p) return;
  std::string conn = p->connectionId, newId = "pane-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++g->paneSeq);
  if (!splitPane(*t, p->paneId, axis, newId)) return;
  createPane(newId, conn, p->missingRef);  // 同一连接，独立 Session
  layoutPanes(); focusPane(newId); refreshTree();
}

void copySelection() {
  PaneHost* p = activePane(); if (!p || !p->backend) return;
  std::string s = p->backend->selectedText(); if (s.empty()) return;
  std::string crlf; for (char c : s) { if (c == '\n') crlf += '\r'; crlf += c; }
  std::wstring w = W(crlf);
  if (!OpenClipboard(g->main)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (h) {
    memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
}

void pasteClipboard() {
  PaneHost* p = activePane(); if (!p || !p->backend) return;
  if (!OpenClipboard(g->main)) return;
  std::wstring w;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* s = static_cast<const wchar_t*>(GlobalLock(h))) { w = s; GlobalUnlock(h); }
  }
  CloseClipboard();
  if (!w.empty()) p->backend->paste(normalizePasteText(U(w)));  // 上限由后端判断，超限走 onPasteRejected
}

bool runAction(const KeyMatch& m) {
  const std::string& a = m.action;
  PaneHost* p = activePane();
  if (a == "newTab") { openTab(p ? p->connectionId : std::string()); return true; }
  if (a == "closePane") { closeActivePane(); return true; }
  if (a == "splitVertical") { splitActive(SplitAxis::Columns); return true; }
  if (a == "splitHorizontal") { splitActive(SplitAxis::Rows); return true; }
  if (a == "nextTab" || a == "prevTab") {
    if (a == "nextTab") g->tabBar.next(); else g->tabBar.prev();
    refreshTabs(); layoutPanes(); if (Tab* t = g->tabBar.activeTab()) focusPane(t->activePaneId);
    return true;
  }
  if (a == "focusPane" && m.direction) {
    Tab* t = g->tabBar.activeTab();
    if (t) if (auto n = neighborPane(t->root, toRect(paneArea()), t->activePaneId, *m.direction)) focusPane(*n);
    return true;
  }
  if (a == "copy") { copySelection(); return true; }
  if (a == "paste") { pasteClipboard(); return true; }
  if (a == "searchConnections") { SetFocus(g->search); SendMessageW(g->search, EM_SETSEL, 0, -1); return true; }
  if (p && p->backend) {
    auto sc = scrollCommandFor(a, estimateRows(*p));
    if (sc.isScroll) { if (sc.toBottom) p->backend->scrollToBottom(); else p->backend->scrollViewport(sc.lines); return true; }
  }
  return false;  // searchHistory / font* 未实现：交给终端
}

std::optional<std::string> keyName(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk));
  if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  switch (vk) {
    case VK_TAB: return "Tab"; case VK_PRIOR: return "PageUp"; case VK_NEXT: return "PageDown";
    case VK_HOME: return "Home"; case VK_END: return "End"; case VK_UP: return "Up"; case VK_DOWN: return "Down";
    case VK_LEFT: return "Left"; case VK_RIGHT: return "Right"; case VK_OEM_PLUS: case VK_ADD: return "Plus";
    case VK_OEM_MINUS: case VK_SUBTRACT: return "Minus"; case VK_RETURN: return "Enter"; case VK_ESCAPE: return "Escape";
    case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_SPACE: return "Space";
  }
  return std::nullopt;
}

// 窗格（后端子窗口转发）收到的键盘消息。返回 true = 已处理
bool handleKey(UINT msg, WPARAM w, LPARAM) {
  PaneHost* p = activePane();
  if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
    g->skipChar = false;
    auto k = keyName(w); if (!k) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    auto m = matchKey(g->settings.keybindings, {ctrl, shift, alt, *k});
    if (!m.action.empty() && runAction(m)) { g->skipChar = true; return true; }
    std::string vt = vtForKey(*k, ctrl, shift, alt);
    if (!vt.empty() && p && p->backend) {
      p->backend->scrollToBottom(); p->backend->sendInput(vt);
      g->skipChar = (*k == "Tab");
      return true;
    }
    return false;
  }
  if (msg == WM_CHAR || msg == WM_SYSCHAR) {
    if (g->skipChar) { g->skipChar = false; return true; }
    static wchar_t highSurrogate = 0;
    wchar_t ch = (wchar_t)w;
    if (IS_HIGH_SURROGATE(ch)) { highSurrogate = ch; return true; }
    std::wstring ws;
    if (IS_LOW_SURROGATE(ch) && highSurrogate) { ws += highSurrogate; ws += ch; } else ws += ch;
    highSurrogate = 0;
    if (p && p->backend) { p->backend->scrollToBottom(); p->backend->sendInput(vtForChar(U(ws), msg == WM_SYSCHAR)); }
    return true;
  }
  return false;
}

LRESULT CALLBACK PaneHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  auto* id = reinterpret_cast<std::string*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (m) {
    case WM_MOUSEACTIVATE: case WM_LBUTTONDOWN:
      if (id) focusPane(*id);
      return m == WM_MOUSEACTIVATE ? MA_ACTIVATE : 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR:
      if (handleKey(m, w, l)) return 0;
      break;
    case WM_COMMAND:
      if (LOWORD(w) == kIdReconnect && id) { if (auto* p = paneById(*id)) reconnectPane(*p); return 0; }
      break;
    case WM_ERASEBKGND: {
      RECT rc; GetClientRect(h, &rc); FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

// ---------- 退出（SHELL_HOST.md 顺序）----------
WorkspaceFile snapshotWorkspace() {
  WorkspaceFile w;  // 保留 schema/revision
  w.schemaVersion = g->ws.schemaVersion; w.revision = g->ws.revision;
  for (auto& t : g->tabBar.tabs()) {
    Tab c; c.title = t.title; c.activePaneId = t.activePaneId;
    std::function<LayoutNode(const LayoutNode&)> copy = [&](const LayoutNode& n) -> LayoutNode {
      if (n.kind == LayoutNode::Kind::Pane) { LayoutNode p = LayoutNode::pane(n.paneId, n.connectionId); p.appearance = n.appearance; return p; }
      return LayoutNode::split(n.axis, n.ratio, copy(*n.first), copy(*n.second));
    };
    c.root = copy(t.root);
    w.tabs.push_back(std::move(c));
  }
  w.connectionsRevision = g->conns.revision;
  return w;
}

void shutdownAndExit() {
  g->quitting = true;
  WorkspaceFile w = snapshotWorkspace();  // 销毁前先取布局
  std::vector<std::string> ids; for (auto& [id, _] : g->panes) ids.push_back(id);
  for (auto& id : ids) destroyPane(id);                        // 1. 销毁所有窗格
#ifndef IT2W_SHELL_STUB_BACKEND
  it2app::shutdownPaneBackends(3000);                          // 2. 等关闭线程
#endif
  std::string err;                                             // 3. 保存 workspace.json
  if (!g->wsReadOnly && !saveWorkspace(g->configDir / "workspace.json", w, false, &err))
    MessageBoxW(nullptr, W("保存 workspace.json 失败：" + err).c_str(), L"it2w", MB_OK | MB_ICONERROR);
  g->lock.release();                                           // 4. 释放配置目录锁
  DestroyWindow(g->main);                                      // 5. 退出消息循环
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      g->main = h;
      g->dpi = GetDpiForWindow(h);
      g->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)kIdSearch, g->inst, nullptr);
      SendMessageW(g->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索会话");
      g->list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0, h, (HMENU)kIdList, g->inst, nullptr);
      g->tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 0, 0, 0, 0, h, (HMENU)kIdTabs, g->inst, nullptr);
      g->statusBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, h, (HMENU)kIdStatus, g->inst, nullptr);
      for (HWND c : {g->search, g->list, g->tabs, g->statusBar}) SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
      return 0;
    }
    case WM_SIZE: layoutAll(); return 0;
    case WM_DPICHANGED: {
      g->dpi = HIWORD(w);
      auto* r = reinterpret_cast<RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      layoutAll();
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == kIdSearch && HIWORD(w) == EN_CHANGE) { refreshTree(); return 0; }
      if (LOWORD(w) == kIdList && HIWORD(w) == LBN_DBLCLK) {
        int i = (int)SendMessageW(g->list, LB_GETCURSEL, 0, 0);
        if (i >= 0 && i < (int)g->listIds.size() && g->listIds[i]) openTab(*g->listIds[i]);
        return 0;
      }
      break;
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(l);
      if (nm->hwndFrom == g->tabs && nm->code == TCN_SELCHANGE) {
        g->tabBar.activate((size_t)TabCtrl_GetCurSel(g->tabs));
        layoutPanes(); if (Tab* t = g->tabBar.activeTab()) focusPane(t->activePaneId);
      }
      break;
    }
    case WM_MBUTTONUP: break;
    // 后端子窗口把键盘消息 SendMessage 给其父窗口（窗格容器）；若直接到主窗口也走同一处理
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR:
      if (handleKey(m, w, l)) return 0;
      break;
    case WM_CLOSE:
      if (!confirm(summarizeQuit(g->tabBar.tabs(), lookupInfo))) return 0;  // 一次汇总确认
      shutdownAndExit();
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

fs::path configDirPath() {
  PWSTR p = nullptr; fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / L"it2w";
  CoTaskMemFree(p);
  if (dir.empty()) if (const wchar_t* e = _wgetenv(L"APPDATA")) dir = fs::path(e) / L"it2w";
  return dir;
}

#ifndef IT2W_SHELL_STUB_BACKEND
std::optional<it2w::SshLaunchOptions> resolveSsh(const std::string& id, std::string* err) {
  const Connection* c = findConn(id);
  if (!c) { *err = "连接配置不存在"; return std::nullopt; }
  if (c->kind != ConnectionKind::Ssh || !c->endpoint || !c->ssh) { *err = "不是 SSH 连接"; return std::nullopt; }
  it2w::SshLaunchOptions o;
  if (c->endpoint->mode == EndpointMode::Manual) o.target.hostName = c->endpoint->host.value_or("");
  else if (c->endpoint->hyperv && c->endpoint->hyperv->fallbackHost) o.target.hostName = *c->endpoint->hyperv->fallbackHost;
  else { *err = "Hyper-V 地址解析尚未接入（T09），且未设置备用地址"; return std::nullopt; }
  o.target.port = (uint16_t)c->endpoint->port;
  o.target.user = c->ssh->user;
  o.target.connectTimeoutSec = (uint32_t)c->ssh->connectTimeoutSeconds;
  o.target.auth = c->ssh->authMode == AuthMode::Agent ? it2w::SshAuthMode::Agent
                : c->ssh->authMode == AuthMode::KeyFile ? it2w::SshAuthMode::KeyFile : it2w::SshAuthMode::Interactive;
  o.target.keyFile = c->ssh->identityFile.value_or("");
  fs::path sshDir = g->configDir / L"ssh", snapDir = sshDir / L"snapshots";
  std::error_code ec; fs::create_directories(snapDir, ec);
  fs::path kh = sshDir / L"known_hosts", empty = sshDir / L"empty_known_hosts";
  for (auto& f : {kh, empty}) if (!fs::exists(f, ec)) { HANDLE hf = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr); if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf); }
  o.target.appKnownHostsFile = U(kh.wstring());
  o.target.emptyKnownHostsFile = U(empty.wstring());
  o.snapshotDir = snapDir.wstring();
  return o;
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // manifest 之外的兜底
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  App app; g = &app; app.inst = inst;

  app.configDir = configDirPath();
  std::error_code ec; fs::create_directories(app.configDir, ec);
  std::string err;
  if (!app.lock.tryAcquire(app.configDir, &err)) {
    MessageBoxW(nullptr, W("配置目录已被另一个实例占用：" + err).c_str(), L"it2w", MB_OK | MB_ICONERROR);
    return 1;
  }
  auto lc = loadConnections(app.configDir / "connections.json");
  app.conns = std::move(lc.data); app.connsReadOnly = lc.readOnly;
  if (lc.status == LoadStatus::Corrupt)
    MessageBoxW(nullptr, W("connections.json 无法读取：" + lc.detail).c_str(), L"it2w", MB_OK | MB_ICONWARNING);
  auto ls = loadSettings(app.configDir / "settings.json");
  app.settings = (ls.status == LoadStatus::NotFound || ls.status == LoadStatus::Corrupt) ? defaultSettings() : ls.data;
  auto lw = loadWorkspace(app.configDir / "workspace.json");
  app.ws = std::move(lw.data); app.wsReadOnly = lw.readOnly;

#ifdef IT2W_SHELL_STUB_BACKEND
  app.factory = it2w_shell::makeStubFactory();
#else
  it2app::PaneBackendConfig cfg;
  cfg.resolveSsh = resolveSsh;
  cfg.fontFamily = W(app.settings.fontFamily);
  cfg.fontSizePt = (float)app.settings.fontSize;
  app.factory = it2app::makeWin32PaneBackendFactory(std::move(cfg));
#endif

  WNDCLASSEXW wc = {}; wc.cbSize = sizeof wc; wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = kMainClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
  WNDCLASSEXW pc = {}; pc.cbSize = sizeof pc; pc.lpfnWndProc = PaneHostProc; pc.hInstance = inst; pc.lpszClassName = kPaneHostClass;
  pc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  RegisterClassExW(&pc);
  HWND h = CreateWindowExW(0, kMainClass, L"iTerm2 for Windows", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760, nullptr, nullptr, inst, nullptr);
  if (!h) return 1;

  // 恢复布局：引用缺失的窗格只放占位，不启动
  std::map<std::string, bool> missing;
  for (auto& b : missingReferencePanes(app.ws, app.conns)) if (b.status == PaneBindStatus::MissingConnection) missing[b.paneId] = true;
  std::vector<Tab> restored = std::move(app.ws.tabs);
  app.ws.tabs.clear();
  app.tabBar.load(std::move(restored));
  for (auto& t : app.tabBar.tabs()) {
    std::function<void(const LayoutNode&)> walk = [&](const LayoutNode& n) {
      if (n.kind == LayoutNode::Kind::Pane) { createPane(n.paneId, n.connectionId, missing.count(n.paneId) > 0); return; }
      walk(*n.first); walk(*n.second);
    };
    walk(t.root);
    if (t.activePaneId.empty() || !paneById(t.activePaneId)) t.activePaneId = paneIds(t.root).front();
  }
  if (app.tabBar.empty()) openTab("");  // 首次运行：本地终端
  refreshTree(); refreshTabs();
  ShowWindow(h, show); UpdateWindow(h);
  layoutAll();
  if (Tab* t = app.tabBar.activeTab()) focusPane(t->activePaneId);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  g = nullptr;
  return 0;
}
