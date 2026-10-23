#include "uicfg/reconnect.h"
namespace uicfg {
int PaneAttempts::reconnect(std::string out) {
  if (out.size() > budget_) {
    size_t cut = out.size() - budget_;
    while (cut < out.size() && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) ++cut;  // 不切断 UTF-8
    out.erase(0, cut);
  }
  old_ = OldAttempt{attempt_, std::move(out)};
  return ++attempt_;
}
std::string PaneAttempts::separator() const {
  if (!old_) return {};
  return "──── 重连 #" + std::to_string(attempt_) + "（以上为 attempt #" + std::to_string(old_->attemptId) + " 的只读输出）────";
}
}  // namespace uicfg
