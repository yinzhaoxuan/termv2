// T08 外壳选型实验：Win32/HWND 基线探针（对照 WinUI 3 / Qt）。
// 四个实验：1 IME 候选窗定位  2 便携启动  3 分屏 20×5 最小网格  4 快捷键捕获。
// 所有观察写入 exe 同目录的 probe-results-*.txt（不可写时退回 %TEMP%），同时显示在主窗口日志里。
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <psapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "uicfg/layout_ops.h"

using namespace uicfg;

namespace {

// ---------------- 通用 ----------------
std::string U8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring fmt(const wchar_t* f, ...) {
  wchar_t buf[2048]; va_list ap; va_start(ap, f); _vsnwprintf(buf, 2047, f, ap); va_end(ap); buf[2047] = 0; return buf;
}

HINSTANCE gInst;
HWND gMain, gLog, gNote;
FILE* gFile = nullptr;
std::wstring gResultsPath;

void logLine(const std::wstring& s) {
  SYSTEMTIME t; GetLocalTime(&t);
  std::wstring line = fmt(L"[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds) + s;
  if (gFile) { std::string u = U8(line) + "\r\n"; fwrite(u.data(), 1, u.size(), gFile); fflush(gFile); }
  if (gLog) {
    int len = GetWindowTextLengthW(gLog);
    if (len > 200000) { SetWindowTextW(gLog, L""); len = 0; }
    SendMessageW(gLog, EM_SETSEL, len, len);
    SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)(line + L"\r\n").c_str());
  }
}

std::wstring exeDir() {
  wchar_t p[MAX_PATH * 4]; DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
  std::wstring s(p, n); auto k = s.find_last_of(L"\\/"); return k == std::wstring::npos ? L"." : s.substr(0, k);
}

void openResults() {
  SYSTEMTIME t; GetLocalTime(&t);
  std::wstring name = fmt(L"probe-results-%04d%02d%02d-%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  gResultsPath = exeDir() + L"\\" + name;
  gFile = _wfopen(gResultsPath.c_str(), L"wb");
  if (!gFile) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    gResultsPath = std::wstring(tmp) + name;
    gFile = _wfopen(gResultsPath.c_str(), L"wb");
  }
  if (gFile) fwrite("\xEF\xBB\xBF", 1, 3, gFile);  // UTF-8 BOM，记事本直接打开
}

UINT dpiOf(HWND h) { return h ? GetDpiForWindow(h) : 96; }
int sc(HWND h, int v) { return MulDiv(v, (int)dpiOf(h), 96); }

std::wstring vkName(UINT vk, LPARAM l) {
  switch (vk) {
    case VK_TAB: return L"Tab"; case VK_MENU: return L"Alt"; case VK_LMENU: return L"LAlt"; case VK_RMENU: return L"RAlt";
    case VK_CONTROL: return L"Ctrl"; case VK_SHIFT: return L"Shift"; case VK_LWIN: return L"LWin"; case VK_RWIN: return L"RWin";
    case VK_LEFT: return L"←"; case VK_RIGHT: return L"→"; case VK_UP: return L"↑"; case VK_DOWN: return L"↓";
    case VK_F10: return L"F10"; case VK_PROCESSKEY: return L"VK_PROCESSKEY(IME)"; case VK_ESCAPE: return L"Esc";
    case VK_RETURN: return L"Enter"; case VK_SPACE: return L"Space";
  }
  if (vk >= VK_F1 && vk <= VK_F24) return fmt(L"F%u", vk - VK_F1 + 1);
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
  wchar_t n[64] = L""; GetKeyNameTextW((LONG)l, n, 64);
  return fmt(L"VK_0x%02X(%s)", vk, n);
}
std::wstring modsNow() {
  std::wstring s;
  if (GetKeyState(VK_CONTROL) < 0) s += L"Ctrl+";
  if (GetKeyState(VK_SHIFT) < 0) s += L"Shift+";
  if (GetKeyState(VK_MENU) < 0) s += L"Alt+";
  if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) s += L"Win+";
  return s;
}

