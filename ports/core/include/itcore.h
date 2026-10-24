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
       ITCORE_CELL_SELECTED = 1u << 5,
       /* SGR 2/5(6)/8/9；22 同时清 BOLD 与 DIM，25/28/29 各自清除。渲染负责混色、闪烁相位、隐藏与删除线 */
       ITCORE_CELL_DIM = 1u << 6, ITCORE_CELL_BLINK = 1u << 7,
       ITCORE_CELL_HIDDEN = 1u << 8, ITCORE_CELL_STRIKE = 1u << 9 };
#define ITCORE_HAS_SGR_EXT_FLAGS 1
typedef struct { uint32_t grapheme_id; uint8_t width; uint8_t reserved; uint16_t flags;
                 uint32_t fg, bg, underline_color; uint32_t hyperlink_id; } itcore_cell;
typedef enum { ITCORE_WRAP_HARD = 0, ITCORE_WRAP_SOFT = 1, ITCORE_WRAP_WIDE = 2 } itcore_wrap;

typedef enum { ITCORE_EV_REPLY_BYTES = 1, ITCORE_EV_TITLE = 2, ITCORE_EV_BELL = 3,
               ITCORE_EV_RENDER_INVALIDATED = 4, ITCORE_EV_CLIPBOARD_REQUEST = 5,
               /* OSC 1（OSC 0 同时发 TITLE 与 ICON_TITLE）；CSI 23 t 出栈时也会重发 */
               ITCORE_EV_ICON_TITLE = 6,
               /* B4 OSC 52（须先经 itcore_set_clipboard_policy 打开）：WRITE 负载为解码并清洗后的 UTF-8（非法序列→U+FFFD，
                  去掉 NUL/C0（保留 \t\n\r）/DEL/C1；目标一律视作系统剪贴板）；READ 负载为规范化的选择参数 Pc
                  （只含 cpqs0-7，空请求为 "s0"），宿主读取剪贴板后用 itcore_encode_clipboard_reply 生成应答。
                  非法 Pc 一律 REQUEST（负载为空）；Pd 为空时不产生任何事件（不清空剪贴板） */
               ITCORE_EV_CLIPBOARD_WRITE = 7, ITCORE_EV_CLIPBOARD_READ = 8 } itcore_event_kind;

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
/* DECSCUSR Ps（0..6）：0/1 闪烁块，2 块，3 闪烁下划线，4 下划线，5 闪烁竖线，6 竖线；RIS 后为 0 */
void itcore_snapshot_cursor_style(const itcore_snapshot *s, int *decscusr);
/* DECSCNM（?5）：1 表示默认前景/背景对调（切换时核心推进所有行 revision，整屏重画） */
int  itcore_snapshot_reverse_screen(const itcore_snapshot *s);
void itcore_snapshot_release(itcore_snapshot *s);
/* OSC 8：itcore_cell.hyperlink_id → URL 与 id 参数（字节串，非 NUL 结尾，生命周期同快照）。
   hyperlink_id 为 0 或无效返回 0，成功返回 1。快照增量引用字素池：任何线程可读，
   与核心后续写入并发安全；释放前指针稳定。 */
int itcore_snapshot_hyperlink(const itcore_snapshot *s, uint32_t hyperlink_id,
                              const char **url, size_t *url_len, const char **id, size_t *id_len);
/* 模式查询，返回值与 DECRQM/DECRPM 的 Pm 对齐。DEC 私有模式（CSI ? Ps）与 ANSI 模式（CSI Ps）
   是两个独立编号空间，分别用两个函数查询；同一编号互不影响（如 DEC ?4 DECSCLM 与 ANSI 4 IRM）。
   为保持 C ABI 稳定，itcore_mode_query 的签名与语义（仅 DEC）不变，ANSI 另立新函数。 */
typedef enum { ITCORE_MODE_NOT_RECOGNIZED = 0, ITCORE_MODE_SET = 1, ITCORE_MODE_RESET = 2 } itcore_mode_state;
/* DEC 私有模式：至少覆盖 1 4 5 7 9 25 47 1000 1002 1003 1006 1047 1049 1004 2004 2026；core 为空返回 NOT_RECOGNIZED */
itcore_mode_state itcore_mode_query(const itcore_core *c, uint32_t dec_private_mode);
/* ?2026 同步输出由开变关的累计次数（?2026l、DECSTR、RIS 各计一次；只增不减）。
   后端在一批 feed 之后比较：计数变化说明本批内有同步结束（即使批尾又重新打开），应出一帧。 */
