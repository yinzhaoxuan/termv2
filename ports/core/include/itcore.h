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
       ITCORE_CELL_UNDERLINE = 1u << 3, ITCORE_CELL_INVERSE = 1u << 4,
       /* 快照里选区内的格：同时已翻转 INVERSE，渲染器无需额外处理 */
       ITCORE_CELL_SELECTED = 1u << 5 };
typedef struct { uint32_t grapheme_id; uint8_t width; uint8_t reserved; uint16_t flags;
                 uint32_t fg, bg, underline_color; uint32_t hyperlink_id; } itcore_cell;
typedef enum { ITCORE_WRAP_HARD = 0, ITCORE_WRAP_SOFT = 1, ITCORE_WRAP_WIDE = 2 } itcore_wrap;

typedef enum { ITCORE_EV_REPLY_BYTES = 1, ITCORE_EV_TITLE = 2, ITCORE_EV_BELL = 3,
               ITCORE_EV_RENDER_INVALIDATED = 4, ITCORE_EV_CLIPBOARD_REQUEST = 5 } itcore_event_kind;

int  itcore_create(const itcore_options *opt, itcore_core **out);
itcore_feed_result itcore_feed(itcore_core *c, const uint8_t *bytes, size_t length);
int  itcore_resize(itcore_core *c, uint16_t columns, uint16_t rows, uint64_t generation);
int  itcore_finish_input(itcore_core *c);
/* 键盘：vk 为 Windows VK_* 或便携枚举（见 ITCORE_VK_*）；unicode 非 0 时优先作为已提交字符 */
enum {
    ITCORE_MOD_SHIFT = 1u << 0, ITCORE_MOD_CTRL = 1u << 1,
    ITCORE_MOD_ALT = 1u << 2, ITCORE_MOD_WIN = 1u << 3
};
/* 便携虚拟键（与 Win32 VK_ 数值对齐的常用项） */
enum {
    ITCORE_VK_BACK = 0x08, ITCORE_VK_TAB = 0x09, ITCORE_VK_RETURN = 0x0D,
    ITCORE_VK_ESCAPE = 0x1B, ITCORE_VK_SPACE = 0x20,
    ITCORE_VK_LEFT = 0x25, ITCORE_VK_UP = 0x26, ITCORE_VK_RIGHT = 0x27, ITCORE_VK_DOWN = 0x28,
    ITCORE_VK_INSERT = 0x2D, ITCORE_VK_DELETE = 0x2E,
    ITCORE_VK_HOME = 0x24, ITCORE_VK_END = 0x23, ITCORE_VK_PRIOR = 0x21, ITCORE_VK_NEXT = 0x22,
    ITCORE_VK_F1 = 0x70, ITCORE_VK_F12 = 0x7B,
    ITCORE_VK_NUMPAD0 = 0x60, ITCORE_VK_NUMPAD9 = 0x69,
    ITCORE_VK_MULTIPLY = 0x6A, ITCORE_VK_ADD = 0x6B, ITCORE_VK_SEPARATOR = 0x6C,
    ITCORE_VK_SUBTRACT = 0x6D, ITCORE_VK_DECIMAL = 0x6E, ITCORE_VK_DIVIDE = 0x6F
};
typedef struct {
    uint32_t struct_size;
    uint32_t vk;          /* ITCORE_VK_* / VK_* */
    uint32_t scan;        /* 可选扫描码，0 忽略 */
    uint32_t modifiers;   /* ITCORE_MOD_* */
    int is_repeat;
    uint32_t unicode;     /* 已提交码点，0 表示纯功能键 */
} itcore_key_event;

enum {
    ITCORE_MOUSE_PRESS = 1, ITCORE_MOUSE_RELEASE = 2,
    ITCORE_MOUSE_MOVE = 3, ITCORE_MOUSE_WHEEL = 4
};
enum {
    ITCORE_MBUTTON_LEFT = 0, ITCORE_MBUTTON_MIDDLE = 1, ITCORE_MBUTTON_RIGHT = 2,
    ITCORE_MBUTTON_NONE = 3, ITCORE_MBUTTON_WHEEL_UP = 64, ITCORE_MBUTTON_WHEEL_DOWN = 65
};
typedef struct {
    uint32_t struct_size;
    uint16_t col, row;    /* 0 基格坐标 */
    uint8_t button;       /* ITCORE_MBUTTON_* */
    uint8_t action;       /* ITCORE_MOUSE_* */
    int32_t wheel_delta;  /* >0 上滚 */
    uint32_t modifiers;
} itcore_mouse_event;

