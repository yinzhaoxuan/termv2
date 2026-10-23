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
struct CaretRect { int x, y, w, h; bool valid; };      // renderer caret_rect_px(), client px

enum class SessionState { Starting, Running, Reconnecting, Exited, Failed };
struct SessionStatus {
  SessionState state;
  int exitCode;              // valid when Exited
  bool hasForegroundJob;     // drives 12.3 close confirmation
  std::string title;         // already rate-limited (<=10/s)
  std::string errorText;     // classified error for Failed (T07 categories)
};

struct PaneEvents {                       // called on the UI thread only
  std::function<void(const SessionStatus&)> onStatus;
  std::function<void()> onBell;           // visual, <=1/s
  std::function<void(std::string url)> onLinkActivated;  // UI runs uicfg::checkLink
  std::function<void()> onRedrawRequested;
};

class PaneBackend {
public:
  virtual ~PaneBackend() = default;
  // parentHwnd: shell-owned child window for this pane; backend creates the swap chain on it.
  virtual bool attach(void* parentHwnd, const PixelRect& rc, float dpiScale) = 0;
  virtual void setBounds(const PixelRect& rc, float dpiScale) = 0; // backend recomputes grid, enforces >=20x5
  virtual void setFocused(bool focused) = 0;
  virtual void sendInput(std::string_view utf8) = 0;               // keys already translated by UI
  virtual void paste(std::string_view utf8) = 0;                   // bracketed-paste aware
  virtual CaretRect caretRect() const = 0;                         // for IME candidate window
  virtual SessionStatus status() const = 0;
  virtual void requestClose() = 0;  // after UI confirmation; backend runs the T03A close order
};

// connectionId: uicfg Connection::id; empty means local shell profile.
using PaneBackendFactory =
    std::function<std::unique_ptr<PaneBackend>(const std::string& connectionId, PaneEvents ev)>;

}  // namespace uishell
