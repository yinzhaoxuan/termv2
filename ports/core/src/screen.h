// SPDX-License-Identifier: GPL-2.0-only
//
// iTerm2 Windows 派生核心：屏幕模型（T05）。
// 派生自 iTerm2（https://github.com/gnachman/iTerm2，固定提交
// f29420ae4d3e0f986ee24ea286885d32af58175a）的
// sources/VT100/VT100Grid.{h,m}、sources/ScreenChar/ScreenChar.h、
// sources/LineBuffer/LineBuffer.h、sources/VT100/VT100CSIParser.m
// （SetCSITypeAndDefaultParameters）与 sources/VT100/VT100Terminal.m（SGR/模式）。
// 原作者 George Nachman 及 iTerm2 贡献者；iTerm2 以 GNU GPL v2 发布。
// 修改：去除 Foundation，改为 C++20；字素池 + itcore_cell；逻辑行滚动缓冲与
// 宽度重排索引；历史双重限额（设计 14.2 / 15.4）。详细对照见 SOURCE_MAP.md。
#pragma once
#include "itcore.h"
#include "vt_parser.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace itcore {

// ---- 字素池：grapheme_id → UTF-8；id 0 为空格/空 ----
// 增量共享（设计 14.2 快照语义）：存储为分块、只追加的 PoolStore；已发布条目不可变。
// 写端（解析线程，持核心锁）追加后以 release 发布 size；快照只记录
// {shared_ptr<const PoolStore>, limit}，读端以 acquire 读取 id<limit 的条目，无需拷贝也无需加锁。
// clear()（RIS 等）换新 PoolStore 并递增 epoch，旧快照继续持有旧存储，id 不会被重解释。
class PoolStore {
public:
    static constexpr uint32_t kChunkBits = 12, kChunk = 1u << kChunkBits, kMaxChunks = 4096;
    PoolStore();
    ~PoolStore();
    PoolStore(const PoolStore &) = delete;
    PoolStore &operator=(const PoolStore &) = delete;
    uint32_t size() const { return size_.load(std::memory_order_acquire); }
    // 仅读端：id 须 < 调用方先前 acquire 得到的 size。
    const std::string *at(uint32_t id) const;
    // 仅写端：返回新 id；满（kChunk*kMaxChunks）时返回 0。
    uint32_t append(std::string &&s);
    static constexpr uint32_t capacity() { return kChunk * kMaxChunks; }
private:
    std::atomic<std::string *> chunks_[kMaxChunks];
    std::atomic<uint32_t> size_{0};
};

struct PoolRef {
    std::shared_ptr<const PoolStore> store;
    uint32_t limit = 0;
    uint64_t epoch = 0;
    const char *get(uint32_t id, size_t *len) const;
};

class GraphemePool {
public:
    GraphemePool();
    uint32_t intern(std::string_view utf8);
    const char *get(uint32_t id, size_t *len) const;
    size_t bytes() const { return bytes_; }
    uint32_t size() const { return store_->size(); }
    uint64_t epoch() const { return epoch_; }
    void clear();
    // 压缩：只保留 live 中的 id（0 恒保留），重建新 store；返回 旧id→新id（未列入者映射到 0）。
    // epoch 递增；旧 PoolRef 继续持有旧 store。bytes_ 重新按实际条目计算。
    std::unordered_map<uint32_t, uint32_t> compact(const std::vector<uint32_t> &live);
    // 池满时 intern 返回的预留 U+FFFD id（未满时为 0 表示尚未预留）。
    uint32_t replacement_id() const { return fffd_id_; }
    void set_capacity_for_test(uint32_t c) { cap_ = c; }
    static size_t entry_bytes(size_t n) { return n * 2 + 48; }
    PoolRef ref() const { return PoolRef{store_, store_->size(), epoch_}; }
    // OSC 8：超链接以 "\0H" id "\0" url 编码存于同一池（与字素不冲突：文本不含 NUL）。
    uint32_t intern_hyperlink(std::string_view url, std::string_view id);
    static bool decode_hyperlink(const char *s, size_t n, std::string_view *url, std::string_view *id);
private:
    std::shared_ptr<PoolStore> store_;
    std::unordered_map<std::string, uint32_t> to_id_;
    size_t bytes_ = 0;
    uint64_t epoch_ = 1;
    uint32_t fffd_id_ = 0;
    uint32_t cap_ = PoolStore::capacity();
};

