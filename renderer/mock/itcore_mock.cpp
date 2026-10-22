#include "itcore_mock.h"
#include <string>
#include <vector>

struct itcore_snapshot {
  uint16_t cols, rows;
  uint64_t revision = 1;
  uint64_t generation = 0;
  uint32_t mouse_mode = 0;
  std::vector<std::vector<itcore_cell>> grid;
  std::vector<itcore_wrap> wrap;
  std::vector<uint64_t> row_rev;
  std::vector<std::string> graphemes;  // id 0 = 空格
  uint16_t cur_col = 0, cur_row = 0;
};

static uint32_t decode(const unsigned char *&p, const unsigned char *e) {
  uint32_t c = *p++;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (n) c &= (0x3F >> n);
  while (n-- && p < e && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
  return c;
}
// 简化宽度表，仅供模拟；正式规则由核心按 Unicode 版本与模糊宽度策略决定（14.2）
static uint8_t width_of(uint32_t c) {
  if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
      (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
      (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD))
    return 2;
  return 1;
}

extern "C" itcore_snapshot *itmock_snapshot_from_utf8(const char *utf8, size_t len, uint16_t cols, uint16_t rows) {
  auto *s = new itcore_snapshot{cols, rows};
  itcore_cell blank{0, 1, 0, 0, 0, 0, 0, 0};
  s->grid.assign(rows, std::vector<itcore_cell>(cols, blank));
  s->wrap.assign(rows, ITCORE_WRAP_HARD);
  s->row_rev.assign(rows, 1);
  s->graphemes.push_back(" ");
  auto *p = reinterpret_cast<const unsigned char *>(utf8), *e = p + len;
  uint16_t r = 0, c = 0;
  while (p < e && r < rows) {
    auto *start = p;
    uint32_t cp = decode(p, e);
    if (cp == '\n') { r++; c = 0; continue; }
    uint8_t w = width_of(cp);
    if (c + w > cols) {  // 宽字符放不下时留空并标记 wide-wrap
      s->wrap[r] = w == 2 && c < cols ? ITCORE_WRAP_WIDE : ITCORE_WRAP_SOFT;
      if (++r >= rows) break;
      c = 0;
    }
    s->graphemes.emplace_back(reinterpret_cast<const char *>(start), p - start);
    uint32_t id = uint32_t(s->graphemes.size() - 1);
    s->grid[r][c] = {id, w, 0, 0, 0, 0, 0, 0};
    if (w == 2) s->grid[r][c + 1] = {id, 0, 0, ITCORE_CELL_CONTINUATION, 0, 0, 0, 0};
    c += w;
  }
  s->cur_row = r < rows ? r : rows - 1;
  s->cur_col = c < cols ? c : cols - 1;
  return s;
}
extern "C" void itmock_set_style(itcore_snapshot *s, uint16_t row, uint16_t col, uint16_t flags, uint32_t fg, uint32_t bg) {
  auto &cell = s->grid[row][col];
  cell.flags = uint16_t((cell.flags & ITCORE_CELL_CONTINUATION) | flags);
  cell.fg = fg; cell.bg = bg;
  s->row_rev[row]++; s->revision++;
}
extern "C" uint64_t itcore_snapshot_revision(const itcore_snapshot *s) { return s->revision; }
extern "C" uint16_t itcore_snapshot_columns(const itcore_snapshot *s) { return s->cols; }
extern "C" uint16_t itcore_snapshot_rows(const itcore_snapshot *s) { return s->rows; }
extern "C" const itcore_cell *itcore_snapshot_row(const itcore_snapshot *s, uint16_t row, itcore_wrap *wrap, uint64_t *rev) {
  if (row >= s->rows) return nullptr;
  if (wrap) *wrap = s->wrap[row];
  if (rev) *rev = s->row_rev[row];
  return s->grid[row].data();
}
extern "C" const char *itcore_snapshot_grapheme(const itcore_snapshot *s, uint32_t id, size_t *len) {
  if (id >= s->graphemes.size()) { if (len) *len = 0; return nullptr; }
  if (len) *len = s->graphemes[id].size();
  return s->graphemes[id].data();
}
extern "C" void itcore_snapshot_cursor(const itcore_snapshot *s, uint16_t *col, uint16_t *row, int *visible) {
  if (col) *col = s->cur_col; if (row) *row = s->cur_row; if (visible) *visible = 1;
}
extern "C" uint64_t itcore_snapshot_generation(const itcore_snapshot *s) { return s ? s->generation : 0; }
extern "C" uint32_t itcore_snapshot_mouse_mode(const itcore_snapshot *s) { return s ? s->mouse_mode : 0; }
extern "C" void itcore_snapshot_release(itcore_snapshot *s) { delete s; }
