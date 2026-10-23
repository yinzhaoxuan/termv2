#pragma once
// 第 12.3「点击重连」：每次重连新建 Session 并增加 attempt ID；
// 每窗格最多保留一个旧 attempt 的只读文本（受第 15.4 节预算限制）。
#include <cstddef>
#include <optional>
#include <string>

namespace uicfg {

struct OldAttempt {
  int attemptId = 0;
  std::string text;  // 只读，已按预算截断（保留尾部）
};

class PaneAttempts {
 public:
  explicit PaneAttempts(size_t oldTextBudgetBytes = 1 << 20) : budget_(oldTextBudgetBytes) {}
  int attemptId() const { return attempt_; }
  // 点击重连：当前 attempt 的输出转为只读旧文本（替换更早的），返回新 attempt ID
  int reconnect(std::string currentOutput);
  const std::optional<OldAttempt>& oldAttempt() const { return old_; }
  void dropOldAttempt() { old_.reset(); }
  // 新旧输出分隔行
  std::string separator() const;
 private:
  size_t budget_;
  int attempt_ = 1;
  std::optional<OldAttempt> old_;
};

}  // namespace uicfg