// 终端列宽：CJK/全角=2，组合/零宽=0，其余=1（设计 14.2：不由 DirectWrite 像素反推）。
int cell_width(uint32_t cp);
// UTF-8 首码点与字节长度；非法则 cp=U+FFFD、n=1。
bool utf8_next(const uint8_t *p, size_t n, uint32_t *cp, size_t *adv);

struct Cell {
    uint32_t grapheme_id = 0;
    uint8_t width = 1;
    uint16_t flags = 0;
    uint32_t fg = 0, bg = 0, underline_color = 0;
    uint32_t hyperlink_id = 0;
    itcore_cell to_abi() const;
    static Cell blank();
};

struct Row {
    std::vector<Cell> cells;
    itcore_wrap wrap = ITCORE_WRAP_HARD;
    uint64_t id = 0;        // 行身份（淘汰后不复用）
    uint64_t revision = 0;  // 内容变更
    void resize_cols(uint16_t cols);
    size_t mem_bytes() const { return cells.size() * sizeof(Cell) + 32; }
};

// 选区端点：seg = 段身份（历史 LogicalLine.id / 网格 Row.id，淘汰进历史时保持不变），
// off = 段内字符序号（不计宽字符尾格）。滚动、历史重排、resize 都不改变 (seg, off)。
struct SelPos { uint64_t seg = 0; uint32_t off = 0; };

struct Cursor { int x = 0, y = 0; };  // 0 基；x 可等于 cols（待换行）

struct SavedCursor {
    Cursor cur;
    uint32_t fg = 0, bg = 0;
    uint16_t flags = 0;
    char g[2] = {'B', 'B'};  // G0/G1 字符集（DECSC 一并保存，同上游）
    int gl = 0;
    bool valid = false;
};

struct Pen {
    uint32_t fg = 0, bg = 0, underline_color = 0;  // 0xTTRRGGBB，TT=0 默认
    uint16_t flags = 0;
    Cell make_cell(uint32_t gid, uint8_t w) const;
};

class Grid {
public:
    void reset(uint16_t cols, uint16_t rows);
    void resize(uint16_t cols, uint16_t rows);  // 内容尽力保留（无滚动缓冲时）
    uint16_t cols() const { return cols_; }
    uint16_t rows() const { return rows_; }
    Row &row(int y) { return rows_data_[y]; }
    const Row &row(int y) const { return rows_data_[y]; }
    Cursor &cursor() { return cursor_; }
    const Cursor &cursor() const { return cursor_; }
    void set_scroll(int top, int bottom);  // 含两端，0 基
    int scroll_top() const { return scroll_top_; }
    int scroll_bottom() const { return scroll_bottom_; }
    bool wrap_pending() const { return wrap_pending_; }
    void set_wrap_pending(bool v) { wrap_pending_ = v; }
    bool origin_mode() const { return origin_; }
    void set_origin(bool v) { origin_ = v; }
    // 滚动：向上 n 行，顶部滚出的行交给回调；向下则底部出屏丢弃。
    using EvictFn = void (*)(Row &&, void *);
    void scroll_up(int n, EvictFn evict, void *ctx);
    void scroll_down(int n);
    void clear_all(const Cell &blank);
    void erase_in_display(int mode, const Cell &blank);  // 0/1/2/3（3=同 2，滚动历史由 Screen 处理）
    void erase_in_line(int mode, const Cell &blank);
    void insert_lines(int n, const Cell &blank);
    void delete_lines(int n, const Cell &blank);
    void insert_chars(int n, const Cell &blank);
    void delete_chars(int n, const Cell &blank);
    // 修复宽字符孤儿：孤立 CONTINUATION→blank；width 与尾格不一致→对齐/清空
    void repair_wide_orphans(int y);
    // 写格前：清掉将被覆盖的宽基字及其尾格；窄盖宽时清多余尾格
    void clear_for_write(int x, int y, int write_width, const Cell &blank);
    void clamp_cursor(bool allow_pending);
    int origin_top() const { return origin_ ? scroll_top_ : 0; }
    int origin_bottom() const { return origin_ ? scroll_bottom_ : rows_ - 1; }
    size_t mem_bytes() const;
    // 行 revision 全局单调：所有赋值取 ++counter（counter 由 Screen 持有，主/备用屏共享）。
    void set_rev_counter(uint64_t *c) { rev_ = c; }
    uint64_t next_rev() { return ++(rev_ ? *rev_ : local_rev_); }
    void touch_all_rows() { for (auto &r : rows_data_) r.revision = next_rev(); }
    template <class F> void for_each_row(F f) { for (auto &r : rows_data_) f(r); }
private:
    uint64_t *rev_ = nullptr;
    uint64_t local_rev_ = 0;
    uint16_t cols_ = 0, rows_ = 0;
    std::vector<Row> rows_data_;
    Cursor cursor_;
    int scroll_top_ = 0, scroll_bottom_ = 0;
    bool wrap_pending_ = false, origin_ = false;
    uint64_t next_row_id_ = 1;
};

