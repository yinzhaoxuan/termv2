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
#include <set>
#include <string>
#include <vector>

#include "uicfg/close_confirm.h"
#include "uicfg/layout_ops.h"
#include "uicfg/reconnect.h"
#include "uicfg/session_tree.h"
#include "uicfg/settings.h"
#include "uicfg/shell_logic.h"
#include "uicfg/shell_models.h"
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
constexpr UINT kMsgViewport = WM_APP + 1;
constexpr UINT_PTR kIdSearch = 101, kIdTree = 102, kIdTabs = 103, kIdStatus = 104, kIdReconnect = 200;

struct PaneHost {
  std::string paneId, connectionId, title;
  HWND host = nullptr, overlay = nullptr, reconnectBtn = nullptr;
  HWND scrollBar = nullptr, oldPanel = nullptr, separator = nullptr;  // 滚动条；旧 attempt 只读文本与分隔行
  int vpOffset = 0, vpHistory = 0;  // 最近一次 onViewportChanged
  std::unique_ptr<uishell::PaneBackend> backend;
  uishell::SessionStatus status{SessionState::Starting, 0, false, "", ""};
  PaneAttempts attempts;
  bool missingRef = false;  // 恢复布局时连接已删除
};

struct App {
  HINSTANCE inst{};
  HWND main{}, search{}, tree{}, tabs{}, statusBar{};
  fs::path configDir;
  ConfigDirLock lock;
  ConnectionsFile conns;
  bool connsReadOnly = false;
  SettingsFile settings;
  bool settingsReadOnly = false;
  std::string statusNote;
  WorkspaceFile ws;
  bool wsReadOnly = false;
  int paneSeq = 0;
  TabBar tabBar{[this] { return "pane-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++paneSeq); }};
  std::map<std::string, std::unique_ptr<PaneHost>> panes;
  SessionSnapshots snaps;
  struct TreeNode { bool isGroup; std::string id; };   // TreeView lParam → 下标；连接 id "" = 本地 shell
  std::vector<TreeNode> treeNodes;
  std::set<std::string> collapsedGroups;
  struct { bool active = false, dragging = false; int from = -1, x = 0, y = 0; } tabDrag;
  struct { bool active = false; std::vector<int> path; Rect splitArea; } divDrag;
  bool imeComposing = false;
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

// 优先用后端实测 cellSize()（GridMetrics，物理像素）；未知时回退估算
uishell::CellSize backendCell() {
  for (auto& [id, p] : g->panes)
    if (p->backend) { auto c = p->backend->cellSize(); if (c.w > 0 && c.h > 0) return c; }
  return {0, 0};
}
double cellH() { auto c = backendCell(); return c.h > 0 ? c.h : g->settings.fontSize * 96.0 / 72.0 * 1.2 * scale(); }
double cellW() { auto c = backendCell(); return c.w > 0 ? c.w : g->settings.fontSize * 96.0 / 72.0 * 0.6 * scale(); }

void updateStatusBar();
void layoutPanes();

// 窗格容器内部：[旧 attempt 只读面板][分隔行][终端 | 滚动条]
struct InnerLayout { AttemptPanelLayout a; int w, sbW; };
InnerLayout innerLayout(const PaneHost& p) {
  RECT rc; GetClientRect(p.host, &rc);
  int sbW = GetSystemMetricsForDpi(SM_CXVSCROLL, g->dpi);
  int w = std::max(1, (int)rc.right);
  return {attemptPanelLayout(rc.bottom, p.attempts.oldAttempt().has_value(), scale()), w, std::min(sbW, w - 1)};
}
uishell::PixelRect termRect(const PaneHost& p) {
  auto l = innerLayout(p);
  return {0, l.a.termY, std::max(1, l.w - l.sbW), std::max(1, l.a.termH)};
}
void updateScrollBar(PaneHost& p) {
  auto sb = scrollBarFor(p.vpOffset, p.vpHistory, std::max(1, (int)(termRect(p).h / std::max(1.0, cellH()))));
  SCROLLINFO si{}; si.cbSize = sizeof si; si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
  si.nMin = sb.min; si.nMax = sb.max; si.nPage = (UINT)sb.page; si.nPos = sb.pos;
  SetScrollInfo(p.scrollBar, SB_CTL, &si, TRUE);
  EnableScrollBar(p.scrollBar, SB_CTL, sb.enabled ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH);
}
void layoutPaneInner(PaneHost& p) {
  auto l = innerLayout(p);
  auto& old = p.attempts.oldAttempt();
  ShowWindow(p.oldPanel, old ? SW_SHOW : SW_HIDE);
  ShowWindow(p.separator, old ? SW_SHOW : SW_HIDE);
  if (old) {
    MoveWindow(p.oldPanel, 0, 0, l.w, l.a.panelH, TRUE);
    MoveWindow(p.separator, 0, l.a.panelH, l.w, l.a.sepH, TRUE);
  }
  auto tr = termRect(p);
  MoveWindow(p.scrollBar, tr.w, tr.y, l.sbW, tr.h, TRUE);
  if (p.backend) p.backend->setBounds(tr, scale());
  updateScrollBar(p);
}
void showOldAttempt(PaneHost& p) {
  auto& old = p.attempts.oldAttempt(); if (!old) return;
  std::string crlf = attemptHeader(old->attemptId) + "\n";
  for (char c : old->text) { if (c == '\n') crlf += '\r'; crlf += c; }
  SetWindowTextW(p.oldPanel, W(crlf).c_str());
  SendMessageW(p.oldPanel, EM_SETSEL, (WPARAM)-1, -1);  // 滚到尾部
  SendMessageW(p.oldPanel, EM_SCROLLCARET, 0, 0);
  SetWindowTextW(p.separator, W(p.attempts.separator()).c_str());
}

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

int estimateRows(const PaneHost& p) { return std::max(1, (int)(termRect(p).h / std::max(1.0, cellH()))); }

void layoutPanes() {
  RECT area = paneArea();
  const int gap = px(4);  // 分隔条宽度（主窗口客户区，可拖动）
  const double aw = area.right - area.left, ah = area.bottom - area.top;
  std::map<std::string, bool> visible;
  if (Tab* t = g->tabBar.activeTab()) {
    for (auto& pr : computeRects(t->root, toRect(area))) {
      PaneHost* p = paneById(pr.paneId); if (!p) continue;
      visible[p->paneId] = true;
      const auto& r = pr.rect;
      int l0 = r.x > 0.5 ? gap - gap / 2 : 0, r0 = r.x + r.w < aw - 0.5 ? gap / 2 : 0;
      int t0 = r.y > 0.5 ? gap - gap / 2 : 0, b0 = r.y + r.h < ah - 0.5 ? gap / 2 : 0;
      int x = area.left + (int)std::lround(r.x) + l0, y = area.top + (int)std::lround(r.y) + t0;
      int w = std::max(1, (int)std::lround(r.w) - l0 - r0), h = std::max(1, (int)std::lround(r.h) - t0 - b0);
      SetWindowPos(p->host, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      ShowWindow(p->host, SW_SHOW);
      layoutPaneInner(*p);
      positionOverlay(*p);
    }
  }
  for (auto& [id, p] : g->panes) if (!visible.count(id)) ShowWindow(p->host, SW_HIDE);
}

void layoutAll() {
  RECT rc; GetClientRect(g->main, &rc);
  int side = px(220);
  MoveWindow(g->search, px(4), px(4), side - px(8), px(24), TRUE);
  MoveWindow(g->tree, 0, px(32), side, std::max(0L, rc.bottom - px(32) - px(22)), TRUE);
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
    t += L"   \u00D7";  // 关闭按钮（点击区域见 tabCloseHit）
    TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = t.data();
    TabCtrl_InsertItem(g->tabs, (int)i, &it);
  }
  if (auto a = g->tabBar.activeIndex()) TabCtrl_SetCurSel(g->tabs, (int)*a);
}

// ---------- 会话树 ----------
void refreshTree() {
  wchar_t buf[256]; GetWindowTextW(g->search, buf, 256);
  std::string filter = U(buf);
  auto groups = buildSessionTree(g->conns, filter, g->snaps.runningByConnection());
  // 保留选中项
  std::optional<App::TreeNode> sel;
  if (HTREEITEM hs = TreeView_GetSelection(g->tree)) {
    TVITEMW it{}; it.mask = TVIF_PARAM; it.hItem = hs;
    if (TreeView_GetItem(g->tree, &it) && it.lParam >= 0 && (size_t)it.lParam < g->treeNodes.size()) sel = g->treeNodes[it.lParam];
  }
  SendMessageW(g->tree, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(g->tree);
  g->treeNodes.clear();
  HTREEITEM reselect = nullptr;
  auto add = [&](HTREEITEM parent, const std::string& text, App::TreeNode node, bool expanded) {
    std::wstring t = W(text);
    TVINSERTSTRUCTW ins{}; ins.hParent = parent; ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE; ins.item.pszText = t.data();
    ins.item.lParam = (LPARAM)g->treeNodes.size();
    ins.item.stateMask = TVIS_EXPANDED | TVIS_BOLD;
    ins.item.state = (expanded ? TVIS_EXPANDED : 0) | (node.isGroup ? TVIS_BOLD : 0);
    g->treeNodes.push_back(node);
    HTREEITEM h = (HTREEITEM)SendMessageW(g->tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    if (sel && sel->isGroup == node.isGroup && sel->id == node.id) reselect = h;
    return h;
  };
  if (filter.empty()) add(TVI_ROOT, "＋ 本地终端", {false, ""}, false);
  for (auto& gr : groups) {
    bool expanded = !filter.empty() || !g->collapsedGroups.count(gr.id);  // 搜索时全部展开
    HTREEITEM hg = add(TVI_ROOT, groupLabel(gr), {true, gr.id}, expanded);
    for (auto& it : gr.items) add(hg, itemLabel(it), {false, it.connectionId}, false);
    if (expanded) TreeView_Expand(g->tree, hg, TVE_EXPAND);
  }
  if (reselect) TreeView_SelectItem(g->tree, reselect);
  SendMessageW(g->tree, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g->tree, nullptr, TRUE);
}

std::optional<App::TreeNode> treeNodeOf(HTREEITEM h) {
  if (!h) return std::nullopt;
  TVITEMW it{}; it.mask = TVIF_PARAM; it.hItem = h;
  if (!TreeView_GetItem(g->tree, &it) || it.lParam < 0 || (size_t)it.lParam >= g->treeNodes.size()) return std::nullopt;
  return g->treeNodes[it.lParam];
}

void updateStatusBar() {
  std::string s = "无活动窗格";
  if (PaneHost* p = activePane()) {
    s = "当前窗格：" + connName(p->connectionId) + " · " + (p->missingRef ? "引用缺失" : stateLabel(p->status.state));
    if (auto* c = findConn(p->connectionId); c && c->endpoint && c->endpoint->host) s += " · " + *c->endpoint->host;
    s += " · attempt #" + std::to_string(p->attempts.attemptId());
  }
  if (!g->statusNote.empty()) s += " · " + g->statusNote;
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
  updateScrollBar(*p);  // 改字体后网格行数可能变化
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
  ev.onViewportChanged = [id](int off, int hist) {  // 投递到 UI 线程（契约要求 UI 线程回调，这里再保险一次）
    struct VpMsg { std::string id; int off, hist; };
    auto* m = new VpMsg{id, off, hist};
    if (!g || !PostMessageW(g->main, kMsgViewport, 0, (LPARAM)m)) delete m;
  };
  ev.onPasteRejected = [](size_t bytes, size_t limit) {
    MessageBoxW(g->main, W(pasteRejectedMessage(bytes, limit)).c_str(), L"粘贴", MB_OK | MB_ICONWARNING);
  };
  ev.onSelectionChanged = [](bool) {};
  p.status = {SessionState::Starting, 0, false, "", ""};
  p.backend = g->factory(p.connectionId, std::move(ev));
  p.vpOffset = p.vpHistory = 0;
  if (p.backend) p.backend->attach(p.host, termRect(p), scale());
  layoutPaneInner(p);
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
  p->scrollBar = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT, 0, 0, 10, 10, p->host, nullptr, g->inst, nullptr);
  p->oldPanel = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, p->host, nullptr, g->inst, nullptr);
  p->separator = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER | SS_CENTERIMAGE, 0, 0, 10, 10, p->host, nullptr, g->inst, nullptr);
  SendMessageW(p->oldPanel, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
  SendMessageW(p->separator, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
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
  if (p.backend) {
    p.backend->reconnect();  // 后端：新 core + 进程，attempt +1，保留上一 attempt 只读文本
    p.attempts.reconnect(p.backend->previousAttemptText());  // 按 15.4 预算截断，只留一个
    p.vpOffset = p.vpHistory = 0;
    showOldAttempt(p);
  } else {
    startSession(p);
  }
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

uishell::PaneBackendFactory makeFactory();

// 字号：6–72，写回 settings.json；经 setFont 运行时应用到所有窗格
void applyFontSize(double v) {
  if (v == g->settings.fontSize) { g->statusNote = "字号已是 " + std::to_string((int)v) + " pt"; updateStatusBar(); return; }
  g->settings.fontSize = v;
  std::string err;
  if (g->settingsReadOnly) g->statusNote = "settings.json 只读，字号未保存";
  else if (!saveSettings(g->configDir / "settings.json", g->settings, false, &err)) g->statusNote = "保存 settings.json 失败：" + err;
  else g->statusNote = "字号 " + std::to_string((int)v) + " pt";
  g->factory = makeFactory();
  for (auto& [id, p] : g->panes) {
    if (p->backend) p->backend->setFont(g->settings.fontFamily, (float)v);
    updateScrollBar(*p);
  }
  updateStatusBar();
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
  if (auto fsz = fontSizeFor(a, g->settings.fontSize)) { applyFontSize(*fsz); return true; }
  if (a == "copy") { copySelection(); return true; }
  if (a == "paste") { pasteClipboard(); return true; }
  if (a == "pasteCtrlV" || a == "pasteShiftInsert") { pasteClipboard(); return true; }
  if (a == "copyOrInterrupt") {
    PaneHost* p = activePane();
    if (!p || !p->backend || p->backend->selectedText().empty()) return false;  // 无选区：交给后端发 ^C
    copySelection(); p->backend->clearSelection(); return true;
  }
  if (a == "searchConnections") { SetFocus(g->search); SendMessageW(g->search, EM_SETSEL, 0, -1); return true; }
  if (p && p->backend) {
    auto sc = scrollCommandFor(a, estimateRows(*p));
    if (sc.isScroll) { if (sc.toBottom) p->backend->scrollToBottom(); else p->backend->scrollViewport(sc.lines); return true; }
  }
  return false;  // searchHistory 未实现：交给终端
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
    case VK_BACK: return "Backspace";
  }
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
  return std::nullopt;
}

bool isModifierVk(WPARAM vk) {
  switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
    case VK_NUMLOCK: case VK_SCROLL: case VK_APPS: case VK_PROCESSKEY: return true;
  }
  return false;
}

// 窗格（后端子窗口转发）收到的键盘消息。返回 true = 已处理。
// 特殊键 → sendKey(vk, mods)（后端用 itcore_encode_key 处理 DECCKM 等）；文本 → WM_CHAR → sendInput。
bool handleKey(UINT msg, WPARAM w, LPARAM l) {
  PaneHost* p = activePane();
  if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
    g->skipChar = false;
    bool processKey = w == VK_PROCESSKEY;
    if (processKey || g->imeComposing) return false;  // 输入法组字：交给 IME
    if (isModifierVk(w)) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    auto k = keyName(w);
    if (k) {
      auto m = matchKey(g->settings.keybindings, {ctrl, shift, alt, *k});
      if (!m.action.empty() && runAction(m)) { g->skipChar = true; return true; }
    }
    bool altGrText = false;
    if (ctrl && alt) {  // AltGr = Ctrl+Alt：ToUnicode 能译出字符就按文本发
      BYTE ks[256]; wchar_t buf[8];
      if (GetKeyboardState(ks)) {
        int n = ToUnicode((UINT)w, (UINT)((l >> 16) & 0xff), ks, buf, 8, 0x4 /* 不改内核键盘状态 */);
        altGrText = n > 0 && buf[0] >= 0x20;
      }
    }
    std::string name = k ? *k : "Char";
    if (routeKey(name, processKey, g->imeComposing, ctrl, alt, altGrText) == KeyRoute::SendKey && p && p->backend) {
      p->backend->scrollToBottom();
      p->backend->sendKey((unsigned)w, keyMods(ctrl, shift, alt));
      g->skipChar = true;  // 吞掉对应 WM_CHAR / WM_SYSCHAR
      return true;
    }
    return false;  // 等 WM_CHAR
  }
  if (msg == WM_CHAR || msg == WM_SYSCHAR) {
    if (g->skipChar) { g->skipChar = false; return true; }
    static wchar_t highSurrogate = 0;
    wchar_t ch = (wchar_t)w;
    if (IS_HIGH_SURROGATE(ch)) { highSurrogate = ch; return true; }
    std::wstring ws;
    if (IS_LOW_SURROGATE(ch) && highSurrogate) { ws += highSurrogate; ws += ch; } else ws += ch;
    highSurrogate = 0;
    if (p && p->backend) { p->backend->scrollToBottom(); p->backend->sendInput(U(ws)); }
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
    case WM_IME_STARTCOMPOSITION: g->imeComposing = true; break;
    case WM_IME_ENDCOMPOSITION: g->imeComposing = false; break;
    case WM_VSCROLL: {
      PaneHost* p = id ? paneById(*id) : nullptr;
      if (!p || !p->backend || (HWND)l != p->scrollBar) break;
      int rows = estimateRows(*p), lines = 0;
      switch (LOWORD(w)) {
        case SB_LINEUP: lines = -1; break;
        case SB_LINEDOWN: lines = 1; break;
        case SB_PAGEUP: lines = -rows; break;
        case SB_PAGEDOWN: lines = rows; break;
        case SB_TOP: lines = scrollLinesForThumb(0, p->vpOffset, p->vpHistory); break;
        case SB_BOTTOM: p->backend->scrollToBottom(); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
          SCROLLINFO si{}; si.cbSize = sizeof si; si.fMask = SIF_TRACKPOS;
          GetScrollInfo(p->scrollBar, SB_CTL, &si);
          lines = scrollLinesForThumb(si.nTrackPos, p->vpOffset, p->vpHistory);
          break;
        }
      }
      if (lines) p->backend->scrollViewport(lines);  // 后端回报 onViewportChanged 后再更新滑块
      if (LOWORD(w) == SB_ENDSCROLL) focusPane(*id);  // 焦点还给终端
      return 0;
    }
    case WM_CTLCOLORSTATIC:
      if (id) if (auto* p = paneById(*id); p && (HWND)l == p->separator) {
        SetBkMode((HDC)w, TRANSPARENT); SetTextColor((HDC)w, RGB(160, 160, 160));
        return (LRESULT)GetStockObject(BLACK_BRUSH);
      }
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

std::optional<Divider> dividerAt(POINT pt) {
  Tab* t = g->tabBar.activeTab(); if (!t) return std::nullopt;
  RECT a = paneArea();
  auto ds = computeDividers(t->root, toRect(a), px(6));
  if (auto i = hitDivider(ds, pt.x - a.left, pt.y - a.top)) return ds[*i];
  return std::nullopt;
}

// 标签右侧 × 的点击区域：标签矩形最右 18 逻辑像素
bool tabCloseHit(HWND h, int i, int x, int y) {
  RECT r; if (i < 0 || !TabCtrl_GetItemRect(h, i, &r)) return false;
  return x >= r.right - px(18) && x < r.right && y >= r.top && y < r.bottom;
}

// 标签栏：拖动重排、点 × 或中键关闭
LRESULT CALLBACK TabSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  auto hit = [&](LPARAM lp) { TCHITTESTINFO ht{}; ht.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; return TabCtrl_HitTest(h, &ht); };
  auto& d = g->tabDrag;
  switch (m) {
    case WM_LBUTTONDOWN: {
      int i = hit(l);
      if (tabCloseHit(h, i, GET_X_LPARAM(l), GET_Y_LPARAM(l))) { d = {}; closeTabAt((size_t)i, true); return 0; }
      d = {}; d.active = i >= 0; d.from = i; d.x = GET_X_LPARAM(l); d.y = GET_Y_LPARAM(l);
      break;  // 交给默认处理（切换标签）
    }
    case WM_MOUSEMOVE:
      if (d.active && (w & MK_LBUTTON) && !d.dragging &&
          isDragStart(GET_X_LPARAM(l) - d.x, GET_Y_LPARAM(l) - d.y, GetSystemMetricsForDpi(SM_CXDRAG, g->dpi))) {
        d.dragging = true; SetCapture(h);
      }
      if (d.dragging) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return 0; }
      break;
    case WM_LBUTTONUP:
      if (d.dragging) {
        int from = d.from; d = {}; ReleaseCapture();
        std::vector<std::pair<double, double>> spans;
        for (int i = 0; i < TabCtrl_GetItemCount(h); ++i) { RECT r; TabCtrl_GetItemRect(h, i, &r); spans.push_back({double(r.left), double(r.right)}); }
        auto to = tabDropIndex(spans, GET_X_LPARAM(l));
        if (to && (int)*to != from && g->tabBar.move((size_t)from, *to)) {
          refreshTabs(); layoutPanes();
          if (Tab* t = g->tabBar.activeTab()) focusPane(t->activePaneId);
        }
        return 0;
      }
      d.active = false;
      break;
    case WM_CAPTURECHANGED: if ((HWND)l != h) d = {}; break;
    case WM_MBUTTONUP: { int i = hit(l); if (i >= 0) closeTabAt((size_t)i, true); return 0; }
    case WM_NCDESTROY: RemoveWindowSubclass(h, TabSubclass, 1); break;
  }
  return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      g->main = h;
      g->dpi = GetDpiForWindow(h);
      g->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)kIdSearch, g->inst, nullptr);
      SendMessageW(g->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索会话");
      g->tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT |
                                TVS_SHOWSELALWAYS | TVS_FULLROWSELECT, 0, 0, 0, 0, h, (HMENU)kIdTree, g->inst, nullptr);

      g->tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 0, 0, 0, 0, h, (HMENU)kIdTabs, g->inst, nullptr);
      g->statusBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, h, (HMENU)kIdStatus, g->inst, nullptr);
      SetWindowSubclass(g->tabs, TabSubclass, 1, 0);
      for (HWND c : {g->search, g->tree, g->tabs, g->statusBar}) SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
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
      break;
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(l);
      if (nm->hwndFrom == g->tabs && nm->code == TCN_SELCHANGE) {
        g->tabBar.activate((size_t)TabCtrl_GetCurSel(g->tabs));
        layoutPanes(); if (Tab* t = g->tabBar.activeTab()) focusPane(t->activePaneId);
      }
      if (nm->hwndFrom == g->tree) {
        if (nm->code == NM_DBLCLK || nm->code == NM_RETURN) {
          if (auto n = treeNodeOf(TreeView_GetSelection(g->tree)); n && !n->isGroup) { openTab(n->id); return 1; }
        }
        if (nm->code == TVN_ITEMEXPANDEDW) {
          auto* tv = reinterpret_cast<NMTREEVIEWW*>(l);
          if (auto n = treeNodeOf(tv->itemNew.hItem); n && n->isGroup) {
            if (tv->action == TVE_COLLAPSE) g->collapsedGroups.insert(n->id); else g->collapsedGroups.erase(n->id);
          }
        }
      }
      break;
    }
    case kMsgViewport: {
      struct VpMsg { std::string id; int off, hist; };
      std::unique_ptr<VpMsg> vm(reinterpret_cast<VpMsg*>(l));
      if (PaneHost* p = paneById(vm->id)) { p->vpOffset = vm->off; p->vpHistory = vm->hist; updateScrollBar(*p); }
      return 0;
    }
    // ---- 分隔条拖动（分隔条是主窗口客户区里窗格之间的间隙）----
    case WM_SETCURSOR:
      if ((HWND)w == h && LOWORD(l) == HTCLIENT) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        if (auto d = dividerAt(pt)) { SetCursor(LoadCursorW(nullptr, d->axis == SplitAxis::Columns ? IDC_SIZEWE : IDC_SIZENS)); return TRUE; }
      }
      break;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (auto d = dividerAt(pt)) { g->divDrag.active = true; g->divDrag.path = d->path; g->divDrag.splitArea = d->splitArea; SetCapture(h); }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g->divDrag.active) {
        Tab* t = g->tabBar.activeTab(); LayoutNode* n = t ? nodeAtPath(t->root, g->divDrag.path) : nullptr;
        if (!n || n->kind != LayoutNode::Kind::Split) { g->divDrag.active = false; ReleaseCapture(); return 0; }
        RECT a = paneArea();
        n->ratio = dragRatio(*n, g->divDrag.splitArea, GET_X_LPARAM(l) - a.left, GET_Y_LPARAM(l) - a.top, cellW(), cellH());
        layoutPanes();
      }
      return 0;
    case WM_LBUTTONUP:
      if (g->divDrag.active) {
        g->divDrag.active = false; ReleaseCapture();
        if (Tab* t = g->tabBar.activeTab()) focusPane(t->activePaneId);  // 焦点留在原窗格
      }
      return 0;
    case WM_CAPTURECHANGED: g->divDrag.active = false; break;
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

uishell::PaneBackendFactory makeFactory() {
#ifdef IT2W_SHELL_STUB_BACKEND
  return it2w_shell::makeStubFactory();
#else
  it2app::PaneBackendConfig cfg;
  cfg.resolveSsh = resolveSsh;
  cfg.fontFamily = W(g->settings.fontFamily);
  cfg.fontSizePt = (float)g->settings.fontSize;
  return it2app::makeWin32PaneBackendFactory(std::move(cfg));
#endif
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // manifest 之外的兜底
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
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
  app.settingsReadOnly = ls.readOnly;
  app.settings.fontSize = std::clamp(app.settings.fontSize, kMinFontSize, kMaxFontSize);
  auto lw = loadWorkspace(app.configDir / "workspace.json");
  app.ws = std::move(lw.data); app.wsReadOnly = lw.readOnly;

  app.factory = makeFactory();

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
