#include "cell_style.h"
#include "itcore_mock.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace itren;
int main() {
  Theme t;
  assert(palette256(t, 1) == 0xCD3131);
  assert(palette256(t, 16) == 0x000000 && palette256(t, 231) == 0xFFFFFF);
  assert(palette256(t, 196) == 0xFF0000);
  assert(palette256(t, 232) == 0x080808 && palette256(t, 255) == 0xEEEEEE);
  assert(resolve(t, 0x00000000, true) == t.fg && resolve(t, 0x00000000, false) == t.bg);
  assert(resolve(t, 0x01C40000, true) == 0xFF0000);   // 调色板 196
  assert(resolve(t, 0x02123456, true) == 0x123456);   // 真彩
  itcore_cell c{}; c.fg = 0x02FF0000; c.bg = 0x00000000; c.flags = ITCORE_CELL_INVERSE;
  auto r = resolve_cell(t, c);
  assert(r.fg == t.bg && r.bg == 0xFF0000 && r.underline == r.fg);

  const char *txt = "abc\ndef";
  auto *s = itmock_snapshot_from_utf8(txt, strlen(txt), 20, 5);
  DirtyRows d;
  assert(d.collect(s).size() == 5);      // 首帧全画
  assert(d.collect(s).empty());          // 无变化不画
  itmock_set_style(s, 1, 0, ITCORE_CELL_BOLD, 0, 0);
  auto v = d.collect(s); assert(v.size() == 1 && v[0] == 1);  // 只画变了的行
  d.invalidate(); assert(d.collect(s).size() == 5);
  itcore_snapshot_release(s);
  std::puts("cell_style ok");
}
