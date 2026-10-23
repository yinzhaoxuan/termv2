// 后端公共上限常量（不依赖 ui/；UI 可 include 后 using）
#pragma once
#include <cstddef>

namespace it2app {
// 旧 attempt 只读文本预算（设计 §15.4：每 Pane 一个，最多 8 MiB）
inline constexpr std::size_t kPrevAttemptTextBytes = 8u * 1024u * 1024u;
}  // namespace it2app
