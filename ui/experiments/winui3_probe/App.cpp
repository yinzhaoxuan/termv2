// T08 外壳选型实验：WinUI 3 探针（纯代码 C++/WinRT，非打包，自包含）。未在云端构建。
// 四个实验与 win32_probe 相同；结果写入 exe 同目录 winui3-probe-results-*.txt（不可写时 %TEMP%）。
#include "pch.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
namespace muxm = winrt::Microsoft::UI::Xaml::Markup;
namespace wuxi = winrt::Windows::UI::Xaml::Interop;
using winrt::Windows::System::VirtualKey;

namespace {
FILE* gFile = nullptr;
std::wstring gPath;
TextBox gLog{nullptr};
std::vector<Window> gWindows;  // 防止子窗口被释放

std::wstring exeDir() {
  wchar_t p[MAX_PATH * 4]; DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 4);
  std::wstring s(p, n); return s.substr(0, s.find_last_of(L"\\/"));
}
std::string u8(std::wstring const& w) { return winrt::to_string(w); }
std::wstring fmt(const wchar_t* f, ...) { wchar_t b[2048]; va_list a; va_start(a, f); _vsnwprintf_s(b, _TRUNCATE, f, a); va_end(a); return b; }

void logLine(std::wstring const& s) {
  SYSTEMTIME t; GetLocalTime(&t);
  auto line = fmt(L"[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds) + s;
  if (gFile) { auto u = u8(line) + "\r\n"; fwrite(u.data(), 1, u.size(), gFile); fflush(gFile); }
  if (gLog) { gLog.Text(gLog.Text() + line + L"\r\n"); gLog.SelectionStart(static_cast<int>(gLog.Text().size())); }
}

HWND hwndOf(Window const& w) { HWND h{}; w.as<::IWindowNative>()->get_WindowHandle(&h); return h; }

std::wstring vkName(VirtualKey k) {
  switch (k) {
    case VirtualKey::Tab: return L"Tab"; case VirtualKey::Menu: return L"Alt"; case VirtualKey::LeftMenu: return L"LAlt"; case VirtualKey::RightMenu: return L"RAlt";
    case VirtualKey::Left: return L"←"; case VirtualKey::Right: return L"→"; case VirtualKey::Up: return L"↑"; case VirtualKey::Down: return L"↓";
    case VirtualKey::F10: return L"F10"; case VirtualKey::LeftWindows: return L"LWin"; case VirtualKey::RightWindows: return L"RWin";
    default: return fmt(L"VK_0x%02X", static_cast<int>(k));
  }
}
bool down(VirtualKey k) {
  using namespace winrt::Windows::UI::Core;
  return (winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(k) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
}
std::wstring mods() {
  std::wstring s;
  if (down(VirtualKey::Control)) s += L"Ctrl+"; if (down(VirtualKey::Shift)) s += L"Shift+"; if (down(VirtualKey::Menu)) s += L"Alt+";
  if (down(VirtualKey::LeftWindows) || down(VirtualKey::RightWindows)) s += L"Win+";
  return s;
}

Window newWindow(std::wstring const& title, int w, int h, UIElement const& content) {
  Window win; win.Title(title); win.Content(content);
  win.AppWindow().Resize({w, h});
  win.Activate(); gWindows.push_back(win); return win;
}

// ---------- 实验 1：IME ----------
// 终端没有原生文本框；WinUI 3 桌面上 CoreTextEditContext 依赖 CoreWindow 不可用，常见做法是
// 把一个透明 TextBox 移到光标处接收 IME。这里同时测：(a) TextBox 位置是否决定候选窗；(b) HWND 上 IMM 强制定位。
void openIme() {
  Canvas canvas; canvas.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
  TextBlock help; help.Text(L"点击放置模拟光标（红框）后直接输入中文。看：候选窗是否贴在红框下方。F2 切换 IMM 强制定位开/关。"); help.TextWrapping(TextWrapping::Wrap); help.Width(700);
  Canvas::SetLeft(help, 8); Canvas::SetTop(help, 8); canvas.Children().Append(help);
  Border caret; caret.Width(2); caret.Height(24); caret.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::Red()));
  Canvas::SetLeft(caret, 120); Canvas::SetTop(caret, 160); canvas.Children().Append(caret);
  TextBox sink; sink.Width(40); sink.Height(24); sink.Opacity(0.01); sink.BorderThickness({0, 0, 0, 0}); sink.Padding({0, 0, 0, 0});
  Canvas::SetLeft(sink, 120); Canvas::SetTop(sink, 160); canvas.Children().Append(sink);
  TextBlock out; Canvas::SetLeft(out, 8); Canvas::SetTop(out, 400); canvas.Children().Append(out);
  auto win = newWindow(L"WinUI 3 实验 1：IME 候选窗定位", 760, 480, canvas);
  HWND hwnd = hwndOf(win);
  auto st = std::make_shared<std::tuple<int, std::wstring, bool>>(0, L"", false);  // 提交次数、已提交、IMM 强制
  auto applyImm = [hwnd, caret, st, canvas](const wchar_t* why) {
    if (!std::get<2>(*st)) return;
    double scale = canvas.XamlRoot() ? canvas.XamlRoot().RasterizationScale() : 1.0;
    LONG x = static_cast<LONG>(Canvas::GetLeft(caret) * scale), y = static_cast<LONG>(Canvas::GetTop(caret) * scale), hh = static_cast<LONG>(24 * scale);
    // 注意：IME 实际挂在 WinUI 的子输入窗口上，顶层 HWND 的 IMC 可能无效 —— 这正是要记录的结论之一
    HWND focus = GetFocus(); HWND target = focus ? focus : hwnd;
    HIMC imc = ImmGetContext(target);
    if (!imc) { logLine(fmt(L"[WinUI IME] %s：ImmGetContext(0x%p) 失败", why, target)); return; }
    COMPOSITIONFORM cf{CFS_FORCE_POSITION, {x, y}, {}}; ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cd{0, CFS_EXCLUDE, {x, y + hh}, {x, y, x + 2, y + hh}}; BOOL ok = ImmSetCandidateWindow(imc, &cd);
    ImmReleaseContext(target, imc);
    logLine(fmt(L"[WinUI IME] %s：IMM 强制定位 HWND=0x%p (%ld,%ld) scale=%.2f ok=%d", why, target, x, y, scale, ok));
  };
  canvas.PointerPressed([=](auto&&, PointerRoutedEventArgs const& e) {
    auto p = e.GetCurrentPoint(canvas).Position();
    Canvas::SetLeft(caret, p.X); Canvas::SetTop(caret, p.Y); Canvas::SetLeft(sink, p.X); Canvas::SetTop(sink, p.Y);
    sink.Focus(FocusState::Programmatic);
    logLine(fmt(L"[WinUI IME] 光标移到 DIP(%.0f,%.0f)", p.X, p.Y)); applyImm(L"移动光标");
  });
  sink.TextCompositionStarted([=](auto&&, auto&&) { logLine(L"[WinUI IME] TextCompositionStarted"); applyImm(L"开始组字"); });
  sink.TextCompositionChanged([=](auto&&, auto&&) { applyImm(L"组字更新"); });
  sink.TextCompositionEnded([=](auto&&, auto&&) { logLine(L"[WinUI IME] TextCompositionEnded"); });
  sink.TextChanged([=](auto&&, auto&&) {
    auto t = std::wstring(sink.Text());
    if (t.empty()) return;
    ++std::get<0>(*st); std::get<1>(*st) += t;
    logLine(fmt(L"[WinUI IME] 提交 #%d：「%s」", std::get<0>(*st), t.c_str()));
    out.Text(fmt(L"已提交 %d 次：%s", std::get<0>(*st), std::get<1>(*st).c_str()));
    sink.Text(L"");
  });
  sink.PreviewKeyDown([=](auto&&, KeyRoutedEventArgs const& e) {
    if (e.Key() == VirtualKey::F2) { std::get<2>(*st) = !std::get<2>(*st); logLine(std::wstring(L"[WinUI IME] IMM 强制定位：") + (std::get<2>(*st) ? L"开" : L"关")); e.Handled(true); }
  });
  sink.Focus(FocusState::Programmatic);
  logLine(L"===== WinUI 实验 1：IME =====");
}

