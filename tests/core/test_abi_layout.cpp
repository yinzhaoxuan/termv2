// SPDX-License-Identifier: GPL-3.0-or-later
// ABI-02：64 位公开 ABI 基线，预期字面量由当前头文件和编译器实测。
// 断言失败时按 ADR-005 §4 判断兼容性；不兼容则递增 ABI 版本，更新断言表，
// 并在提交信息中注明「ABI：兼容 / 不兼容（版本 N→N+1）」。
#include "itcore.h"
#include <cstdio>

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
// itcore_core、itcore_snapshot、itcore_event、itcore_search 为不透明句柄，无公开布局。

// ABI 版本与公开常量宏
static_assert(ITCORE_ABI_VERSION == 0);
static_assert(ITCORE_CELL_UL_STYLE_SHIFT == 10);
static_assert(ITCORE_CELL_UL_STYLE_MASK == 7168);
static_assert(ITCORE_HAS_SGR_EXT_FLAGS == 1);
static_assert(ITCORE_CLIPBOARD_DEFAULT_MAX == 102400);

// itcore_buffer：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_buffer) == 24);
static_assert(offsetof(itcore_buffer, abi_version) == 0);
static_assert(sizeof(((itcore_buffer *)nullptr)->abi_version) == 4);
static_assert(offsetof(itcore_buffer, struct_size) == 4);
static_assert(sizeof(((itcore_buffer *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_buffer, data) == 8);
static_assert(sizeof(((itcore_buffer *)nullptr)->data) == 8);
static_assert(offsetof(itcore_buffer, length) == 16);
static_assert(sizeof(((itcore_buffer *)nullptr)->length) == 8);

// itcore_feed_result：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_feed_result) == 24);
static_assert(offsetof(itcore_feed_result, struct_size) == 0);
static_assert(sizeof(((itcore_feed_result *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_feed_result, accepted) == 8);
static_assert(sizeof(((itcore_feed_result *)nullptr)->accepted) == 8);
static_assert(offsetof(itcore_feed_result, status) == 16);
static_assert(sizeof(((itcore_feed_result *)nullptr)->status) == 4);

// itcore_options：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_options) == 24);
static_assert(offsetof(itcore_options, abi_version) == 0);
static_assert(sizeof(((itcore_options *)nullptr)->abi_version) == 4);
static_assert(offsetof(itcore_options, struct_size) == 4);
static_assert(sizeof(((itcore_options *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_options, columns) == 8);
static_assert(sizeof(((itcore_options *)nullptr)->columns) == 2);
static_assert(offsetof(itcore_options, rows) == 10);
static_assert(sizeof(((itcore_options *)nullptr)->rows) == 2);
static_assert(offsetof(itcore_options, scrollback_lines) == 12);
static_assert(sizeof(((itcore_options *)nullptr)->scrollback_lines) == 4);
static_assert(offsetof(itcore_options, memory_limit_bytes) == 16);
static_assert(sizeof(((itcore_options *)nullptr)->memory_limit_bytes) == 8);

// itcore_memory_usage：新增类型，ABI 版本仍为 0；字面量由本机编译器实测。
static_assert(sizeof(itcore_memory_usage) == 72);
static_assert(offsetof(itcore_memory_usage, struct_size) == 0);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_memory_usage, grid_bytes) == 8);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->grid_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, scrollback_bytes) == 16);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->scrollback_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, pool_bytes) == 24);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->pool_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, pool_epoch) == 32);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->pool_epoch) == 8);
static_assert(offsetof(itcore_memory_usage, queue_bytes) == 40);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->queue_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, search_bytes) == 48);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->search_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, total_bytes) == 56);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->total_bytes) == 8);
static_assert(offsetof(itcore_memory_usage, history_limit_bytes) == 64);
static_assert(sizeof(((itcore_memory_usage *)nullptr)->history_limit_bytes) == 8);

