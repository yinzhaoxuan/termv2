/* iTerm2 Windows 派生核心 C ABI 草案 v0（未冻结，T03A/T04 后冻结） */
#ifndef ITCORE_H
#define ITCORE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ITCORE_ABI_VERSION 0u
typedef struct itcore_core itcore_core;
typedef struct itcore_snapshot itcore_snapshot;
typedef struct itcore_event itcore_event;
typedef struct { uint32_t abi_version, struct_size; uint8_t *data; size_t length; } itcore_buffer;

typedef enum { ITCORE_OK = 0, ITCORE_NEED_DRAIN = 1, ITCORE_FATAL = 2 } itcore_status;
typedef struct { uint32_t struct_size; size_t accepted; itcore_status status; } itcore_feed_result;

typedef struct { uint32_t abi_version, struct_size; uint16_t columns, rows;
                 uint32_t scrollback_lines; uint64_t memory_limit_bytes; } itcore_options;

/* 字符格：颜色统一为 0xTTRRGGBB，TT=0 默认/1 调色板索引(RR 为下标)/2 真彩色 */
enum { ITCORE_CELL_CONTINUATION = 1u << 0, ITCORE_CELL_BOLD = 1u << 1, ITCORE_CELL_ITALIC = 1u << 2,
       ITCORE_CELL_UNDERLINE = 1u << 3, ITCORE_CELL_INVERSE = 1u << 4 };
typedef struct { uint32_t grapheme_id; uint8_t width; uint8_t reserved; uint16_t flags;
                 uint32_t fg, bg, underline_color; uint32_t hyperlink_id; } itcore_cell;
typedef enum { ITCORE_WRAP_HARD = 0, ITCORE_WRAP_SOFT = 1, ITCORE_WRAP_WIDE = 2 } itcore_wrap;

typedef enum { ITCORE_EV_REPLY_BYTES = 1, ITCORE_EV_TITLE = 2, ITCORE_EV_BELL = 3,
               ITCORE_EV_RENDER_INVALIDATED = 4, ITCORE_EV_CLIPBOARD_REQUEST = 5 } itcore_event_kind;

int  itcore_create(const itcore_options *opt, itcore_core **out);
itcore_feed_result itcore_feed(itcore_core *c, const uint8_t *bytes, size_t length);
int  itcore_resize(itcore_core *c, uint16_t columns, uint16_t rows, uint64_t generation);
int  itcore_finish_input(itcore_core *c);
int  itcore_encode_text(itcore_core *c, const char *utf8, size_t len, itcore_buffer **out);
int  itcore_encode_paste(itcore_core *c, const char *utf8, size_t len, itcore_buffer **out);
void itcore_buffer_release(itcore_buffer *b);
itcore_event *itcore_next_event(itcore_core *c);
itcore_event_kind itcore_event_type(const itcore_event *e);
/* 事件负载：REPLY_BYTES 为待写回字节，TITLE 为清洗后的 UTF-8，CLIPBOARD_REQUEST 为
   已被拒绝的 OSC 52 选择参数；指针在 itcore_event_release 前有效。 */
const uint8_t *itcore_event_data(const itcore_event *e, size_t *len);
void itcore_event_release(itcore_event *e);
/* finish_input 丢弃的残缺控制序列计数（含 EOF 时残缺 UTF-8，后者另产生一个 U+FFFD） */
uint64_t itcore_incomplete_count(const itcore_core *c);

itcore_snapshot *itcore_acquire_snapshot(itcore_core *c, int32_t first_row, uint16_t row_count);
uint64_t itcore_snapshot_revision(const itcore_snapshot *s);
uint16_t itcore_snapshot_columns(const itcore_snapshot *s);
uint16_t itcore_snapshot_rows(const itcore_snapshot *s);
const itcore_cell *itcore_snapshot_row(const itcore_snapshot *s, uint16_t row, itcore_wrap *wrap, uint64_t *row_revision);
/* 字素簇 ID 取回 UTF-8；指针在快照释放前有效 */
const char *itcore_snapshot_grapheme(const itcore_snapshot *s, uint32_t grapheme_id, size_t *len);
void itcore_snapshot_cursor(const itcore_snapshot *s, uint16_t *col, uint16_t *row, int *visible);
void itcore_snapshot_release(itcore_snapshot *s);
void itcore_destroy(itcore_core *c);
#ifdef __cplusplus
}
#endif
#endif