// ---------- 实验 2：便携 ----------
void portableReport(Window const& main) {
  logLine(L"===== WinUI 实验 2：便携启动 =====");
  wchar_t p[MAX_PATH * 4]; GetModuleFileNameW(nullptr, p, MAX_PATH * 4); logLine(std::wstring(L"[WinUI 便携] exe：") + p);
  FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); FILETIME now; GetSystemTimeAsFileTime(&now);
  ULONGLONG d = ((ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime) - ((ULONGLONG(c.dwHighDateTime) << 32) | c.dwLowDateTime);
  logLine(fmt(L"[WinUI 便携] 进程创建到现在：%llu ms", d / 10000));
  logLine(fmt(L"[WinUI 便携] 主窗口 DPI=%u", GetDpiForWindow(hwndOf(main))));
  HANDLE hf = CreateFileW((exeDir() + L"\\.write-test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  logLine(std::wstring(L"[WinUI 便携] exe 目录可写：") + (hf != INVALID_HANDLE_VALUE ? L"是" : L"否")); if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
  wchar_t wd[MAX_PATH]; GetWindowsDirectoryW(wd, MAX_PATH); std::wstring win = wd; for (auto& ch : win) ch = towlower(ch);
  HMODULE mods[2048]; DWORD need = 0; int non = 0;
  if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
    for (DWORD i = 1; i < need / sizeof(HMODULE); ++i) {
      wchar_t mp[MAX_PATH * 2]; GetModuleFileNameW(mods[i], mp, MAX_PATH * 2); std::wstring l = mp; for (auto& ch : l) ch = towlower(ch);
      if (l.rfind(win, 0) != 0) { ++non; logLine(std::wstring(L"[WinUI 便携]   随包 DLL：") + mp); }
    }
  logLine(fmt(L"[WinUI 便携] 非系统 DLL %d 个。记录：干净 Win11 无 Windows App Runtime 时能否直接启动、整个目录大小。", non));
}

// ---------- 实验 3：分屏 ----------
void openSplit() {
  TextBlock probe; probe.FontFamily(FontFamily(L"Consolas")); probe.FontSize(16); probe.Text(L"MMMMMMMMMM");
  probe.Measure({10000, 10000}); double cw = probe.DesiredSize().Width / 10, ch = probe.DesiredSize().Height;
  auto mkPane = [](std::wstring n, winrt::Windows::UI::Color c) { Border b; b.Background(SolidColorBrush(c)); TextBlock t; t.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::LightGreen())); t.Margin({8, 8, 8, 8}); b.Child(t); b.Tag(box_value(n)); return b; };
  Grid root; root.ColumnDefinitions().Append(ColumnDefinition()); root.ColumnDefinitions().Append(ColumnDefinition()); root.ColumnDefinitions().Append(ColumnDefinition());
  root.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromPixels(6));
  for (int i : {0, 2}) root.ColumnDefinitions().GetAt(i).MinWidth(20 * cw);
  Grid right; for (int i = 0; i < 3; ++i) right.RowDefinitions().Append(RowDefinition());
  right.RowDefinitions().GetAt(1).Height(GridLengthHelper::FromPixels(6));
  for (int i : {0, 2}) right.RowDefinitions().GetAt(i).MinHeight(5 * ch);
  auto a = mkPane(L"A", {255, 30, 30, 30}), b = mkPane(L"B", {255, 20, 40, 70}), c = mkPane(L"C", {255, 60, 30, 30});
  Border vdiv; vdiv.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::Gray())); Border hdiv; hdiv.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
  Grid::SetColumn(a, 0); Grid::SetColumn(vdiv, 1); Grid::SetColumn(right, 2); Grid::SetRow(b, 0); Grid::SetRow(hdiv, 1); Grid::SetRow(c, 2);
  root.Children().Append(a); root.Children().Append(vdiv); root.Children().Append(right); right.Children().Append(b); right.Children().Append(hdiv); right.Children().Append(c);
  auto label = [cw, ch](Border const& p) {
    int cols = int(p.ActualWidth() / cw), rows = int(p.ActualHeight() / ch); bool ok = cols >= 20 && rows >= 5;
    p.Child().as<TextBlock>().Text(fmt(L"窗格 %s\n%d 列 × %d 行 %s", unbox_value<hstring>(p.Tag()).c_str(), cols, rows, ok ? L"✔" : L"✘ 低于 20×5"));
    return fmt(L" %s=%d×%d%s", unbox_value<hstring>(p.Tag()).c_str(), cols, rows, ok ? L"" : L"←违规");
  };
  for (auto p : {a, b, c}) p.SizeChanged([=](auto&&, auto&&) { label(p); });
  // 分隔条拖动：直接改 Star 比例，MinWidth/MinHeight 负责 20×5 下限
  auto drag = std::make_shared<int>(-1);
  auto start = [drag](Border const& d, int which) {
    d.PointerPressed([d, drag, which](auto&&, PointerRoutedEventArgs const& e) { *drag = which; d.CapturePointer(e.Pointer()); });
    d.PointerReleased([d, drag](auto&&, PointerRoutedEventArgs const& e) { *drag = -1; d.ReleasePointerCapture(e.Pointer()); });
  };
  start(vdiv, 0); start(hdiv, 1);
  auto move = [=](Border const& d) {
    d.PointerMoved([=](auto&&, PointerRoutedEventArgs const& e) {
      if (*drag == 0) { double x = e.GetCurrentPoint(root).Position().X, W = root.ActualWidth(); root.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(std::max(1.0, x), GridUnitType::Star)); root.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::FromValueAndType(std::max(1.0, W - x), GridUnitType::Star)); }
      if (*drag == 1) { double y = e.GetCurrentPoint(right).Position().Y, H = right.ActualHeight(); right.RowDefinitions().GetAt(0).Height(GridLengthHelper::FromValueAndType(std::max(1.0, y), GridUnitType::Star)); right.RowDefinitions().GetAt(2).Height(GridLengthHelper::FromValueAndType(std::max(1.0, H - y), GridUnitType::Star)); }
    });
  };
  move(vdiv); move(hdiv);
  root.PointerReleased([=](auto&&, auto&&) { logLine(fmt(L"[WinUI 分屏] cell %.1f×%.1f DIP", cw, ch) + label(a) + label(b) + label(c)); });
  auto w = newWindow(L"WinUI 3 实验 3：分屏 20×5（Grid Min 约束）", 900, 560, root);
  w.SizeChanged([=](auto&&, auto&&) { logLine(L"[WinUI 分屏] 窗口尺寸变化" + label(a) + label(b) + label(c)); });
  logLine(L"===== WinUI 实验 3：分屏 =====");
}