// 逻辑行：硬换行结束的完整行；软换行跨多显示行。宽度重排时按单元格重排。
struct LogicalLine {
    std::vector<Cell> cells;
    bool hard = true;  // 末尾是否硬换行
    uint64_t id = 0;
    size_t mem_bytes() const { return cells.size() * sizeof(Cell) + 24; }
};

class Scrollback {
public:
    explicit Scrollback(uint32_t max_lines, uint64_t max_bytes)
        : max_lines_(max_lines ? max_lines : 10000),
          max_bytes_(max_bytes ? max_bytes : (64ull << 20)) {}
    void push(LogicalLine &&line);
    // 取出最旧行（重排/测试）
    bool empty() const { return lines_.empty(); }
    size_t logical_count() const { return lines_.size(); }
    size_t bytes() const { return bytes_; }
    const LogicalLine &at(size_t i) const { return lines_[i]; }
    // 以当前宽度重排：返回显示行列表（Row 形态），并填充 wrap。
    void reflow_into(uint16_t cols, std::vector<Row> &out) const;
    void clear();
    template <class F> void for_each_line(F f) { ++gen_; for (auto &l : lines_) f(l); }
    // 内容代数：push/淘汰/clear/可写遍历都 +1（history_display_rows 缓存键，B2）
    uint64_t generation() const { return gen_; }
    // 绝对显示行（从最旧开始）→ (逻辑行下标, 行内列偏移)；失败返回 false。
    bool map_display_row(uint16_t cols, size_t display_row, size_t *logic_i, size_t *cell_off) const;
    size_t display_rows(uint16_t cols) const;
private:
    void trim();
    std::deque<LogicalLine> lines_;
    uint32_t max_lines_;
    uint64_t max_bytes_;
    size_t bytes_ = 0;
    uint64_t gen_ = 0;
};

struct ScreenModes {
    bool wrap = true;          // DECAWM
    bool insert = false;       // IRM (ANSI 4)
    bool smooth_scroll = false;// DECSCLM (?4)：仅记录状态，渲染不做平滑滚动
    bool lnm = false;          // LNM (ANSI 20)：LF/VT/FF 同时回车
    bool cursor_visible = true;// DECTCEM
    bool alt = false;          // 当前是否备用屏
    bool bracketed_paste = false;
    bool app_cursor = false;   // DECCKM (?1)
    bool app_keypad = false;   // DECKPAM / DECKPNM
    uint32_t mouse_mode = 0;   // ITCORE_MOUSE_MODE_* 位掩码
    bool reverse_screen = false;  // DECSCNM (?5)
    bool focus_events = false;    // ?1004
    bool sync_output = false;     // ?2026（后端据此暂停取快照）
    int cursor_style = 0;         // DECSCUSR Ps（0..6）
};

// CSI 默认参数（上游 SetCSITypeAndDefaultParameters 子集）；就地填充 omitted→default。
// 返回“命令标签”字符串便于测试（如 "CUP","ED","SGR"）；未知返回 ""。
std::string apply_csi_defaults(Token &t);