// itcore_cell：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_cell) == 24);
static_assert(offsetof(itcore_cell, grapheme_id) == 0);
static_assert(sizeof(((itcore_cell *)nullptr)->grapheme_id) == 4);
static_assert(offsetof(itcore_cell, width) == 4);
static_assert(sizeof(((itcore_cell *)nullptr)->width) == 1);
static_assert(offsetof(itcore_cell, reserved) == 5);
static_assert(sizeof(((itcore_cell *)nullptr)->reserved) == 1);
static_assert(offsetof(itcore_cell, flags) == 6);
static_assert(sizeof(((itcore_cell *)nullptr)->flags) == 2);
static_assert(offsetof(itcore_cell, fg) == 8);
static_assert(sizeof(((itcore_cell *)nullptr)->fg) == 4);
static_assert(offsetof(itcore_cell, bg) == 12);
static_assert(sizeof(((itcore_cell *)nullptr)->bg) == 4);
static_assert(offsetof(itcore_cell, underline_color) == 16);
static_assert(sizeof(((itcore_cell *)nullptr)->underline_color) == 4);
static_assert(offsetof(itcore_cell, hyperlink_id) == 20);
static_assert(sizeof(((itcore_cell *)nullptr)->hyperlink_id) == 4);

// itcore_key_event：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_key_event) == 24);
static_assert(offsetof(itcore_key_event, struct_size) == 0);
static_assert(sizeof(((itcore_key_event *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_key_event, vk) == 4);
static_assert(sizeof(((itcore_key_event *)nullptr)->vk) == 4);
static_assert(offsetof(itcore_key_event, scan) == 8);
static_assert(sizeof(((itcore_key_event *)nullptr)->scan) == 4);
static_assert(offsetof(itcore_key_event, modifiers) == 12);
static_assert(sizeof(((itcore_key_event *)nullptr)->modifiers) == 4);
static_assert(offsetof(itcore_key_event, is_repeat) == 16);
static_assert(sizeof(((itcore_key_event *)nullptr)->is_repeat) == 4);
static_assert(offsetof(itcore_key_event, unicode) == 20);
static_assert(sizeof(((itcore_key_event *)nullptr)->unicode) == 4);

// itcore_mouse_event：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_mouse_event) == 20);
static_assert(offsetof(itcore_mouse_event, struct_size) == 0);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_mouse_event, col) == 4);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->col) == 2);
static_assert(offsetof(itcore_mouse_event, row) == 6);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->row) == 2);
static_assert(offsetof(itcore_mouse_event, button) == 8);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->button) == 1);
static_assert(offsetof(itcore_mouse_event, action) == 9);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->action) == 1);
static_assert(offsetof(itcore_mouse_event, wheel_delta) == 12);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->wheel_delta) == 4);
static_assert(offsetof(itcore_mouse_event, modifiers) == 16);
static_assert(sizeof(((itcore_mouse_event *)nullptr)->modifiers) == 4);