void centerText(HDC dc, const RECT& r, const std::wstring& s, UINT extra = 0) {
  RECT rr = r; DrawTextW(dc, s.c_str(), (int)s.size(), &rr, DT_LEFT | DT_TOP | DT_WORDBREAK | extra);
}

HFONT uiFont(HWND h, int pt = 10) {
  return CreateFontW(-MulDiv(pt, (int)dpiOf(h), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
}

// ---------------- 实验 1：IME 候选窗定位 ----------------
struct ImeState {
  RECT caret{120, 160, 122, 184};   // 客户区坐标，模拟终端光标矩形（宽 2px，一行高）
  std::wstring committed, comp;
  int commits = 0;
} gIme;

void imeApply(HWND h, const wchar_t* why) {
  HIMC imc = ImmGetContext(h); if (!imc) { logLine(L"[IME] ImmGetContext 失败（" + std::wstring(why) + L"）"); return; }
  COMPOSITIONFORM cf{}; cf.dwStyle = CFS_FORCE_POSITION; cf.ptCurrentPos = {gIme.caret.left, gIme.caret.top};
  ImmSetCompositionWindow(imc, &cf);
  CANDIDATEFORM cand{}; cand.dwIndex = 0; cand.dwStyle = CFS_EXCLUDE;
  cand.ptCurrentPos = {gIme.caret.left, gIme.caret.bottom}; cand.rcArea = gIme.caret;
  BOOL ok = ImmSetCandidateWindow(imc, &cand);
  ImmReleaseContext(h, imc);
  POINT tl{gIme.caret.left, gIme.caret.top}, br{gIme.caret.right, gIme.caret.bottom};
  ClientToScreen(h, &tl); ClientToScreen(h, &br);
  logLine(fmt(L"[IME] %s：设置组字/候选位置 → 光标矩形 客户区(%ld,%ld,%ld,%ld) 屏幕(%ld,%ld)-(%ld,%ld) DPI=%u ok=%d",
              why, gIme.caret.left, gIme.caret.top, gIme.caret.right, gIme.caret.bottom, tl.x, tl.y, br.x, br.y, dpiOf(h), ok));
}

void imeMoveCaret(HWND h, int x, int y) {
  int lineH = sc(h, 24);
  gIme.caret = {x, y, x + sc(h, 2), y + lineH};
  InvalidateRect(h, nullptr, TRUE);
  imeApply(h, L"移动光标");
}

LRESULT CALLBACK ImeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: logLine(L"[IME] 窗口打开。请切到微软拼音，在不同位置（点击/F2/F3）连续输入几次中文"); return 0;
    case WM_SETFOCUS: imeApply(h, L"获得焦点"); return 0;
    case WM_LBUTTONDOWN: SetFocus(h); imeMoveCaret(h, GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_KEYDOWN:
      if (w == VK_F2) { RECT rc; GetClientRect(h, &rc); imeMoveCaret(h, rand() % std::max(1L, rc.right - sc(h, 40)), sc(h, 60) + rand() % std::max(1L, rc.bottom - sc(h, 100))); return 0; }
      if (w == VK_F3) { RECT rc; GetClientRect(h, &rc); imeMoveCaret(h, rc.right - sc(h, 12), rc.bottom - sc(h, 30)); return 0; }  // 右下角：候选窗应翻到上方/左侧
      if (w == VK_F4) { gIme.committed.clear(); gIme.commits = 0; InvalidateRect(h, nullptr, TRUE); return 0; }
      break;
    case WM_IME_STARTCOMPOSITION: imeApply(h, L"开始组字"); break;  // 继续交给 DefWindowProc 显示组字窗
    case WM_IME_COMPOSITION: {
      HIMC imc = ImmGetContext(h);
      if (imc) {
        if (l & GCS_COMPSTR) {
          LONG n = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
          std::wstring s(n / sizeof(wchar_t), L'\0'); if (n > 0) ImmGetCompositionStringW(imc, GCS_COMPSTR, s.data(), n);
          gIme.comp = s;
        }
        if (l & GCS_RESULTSTR) {
          LONG n = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
          std::wstring s(n / sizeof(wchar_t), L'\0'); if (n > 0) ImmGetCompositionStringW(imc, GCS_RESULTSTR, s.data(), n);
          ++gIme.commits; gIme.committed += s; gIme.comp.clear();
          logLine(fmt(L"[IME] 提交 #%d：「%s」", gIme.commits, s.c_str()));
        }
        ImmReleaseContext(h, imc);
      }
      imeApply(h, L"组字更新");
      InvalidateRect(h, nullptr, TRUE);
      break;
    }
    case WM_IME_NOTIFY:
      if (w == IMN_OPENCANDIDATE || w == IMN_CHANGECANDIDATE) imeApply(h, w == IMN_OPENCANDIDATE ? L"候选窗打开" : L"候选变化");
      break;
    case WM_IME_ENDCOMPOSITION: gIme.comp.clear(); InvalidateRect(h, nullptr, TRUE); break;
    case WM_IME_CHAR: return 0;  // 文本已在 GCS_RESULTSTR 记录，避免重复
    case WM_CHAR: if (w >= 0x20) { gIme.committed += (wchar_t)w; InvalidateRect(h, nullptr, TRUE); } return 0;
    case WM_DPICHANGED: {
      auto* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      logLine(fmt(L"[IME] DPI 变为 %u（拖到另一显示器后请再输入一次）", HIWORD(w)));
      imeMoveCaret(h, gIme.caret.left, gIme.caret.top); return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      HFONT f = uiFont(h), of = (HFONT)SelectObject(dc, f);
      RECT rc; GetClientRect(h, &rc); RECT top{sc(h, 8), sc(h, 8), rc.right - sc(h, 8), sc(h, 56)};
      SetBkMode(dc, TRANSPARENT);
      centerText(dc, top, L"点击任意位置放置模拟光标（红框）；F2 随机位置，F3 右下角，F4 清空。看：候选窗是否紧贴红框下方且不遮挡红框。");
      HBRUSH red = CreateSolidBrush(RGB(230, 0, 0)); RECT cr = gIme.caret; InflateRect(&cr, 1, 1); FrameRect(dc, &cr, red); FillRect(dc, &gIme.caret, red); DeleteObject(red);
      RECT ct{gIme.caret.right + 4, gIme.caret.top, rc.right, gIme.caret.bottom}; SetTextColor(dc, RGB(0, 90, 200));
      DrawTextW(dc, gIme.comp.c_str(), (int)gIme.comp.size(), &ct, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      RECT bt{sc(h, 8), rc.bottom - sc(h, 40), rc.right - sc(h, 8), rc.bottom}; SetTextColor(dc, RGB(0, 0, 0));
      std::wstring s = fmt(L"已提交 %d 次：", gIme.commits) + gIme.committed;
      DrawTextW(dc, s.c_str(), (int)s.size(), &bt, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE);
      SelectObject(dc, of); DeleteObject(f);
      EndPaint(h, &ps); return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

// ---------------- 实验 2：便携启动 ----------------
void portableReport() {
  logLine(L"===== 实验 2：便携启动（免安装） =====");
  wchar_t p[MAX_PATH * 4]; GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
  logLine(L"[便携] exe 路径：" + std::wstring(p));
  // 启动耗时：进程创建 → 此刻
  FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  FILETIME now; GetSystemTimeAsFileTime(&now);
  ULONGLONG d = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime);
  logLine(fmt(L"[便携] 进程创建到主窗口可见：%llu ms", d / 10000));
  // OS 版本（RtlGetVersion 不受兼容性清单影响）
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  OSVERSIONINFOEXW vi{}; vi.dwOSVersionInfoSize = sizeof vi;
  if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) fn((PRTL_OSVERSIONINFOW)&vi);
  logLine(fmt(L"[便携] Windows %lu.%lu build %lu（Win11 ≥ 22000）", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber));
  auto aw = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
  bool pmv2 = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  logLine(fmt(L"[便携] DPI 感知=%d（2=Per-Monitor），PerMonitorV2=%s，主窗口 DPI=%u，ACP=%u（65001=清单 UTF-8 生效）",
              (int)aw, pmv2 ? L"是" : L"否", dpiOf(gMain), GetACP()));
  // 目录可写
  std::wstring probe = exeDir() + L"\\.write-test.tmp";
  HANDLE hf = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  logLine(std::wstring(L"[便携] exe 目录可写：") + (hf != INVALID_HANDLE_VALUE ? L"是" : fmt(L"否（错误 %lu；便携版需把配置放到 %%APPDATA%%）", GetLastError())));
  if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
  // 网络下载标记（SmartScreen）
  HANDLE zi = CreateFileW((std::wstring(p) + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  logLine(std::wstring(L"[便携] Zone.Identifier（网络下载标记）：") + (zi != INVALID_HANDLE_VALUE ? L"有 —— 记录首次运行是否出现 SmartScreen 提示" : L"无"));
  if (zi != INVALID_HANDLE_VALUE) CloseHandle(zi);
  // 已加载模块：非系统目录的 DLL 都是要随包分发的依赖
  wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); std::wstring sysDir = sys, winDir;
  { wchar_t wd[MAX_PATH]; GetWindowsDirectoryW(wd, MAX_PATH); winDir = wd; }
  auto lower = [](std::wstring s) { for (auto& ch : s) ch = towlower(ch); return s; };
  HMODULE mods[1024]; DWORD need = 0; int nonSys = 0, total = 0;
  if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) {
      wchar_t mp[MAX_PATH * 2]; GetModuleFileNameW(mods[i], mp, (DWORD)std::size(mp)); ++total;
      std::wstring lm = lower(mp);
      if (i == 0) continue;
      if (lm.rfind(lower(winDir), 0) != 0) { ++nonSys; logLine(L"[便携]   非系统 DLL：" + std::wstring(mp)); }
    }
  }
  logLine(fmt(L"[便携] 已加载模块 %d 个，其中非 Windows 目录 %d 个（0 = 单 exe 即可运行）", total, nonSys));
  logLine(L"[便携] 本程序不写注册表、不写 %APPDATA%，只在 exe 目录（或 %TEMP%）写结果文件。");
  logLine(L"[便携] 记录：干净 Win11 / 普通用户 / U 盘或只读目录下能否直接启动；是否需要 VC++ 运行库或其它安装。");
}

// ---------------- 实验 3：分屏 20×5 最小网格 ----------------
struct SplitState {
  LayoutNode root = LayoutNode::split(SplitAxis::Columns, 0.5, LayoutNode::pane("A", ""),
                                      LayoutNode::split(SplitAxis::Rows, 0.5, LayoutNode::pane("B", ""), LayoutNode::pane("C", "")));
  int fontPt = 12;
  double cellW = 8, cellH = 16;
  int drag = -1;   // 0 = 根（竖分隔条），1 = 右侧（横分隔条）
  int violations = 0;
} gSplit;

void measureCell(HWND h) {
  HDC dc = GetDC(h);
  HFONT f = CreateFontW(-MulDiv(gSplit.fontPt, (int)dpiOf(h), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  HFONT of = (HFONT)SelectObject(dc, f); TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
  SIZE sz; GetTextExtentPoint32W(dc, L"M", 1, &sz);
  gSplit.cellW = sz.cx; gSplit.cellH = tm.tmHeight;
  SelectObject(dc, of); DeleteObject(f); ReleaseDC(h, dc);
}
Rect splitArea(HWND h) { RECT rc; GetClientRect(h, &rc); int top = sc(h, 48); return {0, double(top), double(rc.right), double(std::max(0L, rc.bottom - top))}; }

void splitReport(HWND h, const wchar_t* why) {
  std::wstring s = fmt(L"[分屏] %s：cell %.0f×%.0f px（Consolas %dpt, DPI %u）", why, gSplit.cellW, gSplit.cellH, gSplit.fontPt, dpiOf(h));
  bool bad = false;
  for (auto& pr : computeRects(gSplit.root, splitArea(h))) {
    int cols = (int)std::floor(pr.rect.w / gSplit.cellW), rows = (int)std::floor(pr.rect.h / gSplit.cellH);
    s += fmt(L"  %S=%d×%d", pr.paneId.c_str(), cols, rows);
    if (cols < 20 || rows < 5) bad = true;
  }
  if (bad) { ++gSplit.violations; s += L"  ← 低于 20×5（只应在窗口本身太小时出现）"; }
  logLine(s);
}

LRESULT CALLBACK SplitProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: measureCell(h); logLine(L"===== 实验 3：分屏 20×5 ====="); splitReport(h, L"初始"); return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        Rect a = splitArea(h); double x1 = a.x + a.w * gSplit.root.ratio, tol = sc(h, 4);
        if (std::abs(pt.x - x1) <= tol && pt.y >= a.y) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
        double y1 = a.y + a.h * gSplit.root.second->ratio;
        if (pt.x > x1 && std::abs(pt.y - y1) <= tol) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
      }
      break;
    case WM_LBUTTONDOWN: {
      Rect a = splitArea(h); int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      double x1 = a.x + a.w * gSplit.root.ratio, y1 = a.y + a.h * gSplit.root.second->ratio, tol = sc(h, 4);
      if (std::abs(x - x1) <= tol && y >= a.y) gSplit.drag = 0;
      else if (x > x1 && std::abs(y - y1) <= tol) gSplit.drag = 1;
      if (gSplit.drag >= 0) SetCapture(h);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (gSplit.drag >= 0) {
        Rect a = splitArea(h); int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (gSplit.drag == 0) {
          gSplit.root.ratio = clampRatio(gSplit.root, a, (x - a.x) / std::max(1.0, a.w), gSplit.cellW, gSplit.cellH);
        } else {
          double x1 = a.w * gSplit.root.ratio; Rect r{a.x + x1, a.y, a.w - x1, a.h};
          auto& sub = *gSplit.root.second;
          sub.ratio = clampRatio(sub, r, (y - r.y) / std::max(1.0, r.h), gSplit.cellW, gSplit.cellH);
        }
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: if (gSplit.drag >= 0) { gSplit.drag = -1; ReleaseCapture(); splitReport(h, L"拖动结束"); } return 0;
    case WM_KEYDOWN:
      if (w == VK_OEM_PLUS || w == VK_ADD) { gSplit.fontPt = std::min(72, gSplit.fontPt + 2); measureCell(h); splitReport(h, L"字号变大"); InvalidateRect(h, nullptr, FALSE); }
      if (w == VK_OEM_MINUS || w == VK_SUBTRACT) { gSplit.fontPt = std::max(6, gSplit.fontPt - 2); measureCell(h); splitReport(h, L"字号变小"); InvalidateRect(h, nullptr, FALSE); }
      return 0;
    case WM_EXITSIZEMOVE: splitReport(h, L"窗口缩放结束"); return 0;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
      auto* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      measureCell(h); splitReport(h, L"DPI 变化"); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      RECT rc; GetClientRect(h, &rc); FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
      HFONT f = uiFont(h), of = (HFONT)SelectObject(dc, f); SetBkMode(dc, TRANSPARENT);
      RECT top{sc(h, 8), sc(h, 4), rc.right - sc(h, 8), sc(h, 46)};
      centerText(dc, top, L"拖动竖/横分隔条到极限；缩小窗口；按 +/- 改字号。看：每个窗格是否始终 ≥ 20 列 × 5 行，拖不动时分隔条停住而不是跳动。");
      COLORREF cols[] = {RGB(30, 30, 30), RGB(20, 40, 70), RGB(60, 30, 30)}; int i = 0;
      for (auto& pr : computeRects(gSplit.root, splitArea(h))) {
        RECT r{(LONG)pr.rect.x, (LONG)pr.rect.y, (LONG)(pr.rect.x + pr.rect.w) - 2, (LONG)(pr.rect.y + pr.rect.h) - 2};
        HBRUSH b = CreateSolidBrush(cols[i++ % 3]); FillRect(dc, &r, b); DeleteObject(b);
        int c = (int)std::floor(pr.rect.w / gSplit.cellW), rw = (int)std::floor(pr.rect.h / gSplit.cellH);
        bool ok = c >= 20 && rw >= 5;
        SetTextColor(dc, ok ? RGB(120, 230, 120) : RGB(255, 90, 90));
        std::wstring t = fmt(L"窗格 %S\n%d 列 × %d 行 %s", pr.paneId.c_str(), c, rw, ok ? L"✔" : L"✘ 低于 20×5");
        RECT tr = r; InflateRect(&tr, -sc(h, 8), -sc(h, 8)); DrawTextW(dc, t.c_str(), (int)t.size(), &tr, DT_LEFT | DT_TOP);
      }
      SelectObject(dc, of); DeleteObject(f); EndPaint(h, &ps); return 0;
    }
    case WM_DESTROY: logLine(fmt(L"[分屏] 关闭。低于 20×5 的报告次数：%d", gSplit.violations)); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// ---------------- 实验 4：快捷键捕获 ----------------
struct KeyCheck { const wchar_t* name; bool hit = false; };
std::vector<KeyCheck> gKeys = {
  {L"Ctrl+Tab"}, {L"Ctrl+Shift+Tab"}, {L"Ctrl+Shift+T"}, {L"Alt+←"}, {L"Alt+→"}, {L"Alt+↑"}, {L"Alt+↓"},
  {L"F10（不应激活系统菜单）"}, {L"Alt 单独按下松开（不应激活系统菜单）"}, {L"Win+←（系统保留，预期收不到）"}, {L"Win+D/Win 单独（系统保留）"},
};
bool gAltAlone = false;
void hitKey(HWND h, int i) { if (!gKeys[i].hit) { gKeys[i].hit = true; logLine(L"[快捷键] ✔ 捕获：" + std::wstring(gKeys[i].name)); InvalidateRect(h, nullptr, FALSE); } }

LRESULT CALLBACK KeysProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: logLine(L"===== 实验 4：快捷键捕获 ====="); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
      bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN, sys = m == WM_SYSKEYDOWN || m == WM_SYSKEYUP;
      UINT vk = (UINT)w;
      logLine(fmt(L"[快捷键] %s%s %s%s 扫描码=0x%02X 扩展=%d 重复=%d", sys ? L"WM_SYS" : L"WM_", down ? L"KEYDOWN" : L"KEYUP",
                  modsNow().c_str(), vkName(vk, l).c_str(), (UINT)((l >> 16) & 0xff), (int)((l >> 24) & 1), (int)(l & 0xffff)));
      bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
      bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
      if (down) {
        if (vk == VK_MENU) { if (!(l & 0x40000000)) gAltAlone = true; }
        else if (vk != VK_SHIFT && vk != VK_CONTROL) gAltAlone = false;
        if (vk == VK_TAB && ctrl && !shift) hitKey(h, 0);
        if (vk == VK_TAB && ctrl && shift) hitKey(h, 1);
        if (vk == 'T' && ctrl && shift) hitKey(h, 2);
        if (alt && !ctrl) { if (vk == VK_LEFT) hitKey(h, 3); if (vk == VK_RIGHT) hitKey(h, 4); if (vk == VK_UP) hitKey(h, 5); if (vk == VK_DOWN) hitKey(h, 6); }
        if (vk == VK_F10) hitKey(h, 7);
        if (win && vk == VK_LEFT) hitKey(h, 9);
        if (vk == VK_LWIN || vk == VK_RWIN || (win && vk == 'D')) hitKey(h, 10);
      } else if (vk == VK_MENU && gAltAlone) { gAltAlone = false; hitKey(h, 8); }
      return 0;  // 吞掉：不交给 DefWindowProc（否则 F10/Alt 进入菜单模式）
    }
    case WM_SYSCHAR: logLine(fmt(L"[快捷键] WM_SYSCHAR 0x%04X（已吞掉，避免提示音/菜单助记符）", (UINT)w)); return 0;
    case WM_CHAR: logLine(fmt(L"[快捷键] WM_CHAR 0x%04X", (UINT)w)); return 0;
    case WM_SYSCOMMAND:
      if ((w & 0xFFF0) == SC_KEYMENU) { logLine(L"[快捷键] SC_KEYMENU 已拦截（系统菜单未激活）"); return 0; }
      break;
    case WM_KILLFOCUS: logLine(L"[快捷键] 失去焦点（若刚按下某组合键，说明被系统/其它程序截走）"); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      RECT rc; GetClientRect(h, &rc); FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
      HFONT f = uiFont(h, 11), of = (HFONT)SelectObject(dc, f); SetBkMode(dc, TRANSPARENT);
      int y = sc(h, 8);
      RECT t{sc(h, 8), y, rc.right - sc(h, 8), y + sc(h, 40)};
      centerText(dc, t, L"保持本窗口在前台，依次按下列组合。✔ = 程序收到；Win 组合预期收不到（由系统处理），也请记录系统行为。");
      y += sc(h, 46);
      for (auto& k : gKeys) {
        SetTextColor(dc, k.hit ? RGB(0, 140, 0) : RGB(90, 90, 90));
        std::wstring s = std::wstring(k.hit ? L"✔  " : L"☐  ") + k.name;
        TextOutW(dc, sc(h, 16), y, s.c_str(), (int)s.size()); y += sc(h, 24);
      }
      SelectObject(dc, of); DeleteObject(f); EndPaint(h, &ps); return 0;
    }
    case WM_DESTROY: {
      int n = 0; std::wstring miss;
      for (auto& k : gKeys) { if (k.hit) ++n; else miss += std::wstring(L" [") + k.name + L"]"; }
      logLine(fmt(L"[快捷键] 关闭。捕获 %d/%zu；未捕获：", n, gKeys.size()) + (miss.empty() ? L"无" : miss));
      return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

// ---------------- 主窗口 ----------------
enum { kBtnIme = 1, kBtnPortable, kBtnSplit, kBtnKeys, kBtnNote, kBtnOpen, kNote };
const wchar_t* kClsIme = L"it2w_probe_ime"; const wchar_t* kClsSplit = L"it2w_probe_split"; const wchar_t* kClsKeys = L"it2w_probe_keys";
HFONT gUiFont;

void openChild(const wchar_t* cls, const wchar_t* title, int w, int h) {
  HWND c = CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, sc(gMain, w), sc(gMain, h), nullptr, nullptr, gInst, nullptr);
  SetFocus(c);
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      gMain = h; gUiFont = uiFont(h, 10);
      const wchar_t* labels[] = {L"1 IME 候选窗", L"2 便携启动信息", L"3 分屏 20×5", L"4 快捷键捕获"};
      for (int i = 0; i < 4; ++i)
        SendMessageW(CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, (HMENU)(INT_PTR)(kBtnIme + i), gInst, nullptr), WM_SETFONT, (WPARAM)gUiFont, TRUE);
      gNote = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)kNote, gInst, nullptr);
      SendMessageW(gNote, EM_SETCUEBANNER, TRUE, (LPARAM)L"观察结论（如：候选窗遮住光标 / Ctrl+Tab 正常）→ 写入结果文件");
      SendMessageW(CreateWindowExW(0, L"BUTTON", L"写入结论", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 10, 10, h, (HMENU)kBtnNote, gInst, nullptr), WM_SETFONT, (WPARAM)gUiFont, TRUE);
      SendMessageW(CreateWindowExW(0, L"BUTTON", L"打开结果文件", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, (HMENU)kBtnOpen, gInst, nullptr), WM_SETFONT, (WPARAM)gUiFont, TRUE);
      gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, h, nullptr, gInst, nullptr);
      SendMessageW(gNote, WM_SETFONT, (WPARAM)gUiFont, TRUE); SendMessageW(gLog, WM_SETFONT, (WPARAM)gUiFont, TRUE);
      SendMessageW(gLog, EM_SETLIMITTEXT, 0, 0);
      return 0;
    }
    case WM_SIZE: {
      int W = LOWORD(l), H = HIWORD(l), bw = sc(h, 150), bh = sc(h, 30), g = sc(h, 8);
      for (int i = 0; i < 4; ++i) MoveWindow(GetDlgItem(h, kBtnIme + i), g + i * (bw + g), g, bw, bh, TRUE);
      int y2 = g * 2 + bh;
      MoveWindow(gNote, g, y2, std::max(10, W - 3 * g - 2 * sc(h, 110)), bh, TRUE);
      MoveWindow(GetDlgItem(h, kBtnNote), W - 2 * sc(h, 110) - g, y2, sc(h, 100), bh, TRUE);
      MoveWindow(GetDlgItem(h, kBtnOpen), W - sc(h, 110), y2, sc(h, 100), bh, TRUE);
      MoveWindow(gLog, g, y2 + bh + g, W - 2 * g, std::max(10, H - (y2 + bh + 2 * g)), TRUE);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case kBtnIme: openChild(kClsIme, L"实验 1：IME 候选窗定位", 760, 480); return 0;
        case kBtnPortable: portableReport(); return 0;
        case kBtnSplit: openChild(kClsSplit, L"实验 3：分屏 20×5 最小网格", 900, 560); return 0;
        case kBtnKeys: openChild(kClsKeys, L"实验 4：快捷键捕获", 620, 440); return 0;
        case kBtnNote: {
          int n = GetWindowTextLengthW(gNote); std::wstring s(n, L'\0'); GetWindowTextW(gNote, s.data(), n + 1);
          if (n) { logLine(L"[结论] " + s); SetWindowTextW(gNote, L""); }
          return 0;
        }
        case kBtnOpen: ShellExecuteW(h, L"open", gResultsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
      }
      break;
    case WM_DPICHANGED: {
      auto* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      logLine(fmt(L"[主窗口] DPI 变为 %u", HIWORD(w))); return 0;
    }
    case WM_DESTROY: logLine(L"探针退出。"); if (gFile) fclose(gFile); gFile = nullptr; PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

void reg(const wchar_t* cls, WNDPROC p, LPCWSTR cursor) {
  WNDCLASSEXW wc = {}; wc.cbSize = sizeof wc; wc.lpfnWndProc = p; wc.hInstance = gInst; wc.lpszClassName = cls;
  wc.hCursor = LoadCursorW(nullptr, cursor); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  gInst = inst;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // 清单已声明；兜底
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
  openResults();
  reg(L"it2w_probe_main", MainProc, IDC_ARROW); reg(kClsIme, ImeProc, IDC_IBEAM); reg(kClsSplit, SplitProc, IDC_ARROW); reg(kClsKeys, KeysProc, IDC_ARROW);
  HWND h = CreateWindowExW(0, L"it2w_probe_main", L"it2w 外壳选型探针（Win32 基线）", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 980, 640, nullptr, nullptr, inst, nullptr);
  if (!h) return 1;
  ShowWindow(h, show); UpdateWindow(h);
  logLine(L"结果文件：" + gResultsPath);
  portableReport();  // 实验 2 在启动时自动记录一次
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  return 0;
}