uint64_t itcore_sync_end_count(const itcore_core *c);
/* 按核心 itcore_feed 在 c 的当前屏幕上的字素簇与列宽规则切分 UTF-8 文本（IME 预编辑宽度用）：
   组合/ZWJ+Pictographic/区域指示符对并入前簇，Default_Ignorable 不占格，零宽基字按 1；
   肤色修饰把窄 emoji 扩为 2；VS16 只在主屏扩宽（备用屏不扩，同 screen.cpp 写网格）。
   屏幕状态从 c 读取，调用方不传；c 为 NULL 返回 0。与 itcore_feed 同线程调用。
   widths[i]（1/2）与 byte_lens[i]（该簇 UTF-8 字节数，含并入的可忽略码点）各可为 NULL；
   返回簇总数（可大于 cap，只写前 cap 个）。与写网格的差异：
   - 与写网格一样单簇最多 32 个码点（kMaxClusterCps），超出的码点丢弃、不另起簇，但其字节计入该簇 byte_lens；
   - 不模拟写网格时“tx+1<cols 且光标紧随其后才扩宽”的限制（行尾/光标位置导致网格不扩宽时，这里仍报 2）；
   - 只含可忽略码点的输入返回 0 个簇（其字节不计入任何簇）；
   - 输入不应含 C0 控制字符（网格里是控制功能，这里按宽 1 的普通簇计）。 */
size_t itcore_text_clusters(const itcore_core *c, const char *utf8, size_t length, uint8_t *widths, uint32_t *byte_lens, size_t cap);
/* ANSI 模式：覆盖 4（IRM）、20（LNM），其余 NOT_RECOGNIZED；core 为空返回 NOT_RECOGNIZED */
itcore_mode_state itcore_ansi_mode_query(const itcore_core *c, uint32_t ansi_mode);

/* 本地选区（字符粒度），坐标同 itcore_acquire_snapshot：first_row 相对视口顶部，col/row 为快照内格坐标。
   端点按 (逻辑段, 字符序号) 存储，跟随滚动历史、重排与 resize；切换主/备用屏、RIS、ED 3 时清除。
   start/extend 返回 1 表示选区（或其显示）有变化，0 表示无变化或坐标无效。 */
int  itcore_selection_start(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row);
/* 按单位开始选区：unit 0=字符 1=词（双击） 2=逻辑行（三击，跨软换行）。后续 extend 按同一单位吸附 */
int  itcore_selection_start_unit(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row, int unit);
/* 当前列宽下历史的显示行数（备用屏为 0）；视口 first_row 的下限为 -itcore_history_rows */
uint32_t itcore_history_rows(const itcore_core *c);
int  itcore_selection_extend(itcore_core *c, int32_t first_row, uint16_t col, uint16_t row);
void itcore_selection_clear(itcore_core *c);
int  itcore_selection_active(const itcore_core *c);
/* 复制文本（UTF-8）：软换行拼接、硬换行 '\n'、行尾空白去除、宽字符只出一次。无选区返回 1 且 *out=null */
int  itcore_selection_text(itcore_core *c, itcore_buffer **out);
/* 主屏历史 + 主屏的逻辑纯文本（UTF-8，行以 \n 分隔，不含 VT 状态/链接）。超过 max_bytes 时保留最新的尾部，
   截断点不切断 UTF-8 字符（并尽量从行首开始），因此结果 <= max_bytes。供重连保留旧 attempt 只读文本。成功 0 */
/* CSI 14 t / 16 t 用的单元格像素尺寸（后端从 GridMetrics 传入，须在首次 feed 前调用；0 表示未知，此时按 8x16 估算应答）。
   14 t 报文本区 rows*h;cols*w，16 t 报单元格 h;w，18 t 报行列（总是应答）。 */
