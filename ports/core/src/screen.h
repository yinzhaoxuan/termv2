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
    PoolRef ref() const { return PoolRef{store_, store_->size(), epoch_}; }
    // OSC 8：超链接以 "\0H" id "\0" url 编码存于同一池（与字素不冲突：文本不含 NUL）。
    uint32_t intern_hyperlink(std::string_view url, std::string_view id);
    static bool decode_hyperlink(const char *s, size_t n, std::string_view *url, std::string_view *id);
private:
    std::shared_ptr<PoolStore> store_;
    std::unordered_map<std::string, uint32_t> to_id_;
    size_t bytes_ = 0;
    uint64_t epoch_ = 1;
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

struct Cursor { int x = 0, y = 0; };  // 0 基；x 可等于 cols（待换行）

struct SavedCursor {
    Cursor cur;
    uint32_t fg = 0, bg = 0;
    uint16_t flags = 0;
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
private:
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
    // 绝对显示行（从最旧开始）→ (逻辑行下标, 行内列偏移)；失败返回 false。
    bool map_display_row(uint16_t cols, size_t display_row, size_t *logic_i, size_t *cell_off) const;
    size_t display_rows(uint16_t cols) const;
private:
    void trim();
    std::deque<LogicalLine> lines_;
    uint32_t max_lines_;
    uint64_t max_bytes_;
    size_t bytes_ = 0;
};

struct ScreenModes {
    bool wrap = true;          // DECAWM
    bool insert = false;       // IRM
    bool cursor_visible = true;// DECTCEM
    bool alt = false;          // 当前是否备用屏
    bool bracketed_paste = false;
    bool app_cursor = false;   // DECCKM (?1)
    bool app_keypad = false;   // DECKPAM / DECKPNM
    uint32_t mouse_mode = 0;   // ITCORE_MOUSE_MODE_* 位掩码
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
    const Grid &grid() const { return modes_.alt ? alt_ : main_; }
    Scrollback &scrollback() { return scrollback_; }
    const Scrollback &scrollback() const { return scrollback_; }
    GraphemePool &pool() { return pool_; }
    const GraphemePool &pool() const { return pool_; }
    const ScreenModes &modes() const { return modes_; }
    Pen &pen() { return pen_; }
    uint64_t revision() const { return revision_; }
    // OSC 8：params 为 "k=v:k=v"，url 为空表示结束链接。
    void set_hyperlink(std::string_view params, std::string_view url);
    uint32_t hyperlink_id() const { return link_id_; }
    void reset_pool_for_test() { pool_.clear(); }
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
        std::vector<Row> rows_data;  // 长度 = requested row_count（裁剪后）
        PoolRef pool;                // 增量引用字素池（不拷贝），见 GraphemePool
    };
    SnapshotData take_snapshot(int32_t first_row, uint16_t row_count) const;
    size_t mem_bytes() const;
private:
    void write_char(uint32_t cp, const uint8_t *utf8, size_t utf8_len);
    void write_utf8_text(const std::string &text);
    void linefeed(bool hard);
    void carriage_return();
    void backspace();
    void tab();
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

    GraphemePool pool_;
    Grid main_, alt_;
    Scrollback scrollback_;
    ScreenModes modes_;
    Pen pen_;
    SavedCursor saved_main_, saved_alt_;
    uint64_t revision_ = 1;
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
    bool join_cluster(uint32_t cp, const uint8_t *utf8, size_t utf8_len);
};

}  // namespace itcore