class Screen {
public:
    Screen(uint16_t cols, uint16_t rows, uint32_t sb_lines, uint64_t mem_limit);
    void resize(uint16_t cols, uint16_t rows);
    // 应用解析器 Token；可能产生应答字节（写入 reply）。
    void apply(const Token &t, std::string *reply);
    Grid &grid() { return modes_.alt ? alt_ : main_; }
    bool on_alt() const { return modes_.alt; }
    const Grid &grid() const { return modes_.alt ? alt_ : main_; }
    Scrollback &scrollback() { return scrollback_; }
    const Scrollback &scrollback() const { return scrollback_; }
    GraphemePool &pool() { return pool_; }
    const GraphemePool &pool() const { return pool_; }
    const ScreenModes &modes() const { return modes_; }
    void invalidate_all() { touch_all(); }  // 调色板 / 默认色变化：整屏重画
    Pen &pen() { return pen_; }
    uint64_t revision() const { return revision_; }
    // ?2026 由开变关的累计次数（?2026l、DECSTR、RIS）：后端据此发现同一批里的“关→内容→开”
    uint64_t sync_end_count() const { return sync_ends_; }
    // OSC 8：params 为 "k=v:k=v"，url 为空表示结束链接。
    void set_hyperlink(std::string_view params, std::string_view url);
    uint32_t hyperlink_id() const { return link_id_; }
    // 换新池并推进所有行 revision（epoch 同时递增）。
    void reset_pool_for_test() { pool_.clear(); main_.touch_all_rows(); alt_.touch_all_rows(); }
    // 字素池压缩（RIS / ED 3 / 字节超预算 75% 时自动触发）。
    void compact_pool();
    void set_pool_budget(size_t b) { pool_budget_ = b; pool_threshold_ = b / 4 * 3; }
    size_t pool_budget() const { return pool_budget_; }
    uint64_t compactions() const { return compactions_; }
    void bump() { ++revision_; }
    // 组装视口+滚动历史的显示行快照数据。
    // first_row：相对视口顶部，0=屏幕首行，负=滚入历史。
    struct SnapshotData {
        uint64_t revision = 0;
        uint64_t generation = 0;     // 最近成功 resize_gen（由 core 填入）
        uint32_t mouse_mode = 0;
        uint16_t cols = 0, rows = 0;
        int cursor_col = 0, cursor_row = 0;
        bool cursor_visible = true;
        bool reverse_screen = false;
        int cursor_style = 0;
        std::vector<Row> rows_data;  // 长度 = requested row_count（裁剪后）
        uint64_t epoch = 0;          // 字素池 epoch（clear/压缩时递增）
        PoolRef pool;                // 增量引用字素池（不拷贝），见 GraphemePool
    };
    SnapshotData take_snapshot(int32_t first_row, uint16_t row_count) const;
    // DECRQM 语义：0 不支持 / 1 置位 / 2 复位（DEC 私有模式号）
    int dec_mode_state(int mode) const;
    int ansi_mode_state(int mode) const;  // ANSI 模式（CSI Ps $ p）
    // 历史在当前列宽下的显示行数（备用屏为 0）
    size_t history_display_rows() const;
    // ---- 选区（字符粒度；坐标为视口相对，first_row 同 take_snapshot）----
    // unit：0 字符 / 1 词 / 2 逻辑行（跨软换行）。词/行模式下 extend 以起点所在的词/行为锚扩展。
    bool sel_start(int32_t first_row, uint16_t col, uint16_t row, int unit = 0);
    bool sel_extend(int32_t first_row, uint16_t col, uint16_t row);
    void sel_clear();
    bool sel_active() const { return sel_.active; }
    std::string sel_text() const;
    // 主屏历史 + 主屏的逻辑纯文本（软换行拼接、行尾空格去掉、末尾空行去掉）；超过 max_bytes 保留尾部，
    // 截断点落在 UTF-8 字符边界并尽量落在行首
    std::string export_text(size_t max_bytes) const;
    uint64_t sel_serial() const { return sel_serial_; }