// itcore_clipboard_policy：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_clipboard_policy) == 16);
static_assert(offsetof(itcore_clipboard_policy, struct_size) == 0);
static_assert(sizeof(((itcore_clipboard_policy *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_clipboard_policy, allow_write) == 4);
static_assert(sizeof(((itcore_clipboard_policy *)nullptr)->allow_write) == 4);
static_assert(offsetof(itcore_clipboard_policy, allow_read) == 8);
static_assert(sizeof(((itcore_clipboard_policy *)nullptr)->allow_read) == 4);
static_assert(offsetof(itcore_clipboard_policy, max_bytes) == 12);
static_assert(sizeof(((itcore_clipboard_policy *)nullptr)->max_bytes) == 4);

// itcore_colors：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_colors) == 1040);
static_assert(offsetof(itcore_colors, struct_size) == 0);
static_assert(sizeof(((itcore_colors *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_colors, foreground) == 4);
static_assert(sizeof(((itcore_colors *)nullptr)->foreground) == 4);
static_assert(offsetof(itcore_colors, background) == 8);
static_assert(sizeof(((itcore_colors *)nullptr)->background) == 4);
static_assert(offsetof(itcore_colors, cursor) == 12);
static_assert(sizeof(((itcore_colors *)nullptr)->cursor) == 4);
static_assert(offsetof(itcore_colors, palette) == 16);
static_assert(sizeof(((itcore_colors *)nullptr)->palette) == 1024);

// itcore_search_options：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_search_options) == 40);
static_assert(offsetof(itcore_search_options, struct_size) == 0);
static_assert(sizeof(((itcore_search_options *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_search_options, case_mode) == 4);
static_assert(sizeof(((itcore_search_options *)nullptr)->case_mode) == 1);
static_assert(offsetof(itcore_search_options, flags) == 8);
static_assert(sizeof(((itcore_search_options *)nullptr)->flags) == 4);
static_assert(offsetof(itcore_search_options, start_seg) == 16);
static_assert(sizeof(((itcore_search_options *)nullptr)->start_seg) == 8);
static_assert(offsetof(itcore_search_options, start_off) == 24);
static_assert(sizeof(((itcore_search_options *)nullptr)->start_off) == 4);
static_assert(offsetof(itcore_search_options, max_results) == 28);
static_assert(sizeof(((itcore_search_options *)nullptr)->max_results) == 4);
static_assert(offsetof(itcore_search_options, max_match_cells) == 32);
static_assert(sizeof(((itcore_search_options *)nullptr)->max_match_cells) == 4);

// itcore_search_match：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_search_match) == 64);
static_assert(offsetof(itcore_search_match, start_abs_row) == 0);
static_assert(sizeof(((itcore_search_match *)nullptr)->start_abs_row) == 8);
static_assert(offsetof(itcore_search_match, start_col) == 8);
static_assert(sizeof(((itcore_search_match *)nullptr)->start_col) == 2);
static_assert(offsetof(itcore_search_match, end_abs_row) == 16);
static_assert(sizeof(((itcore_search_match *)nullptr)->end_abs_row) == 8);
static_assert(offsetof(itcore_search_match, end_col) == 24);
static_assert(sizeof(((itcore_search_match *)nullptr)->end_col) == 2);
static_assert(offsetof(itcore_search_match, start_seg) == 32);
static_assert(sizeof(((itcore_search_match *)nullptr)->start_seg) == 8);
static_assert(offsetof(itcore_search_match, start_off) == 40);
static_assert(sizeof(((itcore_search_match *)nullptr)->start_off) == 4);
static_assert(offsetof(itcore_search_match, end_seg) == 48);
static_assert(sizeof(((itcore_search_match *)nullptr)->end_seg) == 8);
static_assert(offsetof(itcore_search_match, end_off) == 56);
static_assert(sizeof(((itcore_search_match *)nullptr)->end_off) == 4);

// itcore_search_progress：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_search_progress) == 48);
static_assert(offsetof(itcore_search_progress, struct_size) == 0);
static_assert(sizeof(((itcore_search_progress *)nullptr)->struct_size) == 4);
static_assert(offsetof(itcore_search_progress, status) == 4);
static_assert(sizeof(((itcore_search_progress *)nullptr)->status) == 4);
static_assert(offsetof(itcore_search_progress, progress) == 8);
static_assert(sizeof(((itcore_search_progress *)nullptr)->progress) == 4);
static_assert(offsetof(itcore_search_progress, total) == 16);
static_assert(sizeof(((itcore_search_progress *)nullptr)->total) == 8);
static_assert(offsetof(itcore_search_progress, current) == 24);
static_assert(sizeof(((itcore_search_progress *)nullptr)->current) == 8);
static_assert(offsetof(itcore_search_progress, flags) == 32);
static_assert(sizeof(((itcore_search_progress *)nullptr)->flags) == 4);
static_assert(offsetof(itcore_search_progress, dropped) == 36);
static_assert(sizeof(((itcore_search_progress *)nullptr)->dropped) == 4);
static_assert(offsetof(itcore_search_progress, changed) == 40);
static_assert(sizeof(((itcore_search_progress *)nullptr)->changed) == 4);

// itcore_search_range：结构体大小、各字段偏移及大小。
static_assert(sizeof(itcore_search_range) == 16);
static_assert(offsetof(itcore_search_range, row) == 0);
static_assert(sizeof(((itcore_search_range *)nullptr)->row) == 8);
static_assert(offsetof(itcore_search_range, start_col) == 8);
static_assert(sizeof(((itcore_search_range *)nullptr)->start_col) == 2);
static_assert(offsetof(itcore_search_range, end_col) == 10);
static_assert(sizeof(((itcore_search_range *)nullptr)->end_col) == 2);
static_assert(offsetof(itcore_search_range, is_current) == 12);
static_assert(sizeof(((itcore_search_range *)nullptr)->is_current) == 1);
static_assert(offsetof(itcore_search_range, reserved) == 13);
static_assert(sizeof(((itcore_search_range *)nullptr)->reserved) == 3);

// 状态 itcore_status
static_assert(ITCORE_OK == 0);
static_assert(ITCORE_NEED_DRAIN == 1);
static_assert(ITCORE_FATAL == 2);

// 字符格标志 ITCORE_CELL_*
static_assert(ITCORE_CELL_CONTINUATION == 1);
static_assert(ITCORE_CELL_BOLD == 2);
static_assert(ITCORE_CELL_ITALIC == 4);
static_assert(ITCORE_CELL_UNDERLINE == 8);
static_assert(ITCORE_CELL_INVERSE == 16);
static_assert(ITCORE_CELL_SELECTED == 32);
static_assert(ITCORE_CELL_DIM == 64);
static_assert(ITCORE_CELL_BLINK == 128);
static_assert(ITCORE_CELL_HIDDEN == 256);
static_assert(ITCORE_CELL_STRIKE == 512);
static_assert(ITCORE_CELL_OVERLINE == 8192);

// 下划线样式 ITCORE_UL_*
static_assert(ITCORE_UL_SINGLE == 1);
static_assert(ITCORE_UL_DOUBLE == 2);
static_assert(ITCORE_UL_WAVY == 3);
static_assert(ITCORE_UL_DOTTED == 4);
static_assert(ITCORE_UL_DASHED == 5);

// 换行类型 itcore_wrap
static_assert(ITCORE_WRAP_HARD == 0);
static_assert(ITCORE_WRAP_SOFT == 1);
static_assert(ITCORE_WRAP_WIDE == 2);

// 事件类型 itcore_event_kind
static_assert(ITCORE_EV_REPLY_BYTES == 1);
static_assert(ITCORE_EV_TITLE == 2);
static_assert(ITCORE_EV_BELL == 3);
static_assert(ITCORE_EV_RENDER_INVALIDATED == 4);
static_assert(ITCORE_EV_CLIPBOARD_REQUEST == 5);
static_assert(ITCORE_EV_ICON_TITLE == 6);
static_assert(ITCORE_EV_CLIPBOARD_WRITE == 7);
static_assert(ITCORE_EV_CLIPBOARD_READ == 8);
static_assert(ITCORE_EV_SEARCH_INVALIDATED == 9);

// 修饰键 ITCORE_MOD_*
static_assert(ITCORE_MOD_SHIFT == 1);
static_assert(ITCORE_MOD_CTRL == 2);
static_assert(ITCORE_MOD_ALT == 4);
static_assert(ITCORE_MOD_WIN == 8);

// 便携虚拟键 ITCORE_VK_*
static_assert(ITCORE_VK_BACK == 8);
static_assert(ITCORE_VK_TAB == 9);
static_assert(ITCORE_VK_RETURN == 13);
static_assert(ITCORE_VK_ESCAPE == 27);
static_assert(ITCORE_VK_SPACE == 32);
static_assert(ITCORE_VK_LEFT == 37);
static_assert(ITCORE_VK_UP == 38);
static_assert(ITCORE_VK_RIGHT == 39);
static_assert(ITCORE_VK_DOWN == 40);
static_assert(ITCORE_VK_INSERT == 45);
static_assert(ITCORE_VK_DELETE == 46);
static_assert(ITCORE_VK_HOME == 36);
static_assert(ITCORE_VK_END == 35);
static_assert(ITCORE_VK_PRIOR == 33);
static_assert(ITCORE_VK_NEXT == 34);
static_assert(ITCORE_VK_F1 == 112);
static_assert(ITCORE_VK_F12 == 123);
static_assert(ITCORE_VK_NUMPAD0 == 96);
static_assert(ITCORE_VK_NUMPAD9 == 105);
static_assert(ITCORE_VK_MULTIPLY == 106);
static_assert(ITCORE_VK_ADD == 107);
static_assert(ITCORE_VK_SEPARATOR == 108);
static_assert(ITCORE_VK_SUBTRACT == 109);
static_assert(ITCORE_VK_DECIMAL == 110);
static_assert(ITCORE_VK_DIVIDE == 111);

// 鼠标动作 ITCORE_MOUSE_*
static_assert(ITCORE_MOUSE_PRESS == 1);
static_assert(ITCORE_MOUSE_RELEASE == 2);
static_assert(ITCORE_MOUSE_MOVE == 3);
static_assert(ITCORE_MOUSE_WHEEL == 4);

// 鼠标按键 ITCORE_MBUTTON_*
static_assert(ITCORE_MBUTTON_LEFT == 0);
static_assert(ITCORE_MBUTTON_MIDDLE == 1);
static_assert(ITCORE_MBUTTON_RIGHT == 2);
static_assert(ITCORE_MBUTTON_NONE == 3);
static_assert(ITCORE_MBUTTON_WHEEL_UP == 64);
static_assert(ITCORE_MBUTTON_WHEEL_DOWN == 65);

// 鼠标模式 ITCORE_MOUSE_MODE_*
static_assert(ITCORE_MOUSE_MODE_NONE == 0);
static_assert(ITCORE_MOUSE_MODE_X10 == 1);
static_assert(ITCORE_MOUSE_MODE_NORMAL == 2);
static_assert(ITCORE_MOUSE_MODE_BUTTON == 4);
static_assert(ITCORE_MOUSE_MODE_ANY == 8);
static_assert(ITCORE_MOUSE_MODE_SGR == 16);

// 模式查询状态 itcore_mode_state
static_assert(ITCORE_MODE_NOT_RECOGNIZED == 0);
static_assert(ITCORE_MODE_SET == 1);
static_assert(ITCORE_MODE_RESET == 2);

// 搜索大小写策略 ITCORE_SEARCH_CASE_*
static_assert(ITCORE_SEARCH_CASE_SMART == 0);
static_assert(ITCORE_SEARCH_CASE_SENSITIVE == 1);
static_assert(ITCORE_SEARCH_CASE_INSENSITIVE == 2);

// 搜索选项标志 ITCORE_SEARCH_F_*
static_assert(ITCORE_SEARCH_F_REGEX == 1);
static_assert(ITCORE_SEARCH_F_BACKWARD == 2);
static_assert(ITCORE_SEARCH_F_WRAP == 4);

// 搜索状态与错误码 ITCORE_SEARCH_*
static_assert(ITCORE_SEARCH_DONE == 0);
static_assert(ITCORE_SEARCH_RUNNING == 1);
static_assert(ITCORE_SEARCH_PENDING == 2);
static_assert(ITCORE_SEARCH_NOT_FOUND == 3);
static_assert(ITCORE_SEARCH_RESTARTED == 4);
static_assert(ITCORE_SEARCH_EINVAL == -1);
static_assert(ITCORE_SEARCH_RESTART == -2);
static_assert(ITCORE_SEARCH_EREGEX == -3);
static_assert(ITCORE_SEARCH_EUNSUPPORTED == -4);

// 搜索进度标志 ITCORE_SEARCH_PF_*
static_assert(ITCORE_SEARCH_PF_WRAPPED == 1);
static_assert(ITCORE_SEARCH_PF_TRUNCATED == 2);
static_assert(ITCORE_SEARCH_PF_INCOMPLETE == 4);
static_assert(ITCORE_SEARCH_PF_NEW_CONTENT == 8);
#endif

int main() {
#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFu
    std::puts("ABI-02.layout：跳过，布局断言仅适用于 64 位目标。");
#endif
    return 0;
}
