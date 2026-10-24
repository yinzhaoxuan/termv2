#pragma once
// T08 shell <-> backend contract. UI owns this header; 架构师 implements PaneBackend
// on top of win32_term / ISwapChainHost / SessionTransport. Platform-neutral on purpose
// (HWND passed as void*), so UI state models stay testable on Linux.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace uishell {

struct PixelRect { int x, y, w, h; };
struct CellSize { int w, h; };   // physical px, from renderer GridMetrics::from_font
struct CaretRect { int x, y, w, h; bool valid; };      // renderer caret_rect_px(), client px

enum class SessionState { Starting, Running, Reconnecting, Exited, Failed };
struct SessionStatus {
  SessionState state;
  int exitCode;              // valid when Exited
  bool hasForegroundJob;     // informational only; 12.3 confirm uses state (Running/Starting/Reconnecting => confirm)
  std::string title;         // already rate-limited (<=10/s)
  std::string errorText;     // classified error for Failed (T07 categories)
};

// ---- 滚动历史搜索（草案 /workspace/qa/itcore_search-草案-v0.2.md §1.6 映射；架构师拍板 v0.1：纯文本、无正则）----
// 大小写：对应 ITCORE_SEARCH_CASE_*；Smart = needle 含大写则区分，否则不区分
enum class SearchCase { Sensitive, Insensitive, Smart };
struct SearchOptions {
  SearchCase caseMode = SearchCase::Smart;
  bool regex = false;        // v0.1 不支持：后端遇到 true 应返回 found=false（不要退回子串搜索）
  bool wrapAround = true;    // ITCORE_SEARCH_F_WRAP
};
struct SearchResult {
  bool found = false;        // next/prev 返回 DONE/RUNNING
  int index = 0;             // 当前结果在文档顺序（上→下）中的 0 基序号（草案 *index；UI 显示 +1）；found=false 时无意义
  int total = -1;            // prog.total；-1 = 还在分批计数（RUNNING），DONE 后为精确值
  bool hasNewOutput = false; // PF_NEW_CONTENT：begin 之后有新输出，不在本轮范围（v0.1 不做 tail find）
  bool restarted = false;    // ITCORE_SEARCH_RESTARTED（v0.2 失效事件，只报一次）：resize / 主备屏切换 / ED3 / RIS，
                             // 渲染清高亮层，UI 用同一 needle 重新搜
  bool pending = false;      // 草案 PENDING：已扫区域里还没有、扫描未完（found=false）；结果稍后经 onSearchUpdated 给出
};

struct PaneEvents {                       // called on the UI thread only
  std::function<void(const SessionStatus&)> onStatus;
  std::function<void()> onBell;           // visual, <=1/s
  std::function<void(std::string url)> onLinkActivated;  // UI runs uicfg::checkLink
  std::function<void(int offsetFromBottom, int historyLines)> onViewportChanged; // scrollbar
  std::function<void(size_t bytes, size_t limit)> onPasteRejected;
  std::function<void(bool hasSelection)> onSelectionChanged;
  // 可选：搜索状态异步变化（分批计数推进 total、PENDING 落定为 found、新输出、RESTART）。空 = 不通知
  std::function<void(const SearchResult&)> onSearchUpdated;
};

class PaneBackend {
public:
  virtual ~PaneBackend() = default;
  // parentHwnd: shell-owned child window for this pane; backend creates the swap chain on it.
  virtual bool attach(void* parentHwnd, const PixelRect& rc, float dpiScale) = 0;
  virtual void setBounds(const PixelRect& rc, float dpiScale) = 0; // backend recomputes grid, enforces >=20x5
  virtual void setFocused(bool focused) = 0;
  virtual void sendInput(std::string_view utf8) = 0;               // committed text (WM_CHAR / IME result)
  // Special keys (arrows, Home/End, PgUp/PgDn, F1-F24, Ins/Del, keypad, Enter/Tab/Backspace/Esc with mods,
  // Alt+char as ESC prefix). Backend encodes via itcore_encode_key (DECCKM/DECKPAM/xterm modifiers).
  // UI never calls this while IME is composing (VK_PROCESSKEY), and sends AltGr chars that ToUnicode
  // translates as text via sendInput instead.
  enum KeyMods : unsigned { ModShift = 1, ModAlt = 2, ModCtrl = 4 };
  virtual void sendKey(unsigned vk, unsigned mods) = 0;
  virtual void paste(std::string_view utf8) = 0;                   // bracketed-paste aware
  virtual CaretRect caretRect() const = 0;                         // for IME candidate window
  virtual SessionStatus status() const = 0;
  // 12.3 reconnect: read-only logical text of the previous attempt (at most one kept, budgeted
  // per 15.4). previousAttemptId()==0 means none. UI shows it above a separator, not clickable.
  virtual std::uint64_t previousAttemptId() const = 0;
  virtual std::string previousAttemptText() const = 0;
  virtual void reconnect() = 0;   // new core + process, attempt id +1
  // Selection is hit-tested and drawn by the backend (it owns the grid); UI owns the copy command.
  // Mouse (wheel, click, double=word, triple=line, drag edge auto-scroll, mouse modes) is handled
  // entirely by the backend in the pane child window; UI never forwards mouse messages.
  // Keyboard/menu scroll commands from UI (Shift+PgUp etc.):
  virtual void scrollViewport(int lines) = 0;   // negative = into history; clamps
  virtual void scrollToBottom() = 0;            // also on user input
  virtual std::string selectedText() const = 0;     // empty if none
  virtual void clearSelection() = 0;
  // After UI confirmation. MUST NOT block the UI thread: start the T03A close order and return;
  // completion is reported via onStatus(Exited/Failed).
  // Runtime font change (STATUS 第 4 批提案): backend rebuilds DWrite format, clears glyph/advance
  // caches, recomputes GridMetrics, then set_cell_pixel_size -> resize(gen). Async; grid change
  // arrives via onStatus. sizePt clamped 6..72 by UI. Empty family = keep current.
  virtual void setFont(std::string_view family, float sizePt) { (void)family; (void)sizePt; }
  virtual CellSize cellSize() const { return {0, 0}; }  // {0,0} = not yet known
  // 滚动历史搜索（UI 线程）。needle 或选项与上次不同 → 新一轮搜索（草案 end+begin），否则在当前结果上移动：
  // backward=true 向上（更早，草案 prev），false 向下（next）。找到后后端滚动视口使当前结果可见；高亮由渲染
  // 的独立层画（草案 §2.6，ranges_in_view，不经 UI）。主屏搜历史+网格，备屏只搜备屏网格。空 needle → found=false。
  virtual SearchResult findInScrollback(std::string_view needleUtf8, const SearchOptions& opt, bool backward) {
    (void)needleUtf8; (void)opt; (void)backward; return {};
  }
  virtual void clearSearch() {}   // 结束本轮搜索（itcore_search_end），清高亮层；视口不动
  virtual void requestClose() = 0;
};

// connectionId: uicfg Connection::id; empty means local shell profile.
using PaneBackendFactory =
    std::function<std::unique_ptr<PaneBackend>(const std::string& connectionId, PaneEvents ev)>;

}  // namespace uishell
