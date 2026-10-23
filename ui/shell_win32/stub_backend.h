// 独立构建 ui/（未链接 it2w_pane_backend）时的占位后端：窗格直接进入 Failed，便于检查外壳本身。
#pragma once
#include "uishell/pane_backend.h"
namespace it2w_shell {
class StubBackend : public uishell::PaneBackend {
 public:
  explicit StubBackend(uishell::PaneEvents ev) : ev_(std::move(ev)) {}
  bool attach(void*, const uishell::PixelRect&, float) override {
    if (ev_.onStatus) ev_.onStatus(status());
    return true;
  }
  void setBounds(const uishell::PixelRect&, float) override {}
  void setFocused(bool) override {}
  void sendInput(std::string_view) override {}
  void paste(std::string_view) override {}
  uishell::CaretRect caretRect() const override { return {0, 0, 0, 0, false}; }
  uishell::SessionStatus status() const override {
    return {uishell::SessionState::Failed, 0, false, "", "此构建未链接终端后端（仅外壳）"};
  }
  void scrollViewport(int) override {}
  void scrollToBottom() override {}
  std::string selectedText() const override { return {}; }
  void clearSelection() override {}
  void requestClose() override {}
 private:
  uishell::PaneEvents ev_;
};
inline uishell::PaneBackendFactory makeStubFactory() {
  return [](const std::string&, uishell::PaneEvents ev) { return std::make_unique<StubBackend>(std::move(ev)); };
}
}  // namespace it2w_shell