// ---------- 实验 4：快捷键 ----------
LRESULT CALLBACK rawSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP)
    logLine(fmt(L"[WinUI 原始] HWND=0x%p msg=0x%04X vk=0x%02X（顶层窗口收到）", h, m, (UINT)w));
  if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) { logLine(L"[WinUI 原始] SC_KEYMENU 已拦截"); return 0; }
  return DefSubclassProc(h, m, w, l);
}
void openKeys() {
  StackPanel panel; panel.Padding({16, 16, 16, 16}); panel.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
  panel.IsTabStop(true); panel.AllowFocusOnInteraction(true);
  std::vector<std::wstring> names = {L"Ctrl+Tab", L"Ctrl+Shift+Tab", L"Ctrl+Shift+T", L"Alt+←", L"Alt+→", L"Alt+↑", L"Alt+↓", L"F10", L"Alt 单独", L"Win+←（预期收不到）", L"Win 单独（预期收不到）"};
  auto items = std::make_shared<std::vector<TextBlock>>();
  TextBlock help; help.Text(L"保持本窗口在前台依次按下列组合。✔ = PreviewKeyDown 收到。"); panel.Children().Append(help);
  for (auto& n : names) { TextBlock t; t.Text(L"☐  " + n); panel.Children().Append(t); items->push_back(t); }
  auto hits = std::make_shared<std::vector<bool>>(names.size(), false);
  auto hit = [=](size_t i) { if (!(*hits)[i]) { (*hits)[i] = true; (*items)[i].Text(L"✔  " + names[i]); logLine(L"[WinUI 快捷键] ✔ " + names[i]); } };
  auto altAlone = std::make_shared<bool>(false);
  auto handler = [=](auto&&, KeyRoutedEventArgs const& e) {
    auto k = e.Key(); bool ctrl = down(VirtualKey::Control), shift = down(VirtualKey::Shift), alt = down(VirtualKey::Menu);
    bool win = down(VirtualKey::LeftWindows) || down(VirtualKey::RightWindows);
    logLine(fmt(L"[WinUI 快捷键] PreviewKeyDown %s%s 原始VK=0x%02X 扫描码=0x%02X 菜单键=%d", mods().c_str(), vkName(k).c_str(), (int)e.OriginalKey(), e.KeyStatus().ScanCode, e.KeyStatus().IsMenuKeyDown));
    *altAlone = (k == VirtualKey::Menu || k == VirtualKey::LeftMenu || k == VirtualKey::RightMenu) && !e.KeyStatus().WasKeyDown ? true : (k == VirtualKey::Menu || k == VirtualKey::LeftMenu || k == VirtualKey::RightMenu) && *altAlone;
    if (k == VirtualKey::Tab && ctrl) hit(shift ? 1 : 0);
    if (k == VirtualKey::T && ctrl && shift) hit(2);
    if (alt && !ctrl) { if (k == VirtualKey::Left) hit(3); if (k == VirtualKey::Right) hit(4); if (k == VirtualKey::Up) hit(5); if (k == VirtualKey::Down) hit(6); }
    if (k == VirtualKey::F10) hit(7);
    if (win && k == VirtualKey::Left) hit(9);
    if (k == VirtualKey::LeftWindows || k == VirtualKey::RightWindows) hit(10);
    e.Handled(true);  // 阻止 XAML 焦点导航 / 访问键
  };
  panel.PreviewKeyDown(handler);
  panel.PreviewKeyUp([=](auto&&, KeyRoutedEventArgs const& e) {
    auto k = e.Key();
    if ((k == VirtualKey::Menu || k == VirtualKey::LeftMenu || k == VirtualKey::RightMenu) && *altAlone) { *altAlone = false; hit(8); }
    e.Handled(true);
  });
  panel.LostFocus([](auto&&, auto&&) { logLine(L"[WinUI 快捷键] 失去焦点"); });
  auto w = newWindow(L"WinUI 3 实验 4：快捷键捕获", 620, 440, panel);
  SetWindowSubclass(hwndOf(w), rawSub, 1, 0);
  panel.Loaded([panel](auto&&, auto&&) { panel.Focus(FocusState::Programmatic); });
  w.Closed([=](auto&&, auto&&) {
    int n = 0; std::wstring miss; for (size_t i = 0; i < names.size(); ++i) { if ((*hits)[i]) ++n; else miss += L" [" + names[i] + L"]"; }
    logLine(fmt(L"[WinUI 快捷键] 捕获 %d/%zu；未捕获：", n, names.size()) + (miss.empty() ? L"无" : miss));
  });
  logLine(L"===== WinUI 实验 4：快捷键 =====");
}

