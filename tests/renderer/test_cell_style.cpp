#undef NDEBUG  // assert 在任何构建类型下都必须执行
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
  c.flags = ITCORE_CELL_SELECTED; r = resolve_cell(t, c);
  assert(r.fg == t.bg && r.bg == 0xFF0000);                    // 仅选中：反色
  c.flags = ITCORE_CELL_SELECTED | ITCORE_CELL_INVERSE; r = resolve_cell(t, c);
  assert(r.fg == t.bg && r.bg == 0xFF0000);                    // 选中+INVERSE：仍是反色，不会被反回原色

  // ?5 反显屏幕：只交换默认色，再应用 INVERSE
  itcore_cell d0{}; auto q = resolve_cell(t, d0, true); assert(q.fg == t.bg && q.bg == t.fg);
  d0.fg = 0x02FF0000; q = resolve_cell(t, d0, true); assert(q.fg == 0xFF0000 && q.bg == t.fg);
  d0.flags = ITCORE_CELL_INVERSE; q = resolve_cell(t, d0, true); assert(q.fg == t.fg && q.bg == 0xFF0000);
  // DIM：交换后 fg 向 bg 混 50%
  itcore_cell e{}; e.fg = 0x02FFFFFF; e.bg = 0x02000000; e.flags = ITREN_CELL_DIM;
  q = resolve_cell(t, e); assert(q.fg == 0x7F7F7F && q.bg == 0 && q.underline == 0x7F7F7F);
  e.flags = ITREN_CELL_DIM | ITCORE_CELL_INVERSE; q = resolve_cell(t, e); assert(q.bg == 0xFFFFFF && q.fg == 0x7F7F7F);
  assert(blend50(0x204060, 0x406080) == 0x305070);
  // HIDDEN：fg=bg；选中仍隐藏，仅背景反色
  e.flags = ITREN_CELL_HIDDEN; q = resolve_cell(t, e); assert(q.fg == 0 && q.bg == 0 && q.underline == 0);
  e.flags = ITREN_CELL_HIDDEN | ITCORE_CELL_SELECTED; q = resolve_cell(t, e); assert(q.fg == 0xFFFFFF && q.bg == 0xFFFFFF);
  // BLINK
  e.flags = ITREN_CELL_BLINK; assert(text_visible(e, 0) && text_visible(e, 999) && !text_visible(e, 1000) && text_visible(e, 2000));
  e.flags = 0; assert(text_visible(e, 1500));
  // STRIKE：格 8x16 pad 4 → y = 4+8-1；150% 格 12x24 → 粗 3
  GridMetrics gm; gm.cell_w_dip = 8; gm.cell_h_dip = 16;
  auto sr = strike_rect(gm, 1, 0, 2); assert(sr.x == 12 && sr.w == 16 && sr.h == 2 && sr.y == 11);
  gm.dpi_scale = 1.5f; sr = strike_rect(gm, 0, 0); assert(sr.h == 3 && sr.w == 12 && sr.y == 6 + 12 - 1);
  const char *txt = "abc\ndef";
  auto *s = itmock_snapshot_from_utf8(txt, strlen(txt), 20, 5);
  DirtyRows d;
  assert(d.collect(s).size() == 5);      // 首帧全画
  assert(d.collect(s).empty());          // 无变化不画
  itmock_set_style(s, 1, 0, ITCORE_CELL_BOLD, 0, 0);
  auto v = d.collect(s); assert(v.size() == 1 && v[0] == 1);  // 只画变了的行
  itmock_bump_epoch(s); assert(d.collect(s).size() == 5);  // epoch 变化整屏重画
  assert(d.collect(s).empty());
  d.invalidate(); assert(d.collect(s).size() == 5);
  // BLINK-01：只有含闪烁格的行在相位翻转时被标脏；闪烁全部消失后 any()=false（renderer 据此停 animating_）
  {
    BlinkRows br; br.resize(5); assert(!br.any());
    assert(d.collect(s).empty());
    br.set(1, true); br.set(3, true); br.set(3, true);
    auto rr = br.rows(); assert(rr.size() == 2 && rr[0] == 1 && rr[1] == 3 && br.any());
    for (auto y : rr) d.mark(y);
    auto dv = d.collect(s); assert(dv.size() == 2 && dv[0] == 1 && dv[1] == 3);
    assert(d.collect(s).empty());
    br.set(1, false); assert(br.any()); br.set(3, false); assert(!br.any() && br.rows().empty());
    br.set(2, true); br.resize(6); assert(!br.any());  // 尺寸变化清空
    br.set(9, true); assert(!br.any());               // 越界忽略
  }
  // BLINK-02：下划线随闪烁隐藏；位置/粗细按度量与 DPI（贴底留 1 逻辑像素）
  {
    itcore_cell u{}; u.flags = ITCORE_CELL_UNDERLINE | ITREN_CELL_BLINK;
    assert(underline_visible(u, 0) && !underline_visible(u, 1000) && underline_visible(u, 2000));
    u.flags = ITCORE_CELL_UNDERLINE; assert(underline_visible(u, 1000));
    u.flags = ITREN_CELL_BLINK; assert(!underline_visible(u, 0));
    GridMetrics g1; g1.cell_w_dip = 8; g1.cell_h_dip = 16;
    auto ur = underline_rect(g1, 1, 0, 2); assert(ur.x == 12 && ur.w == 16 && ur.h == 2 && ur.y == 4 + 16 - 2 - 1);
    g1.dpi_scale = 2.f; ur = underline_rect(g1, 0, 0); assert(ur.h == 4 && ur.y == 8 + 32 - 4 - 2);
  }
  itcore_snapshot_release(s);
  std::puts("cell_style ok");
}
