#pragma once
// UI 与 pane 后端共用的上限常量（设计文档 §15.4）。只改这里。
#include <cstddef>
namespace uishell {
inline constexpr std::size_t kPrevAttemptTextBytes = 8u * 1024u * 1024u;
}
