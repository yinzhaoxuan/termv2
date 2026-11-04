// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include "pane_logic.h"
#include "cell_style.h"
#include <cassert>
#include <string>

static std::string feed(itcore_core* core, const std::string& bytes) {
  const auto r = itcore_feed(core, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  assert(r.accepted == bytes.size());
  std::string reply;
  while (auto* event = itcore_next_event(core)) {
    if (itcore_event_type(event) == ITCORE_EV_REPLY_BYTES) {
      size_t n = 0; const auto* p = itcore_event_data(event, &n);
      reply.append(reinterpret_cast<const char*>(p), n);
    }
    itcore_event_release(event);
  }
  return reply;
}

static itren::Theme snapshotTheme(itcore_core* core) {
  auto* snap = itcore_acquire_snapshot(core, 0, 5);
  assert(snap);
  itcore_colors colors{}; itcore_snapshot_colors(snap, &colors);
  const auto theme = itren::themeFromColors(colors, itren::kDefaultSearchColors);
  itcore_snapshot_release(snap);
  return theme;
}

int main() {
  uishell::PaneColors colors{0x112233, 0x445566, 0x778899, {}};
  for (size_t i = 0; i < colors.ansi.size(); ++i) colors.ansi[i] = uint32_t(0x102030 + i);
  const auto mapped = it2app::toCoreColors(colors);
  itcore_colors defaults{}; itcore_default_colors(&defaults);
  assert(mapped.struct_size == sizeof(itcore_colors));
  assert(mapped.foreground == colors.foreground && mapped.background == colors.background && mapped.cursor == colors.cursor);
  const auto coreAppearance = it2app::coreDefaultAppearance();
  assert(coreAppearance.colors.foreground == defaults.foreground && coreAppearance.colors.background == defaults.background);
  assert(coreAppearance.colors.cursor == defaults.cursor && coreAppearance.scrollbackLines == 0 && coreAppearance.memoryLimitBytes == 0);
  for (size_t i = 0; i < 16; ++i) assert(coreAppearance.colors.ansi[i] == defaults.palette[i]);
  const itren::Theme fallback;
  for (size_t i = 0; i < 256; ++i) {
    assert(mapped.palette[i] == (i < 16 ? colors.ansi[i] : defaults.palette[i]));
    if (i >= 16) assert(fallback.palette[i] == defaults.palette[i]);
  }
  itcore_options options{};
  options.abi_version = ITCORE_ABI_VERSION; options.struct_size = sizeof options;
  options.columns = 20; options.rows = 5;
  itcore_core* core = nullptr;
  assert(itcore_create(&options, &core) == 0 && core);
  assert(itcore_set_colors(core, &mapped) == 0);
  const auto configured = snapshotTheme(core);
  assert(configured == itren::themeFromColors(mapped, itren::kDefaultSearchColors));
  // OSC 覆盖贯穿快照与渲染；查询值与用于绘制的主题一致。
  feed(core, "\x1b]11;#336699\a\x1b]10;#abcdef\a\x1b]12;#fedcba\a\x1b]4;200;#123456\a");
  const auto changed = snapshotTheme(core);
  assert(changed.bg == 0x336699 && changed.fg == 0xabcdef && changed.cursor == 0xfedcba && changed.palette[200] == 0x123456);
  assert(feed(core, "\x1b]11;?\a") == "\x1b]11;rgb:3333/6666/9999\a");
  feed(core, "\x1b]111\a\x1b]110\a\x1b]112\a\x1b]104\a");
  assert(snapshotTheme(core) == configured);
  feed(core, "\x1b]11;#ffffff\a");
  const auto replacement = it2app::toCoreColors({0x010203, 0x040506, 0x070809, colors.ansi});
  assert(itcore_set_colors(core, &replacement) == 0);
  assert(snapshotTheme(core).bg == replacement.background);  // 热切换清掉 OSC 覆盖
  itcore_destroy(core);
}