struct App : ApplicationT<App, muxm::IXamlMetadataProvider> {
  void OnLaunched(LaunchActivatedEventArgs const&) {
    Resources().MergedDictionaries().Append(XamlControlsResources());
    SYSTEMTIME t; GetLocalTime(&t);
    auto name = fmt(L"winui3-probe-results-%04d%02d%02d-%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    gPath = exeDir() + L"\\" + name;
    if (_wfopen_s(&gFile, gPath.c_str(), L"wb") != 0) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); gPath = std::wstring(tmp) + name; _wfopen_s(&gFile, gPath.c_str(), L"wb"); }
    if (gFile) fwrite("\xEF\xBB\xBF", 1, 3, gFile);

    Grid g; g.Padding({8, 8, 8, 8}); g.RowSpacing(8);
    for (int i = 0; i < 3; ++i) g.RowDefinitions().Append(RowDefinition());
    g.RowDefinitions().GetAt(0).Height(GridLengthHelper::Auto()); g.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());
    StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8);
    auto btn = [&](hstring text, std::function<void()> f) { Button b; b.Content(box_value(text)); b.Click([f](auto&&, auto&&) { f(); }); row.Children().Append(b); };
    btn(L"1 IME 候选窗", [] { openIme(); });
    btn(L"2 便携启动信息", [this] { portableReport(window); });
    btn(L"3 分屏 20×5", [] { openSplit(); });
    btn(L"4 快捷键捕获", [] { openKeys(); });
    StackPanel notes; notes.Orientation(Orientation::Horizontal); notes.Spacing(8);
    TextBox note; note.Width(600); note.PlaceholderText(L"观察结论 → 写入结果文件"); notes.Children().Append(note);
    Button wb; wb.Content(box_value(L"写入结论")); wb.Click([note](auto&&, auto&&) { if (!note.Text().empty()) { logLine(L"[结论] " + std::wstring(note.Text())); note.Text(L""); } }); notes.Children().Append(wb);
    gLog = TextBox(); gLog.IsReadOnly(true); gLog.AcceptsReturn(true); gLog.TextWrapping(TextWrapping::Wrap);
    ScrollViewer::SetVerticalScrollBarVisibility(gLog, Controls::ScrollBarVisibility::Auto);
    Grid::SetRow(row, 0); Grid::SetRow(notes, 1); Grid::SetRow(gLog, 2);
    g.Children().Append(row); g.Children().Append(notes); g.Children().Append(gLog);
    window = Window(); window.Title(L"it2w 外壳选型探针（WinUI 3）"); window.Content(g); window.AppWindow().Resize({980, 640}); window.Activate();
    window.Closed([](auto&&, auto&&) { logLine(L"探针退出。"); if (gFile) fclose(gFile); gFile = nullptr; });
    logLine(L"结果文件：" + gPath);
    portableReport(window);
  }
  muxm::IXamlType GetXamlType(wuxi::TypeName const& type) { return provider.GetXamlType(type); }
  muxm::IXamlType GetXamlType(hstring const& fullName) { return provider.GetXamlType(fullName); }
  com_array<muxm::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }
  Window window{nullptr};
  winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
};
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  Application::Start([](auto&&) { make<App>(); });
  return 0;
}