/* 鼠标报告模式位（与 DECSET 1000/1002/1003/1006 对应） */
enum {
    ITCORE_MOUSE_MODE_NONE = 0,
    ITCORE_MOUSE_MODE_X10 = 1,       /* 9 */
    ITCORE_MOUSE_MODE_NORMAL = 2,    /* 1000 */
    ITCORE_MOUSE_MODE_BUTTON = 4,    /* 1002 */
    ITCORE_MOUSE_MODE_ANY = 8,       /* 1003 */
    ITCORE_MOUSE_MODE_SGR = 16       /* 1006 */
};

int  itcore_encode_text(itcore_core *c, const char *utf8, size_t len, itcore_buffer **out);
/* 粘贴：整事务一次返回；DECSET 2004 开启时由核心加 ESC[200~/ESC[201~。正文去掉 NUL 及内嵌的
   括号粘贴起止标记（7 位与 C1 形式，反复删除），防止伪造结束标记注入。>32KiB 返回 -2，*out=null */
int  itcore_encode_paste(itcore_core *c, const char *utf8, size_t len, itcore_buffer **out);
int  itcore_encode_key(itcore_core *c, const itcore_key_event *ev, itcore_buffer **out);
/* 未启用鼠标模式时返回 1 且 *out=null（留给本地选择）；成功 0 */
int  itcore_encode_mouse(itcore_core *c, const itcore_mouse_event *ev, itcore_buffer **out);
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
/* 字素池 epoch：RIS/ED 3/压缩换新存储时递增；变化时 grapheme/hyperlink id 需整屏重解析 */
uint64_t itcore_snapshot_epoch(const itcore_snapshot *s);
uint16_t itcore_snapshot_columns(const itcore_snapshot *s);
uint16_t itcore_snapshot_rows(const itcore_snapshot *s);
const itcore_cell *itcore_snapshot_row(const itcore_snapshot *s, uint16_t row, itcore_wrap *wrap, uint64_t *row_revision);
/* 字素簇 ID 取回 UTF-8；指针在快照释放前有效 */
const char *itcore_snapshot_grapheme(const itcore_snapshot *s, uint32_t grapheme_id, size_t *len);
void itcore_snapshot_cursor(const itcore_snapshot *s, uint16_t *col, uint16_t *row, int *visible);
/* 最近一次成功 itcore_resize 的 generation（供渲染对齐） */
uint64_t itcore_snapshot_generation(const itcore_snapshot *s);
/* 当前鼠标模式位掩码（ITCORE_MOUSE_MODE_*）；0 表示本地选择 */
uint32_t itcore_snapshot_mouse_mode(const itcore_snapshot *s);
void itcore_snapshot_release(itcore_snapshot *s);
/* OSC 8：itcore_cell.hyperlink_id → URL 与 id 参数（字节串，非 NUL 结尾，生命周期同快照）。
   hyperlink_id 为 0 或无效返回 0，成功返回 1。快照增量引用字素池：任何线程可读，
   与核心后续写入并发安全；释放前指针稳定。 */
int itcore_snapshot_hyperlink(const itcore_snapshot *s, uint32_t hyperlink_id,
                              const char **url, size_t *url_len, const char **id, size_t *id_len);
/* DEC 私有模式查询，返回值与 DECRQM/DECRPM 的 Pm 对齐 */
typedef enum { ITCORE_MODE_NOT_RECOGNIZED = 0, ITCORE_MODE_SET = 1, ITCORE_MODE_RESET = 2 } itcore_mode_state;
/* 至少覆盖 1 7 9 25 47 1000 1002 1003 1006 1047 1049 2004；core 为空返回 NOT_RECOGNIZED */
itcore_mode_state itcore_mode_query(const itcore_core *c, uint32_t dec_private_mode);

/* 本地选区（字符粒度），坐标同 itcore_acquire_snapshot：first_row 相对视口顶部，col/row 为快照内格坐标。
   端点按 (逻辑段, 字符序号) 存储，跟随滚动历史、重排与 resize；切换主/备用屏、RIS、ED 3 时清除。
   start/extend 返回 1 表示选区（或其显示）有变化，0 表示无变化或坐标无效。 */
int  itcore_selection_start(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row);
int  itcore_selection_extend(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row);
void itcore_selection_clear(itcore_core *c);
int  itcore_selection_active(const itcore_core *c);
/* 复制文本（UTF-8）：软换行拼接、硬换行 '\n'、行尾空白去除、宽字符只出一次。无选区返回 1 且 *out=null */
int  itcore_selection_text(itcore_core *c, itcore_buffer **out);
void itcore_destroy(itcore_core *c);
#ifdef __cplusplus
}
#endif
#endif