void itcore_set_cell_pixel_size(itcore_core *c, uint16_t width_px, uint16_t height_px);
/* 双击选词：除字母数字与非 ASCII（含 CJK）外，还算作词内字符的 ASCII 标点（B3，对应 iTerm2 "Characters considered part of a word"）。
   chars 为 NULL 恢复默认 "/-+\~_."；len 为 0 表示没有额外词字符。只接受可打印 ASCII 标点（0x21–0x7E 的非字母数字），
   含空白、控制字符或非 ASCII 返回 -1 且不改动；成功 0。之后的选词立即生效。
   有意比上游 iTerm2 窄：上游允许任意字符，这里只收 ASCII 标点（字母数字与非 ASCII 本来就是词字符，空白/控制字符不该是）。
   线程：与 itcore_feed / 选区调用同一线程（UI 线程）调用，核心不加锁。 */
int  itcore_set_word_chars(itcore_core *c, const char *chars, size_t len);
/* B4 OSC 52 剪贴板策略。默认全关：任何 OSC 52 都只产生 CLIPBOARD_REQUEST（已拒绝）事件。
   allow_write：OSC 52;sel;<base64> 解码后不超过 max_bytes 时产生 CLIPBOARD_WRITE；超限/非法 base64 仍为 REQUEST（拒绝）。
   allow_read：OSC 52;sel;? 产生 CLIPBOARD_READ；不打开则拒绝。读与写分开授权（读会把剪贴板泄露给远端程序）。
   max_bytes 为 0 用默认 ITCORE_CLIPBOARD_DEFAULT_MAX，大于 768 KiB 钳到 768 KiB（OSC 负载上限 1 MiB 的 3/4）；
   base64 中的空白（空格/\t/\r/\n）忽略。cfg 为 NULL 恢复默认（全关）。线程同 itcore_feed（UI 线程）。成功 0 */
#define ITCORE_CLIPBOARD_DEFAULT_MAX (100u * 1024u)
typedef struct { uint32_t struct_size; int allow_write; int allow_read; uint32_t max_bytes; } itcore_clipboard_policy;
int  itcore_set_clipboard_policy(itcore_core *c, const itcore_clipboard_policy *cfg);
/* 生成 OSC 52 读应答 ESC]52;<Pc>;<base64><结尾>，Pc 回显 req 的选择参数（req 为 NULL 用 c）。req 为触发它的 CLIPBOARD_READ 事件（须在 itcore_event_release 前调用），
   结尾与请求一致：BEL 请求回 BEL，ST 请求回 ESC\（同 OSC 4/10/11/12）；req 为 NULL 用 ST，非 READ 事件返回 -1。
   未允许读返回 1，utf8 超过 max_bytes 返回 -2（*out=null），成功 0 */
int  itcore_encode_clipboard_reply(itcore_core *c, const itcore_event *req, const char *utf8, size_t len, itcore_buffer **out);
/* 颜色配置：值为 0x00RRGGBB。OSC 10/11/12 与 OSC 4 可在运行时覆盖、OSC 110/111/112 与 104 恢复为这里的配置值，
   查询（"?"）按当前值以 rgb:rrrr/gggg/bbbb 应答，结尾符与请求一致（BEL 或 ST）。修改会推进全部行 revision。 */
typedef struct {
    uint32_t struct_size;
    uint32_t foreground, background, cursor;
    uint32_t palette[256];
} itcore_colors;
/* 默认配置：xterm 16 色 + 6x6x6 + 24 级灰，前景 0xC7C7C7、背景 0x000000、光标 0xC7C7C7 */
void itcore_default_colors(itcore_colors *out);
/* 线程：itcore_core 不加锁，所有调用（feed/set_colors/set_cell_pixel_size/resize/encode/selection）须在同一线程；
   Win32 后端里传输线程只投递字节，喂核心与改配置都在 UI 线程。快照可跨线程读。 */
int  itcore_set_colors(itcore_core *c, const itcore_colors *cfg);
/* 快照时刻的生效颜色（配置 + OSC 覆盖）；渲染用它解析 TT=0 默认色与 TT=1 调色板索引 */
void itcore_snapshot_colors(const itcore_snapshot *s, itcore_colors *out);
int  itcore_export_text(const itcore_core *c, size_t max_bytes, itcore_buffer **out);
void itcore_destroy(itcore_core *c);
#ifdef __cplusplus
}
#endif
#endif
