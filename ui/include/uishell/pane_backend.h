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

struct PaneEvents {                       // called on the UI thread only
  std::function<void(const SessionStatus&)> onStatus;
  std::function<void()> onBell;           // visual, <=1/s
  std::function<void(std::string url)> onLinkActivated;  // UI runs uicfg::checkLink
  std::function<void(int offsetFromBottom, int historyLines)> onViewportChanged; // scrollbar
  std::function<void(size_t bytes, size_t limit)> onPasteRejected;
  std::function<void(bool hasSelection)> onSelectionChanged;
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
  virtual void requestClose() = 0;
};

// connectionId: uicfg Connection::id; empty means local shell profile.
using PaneBackendFactory =
    std::function<std::unique_ptr<PaneBackend>(const std::string& connectionId, PaneEvents ev)>;

}  // namespace uishell