    size_t mem_bytes() const;
private:
    void write_char(uint32_t cp, const uint8_t *utf8, size_t utf8_len);
    void write_utf8_text(const std::string &text);
    void linefeed(bool hard);
    void carriage_return();
    void backspace();
    void tab();
    void back_tab();
    void reset_tabs();
    void soft_reset();   // DECSTR
    void decaln();       // DECALN
    void save_cursor();
    void restore_cursor();
    void touch_all() { main_.touch_all_rows(); alt_.touch_all_rows(); }
    void index();     // IND
    void reverse_index();  // RI
    void set_mode(const Token &t, bool set);
    void do_sgr(const Token &t);
    void do_cup(int row1, int col1);
    void enter_alt(bool clear);
    void leave_alt(bool clear);
    void evict_to_scrollback(Row &&r);
    static void evict_thunk(Row &&r, void *ctx);
    void flush_soft_line_to_scrollback_if_needed();
    void full_reset();
    void apply_one(const Token &t, std::string *reply);
    void maybe_compact();

    uint64_t row_rev_ = 0;           // 行 revision 计数器（须先于 main_/alt_ 声明）
    size_t pool_budget_ = 8u << 20, pool_threshold_ = 6u << 20;
    bool want_compact_ = false;
    uint64_t compactions_ = 0;

    GraphemePool pool_;
    Grid main_, alt_;
    Scrollback scrollback_;
    ScreenModes modes_;
    Pen pen_;
    SavedCursor saved_main_, saved_alt_;
    uint64_t revision_ = 1;
    uint64_t sync_ends_ = 0;
    uint64_t next_logic_id_ = 1;
    // 正在写入的当前逻辑行缓冲（视口内软换行累积）；硬换行时压入 scrollback。
    // 简化：滚动出顶部的行按 wrap 标记合并/压入。
    Cell last_printed_;  // REP 用
    bool has_last_ = false;
    // 字素簇状态（上游 enumerateComposedCharacters + StringToScreenChars）
    uint32_t last_base_cp_ = 0, last_cp_ = 0;
    int last_cluster_cps_ = 0;
    int last_x_ = -1, last_y_ = -1;
    uint32_t link_id_ = 0;
    std::vector<bool> tabs_;     // 制表位（HTS/TBC），列数变化时补默认每 8 列
    char charset_[2] = {'B', 'B'};  // G0/G1：'0' DEC 特殊图形，其余按 ASCII
    int gl_ = 0;                    // SO=1 / SI=0
    bool join_cluster(uint32_t cp, const uint8_t *utf8, size_t utf8_len);
    // 选区
    struct Selection { bool active = false, alt = false; int unit = 0; SelPos anchor, head; };
    struct Seg { uint64_t id; std::vector<const Cell *> chars; bool hard; };
    Selection sel_;
    uint64_t sel_serial_ = 0;
    std::vector<Seg> sel_segments() const { return segments(sel_.alt); }
    std::vector<Seg> segments(bool alt) const;
    bool sel_pos_at(int32_t first_row, uint16_t col, uint16_t row, SelPos *out) const;
    // 视口 [first_row, first_row+n) 每行每列的选区坐标
    std::vector<std::vector<SelPos>> view_positions(int32_t first_row, uint16_t n) const;
    using SegPos = std::pair<size_t, uint32_t>;  // (段序号, 字符序号)
    bool sel_range(const Selection &sel, const std::vector<Seg> &segs, SegPos *s, SegPos *e) const;
    bool sel_range(const std::vector<Seg> &segs, SegPos *s, SegPos *e) const { return sel_range(sel_, segs, s, e); }
    void unit_range(const std::vector<Seg> &segs, SegPos p, int unit, SegPos *lo, SegPos *hi) const;
    int char_class(const Cell *c) const;
    // 选区变化：新旧选区覆盖到的网格行全部取新 revision（全局计数器），历史行随计数器一起变
    void sel_touch(const Selection &old);
    size_t sel_hist_segments_ = 0;
    // B2：history_display_rows 缓存（键 = 回滚代数 + 列宽）；后端每帧查询，避免每次整段重排
    mutable uint64_t hist_rows_gen_ = ~0ull;
    mutable uint16_t hist_rows_cols_ = 0;
    mutable size_t hist_rows_cache_ = 0;
public:
    // 测试用：缓存未命中（真正重排）的次数
    mutable uint64_t hist_rows_recompute_count = 0;
private:
};

}  // namespace itcore
