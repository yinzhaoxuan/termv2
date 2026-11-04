// SPDX-License-Identifier: GPL-3.0-or-later
#include "itcore.h"
#include "screen.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <thread>

static itcore_core* make() {
  itcore_options opt{ITCORE_ABI_VERSION, sizeof(opt), 80, 5, 10000, 64ull << 20};
  itcore_core* c = nullptr;
  assert(itcore_create(&opt, &c) == 0);
  return c;
}
static void drain(itcore_core* c) {
  while (auto* e = itcore_next_event(c)) itcore_event_release(e);
}
static void feed(itcore_core* c, const std::string& text, bool takeEvents = true) {
  auto r = itcore_feed(c, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  assert(r.status == ITCORE_OK && r.accepted == text.size());
  if (takeEvents) drain(c);
}
static itcore_memory_usage usage(itcore_core* c) {
  itcore_memory_usage u{}; u.struct_size = sizeof(u);
  assert(itcore_memory_usage_get(c, &u) == 0);
  assert(u.total_bytes == u.grid_bytes + u.scrollback_bytes + u.pool_bytes + u.queue_bytes + u.search_bytes);
  return u;
}
static void measurement() {
  auto* c = make();
  const auto initial = usage(c);
  assert(initial.grid_bytes && initial.pool_bytes && initial.pool_epoch);
  assert(!initial.scrollback_bytes && !initial.search_bytes && !initial.queue_bytes);
  itcore_memory_usage invalid{};
  assert(itcore_memory_usage_get(c, &invalid) == -1);
  assert(itcore_memory_usage_get(c, nullptr) == -1 && itcore_memory_usage_get(nullptr, &invalid) == -1);
  struct Extended { itcore_memory_usage u; uint64_t sentinel; } ext{};
  ext.u.struct_size = sizeof(ext); ext.sentinel = 123;
  assert(itcore_memory_usage_get(c, &ext.u) == 0 && ext.sentinel == 123);
  for (int i = 0; i < 100; ++i) feed(c, std::string(80, 'x') + "\r\n");
  assert(usage(c).scrollback_bytes > initial.scrollback_bytes);
  feed(c, "\x1b]2;title\x07", false);
  assert(usage(c).queue_bytes > 0);
  drain(c); assert(usage(c).queue_bytes == 0);
  feed(c, "\x1b]2;unfinished", false);
  assert(usage(c).queue_bytes >= std::strlen("2;unfinished"));
  feed(c, "\x07"); assert(usage(c).queue_bytes == 0);
  itcore_search_options opt{}; opt.struct_size = sizeof(opt);
  itcore_search* search = nullptr;
  assert(itcore_search_begin(c, "x", 1, &opt, &search) == 0);
  const auto started = usage(c).search_bytes; assert(started > 0);
  itcore_search_progress progress{}; progress.struct_size = sizeof(progress);
  uint32_t count = 0;
  itcore_search_step(search, 100000, 0, &progress, nullptr, 0, &count);
  assert(usage(c).search_bytes > started);
  itcore_search_end(search); assert(usage(c).search_bytes == 0);
  feed(c, "\x1b[3J"); assert(usage(c).scrollback_bytes == 0);
  for (int i = 0; i < 20; ++i) feed(c, "hello\r\n");
  assert(usage(c).scrollback_bytes > 0);
  feed(c, "\x1b" "c"); assert(usage(c).scrollback_bytes == 0);
  itcore_destroy(c);
}
static void limits() {
  auto* c = make();
  for (int i = 0; i < 100; ++i) feed(c, std::string(80, 'x') + "\r\n");
  const auto rows = itcore_history_rows(c);
  auto* snapshot = itcore_acquire_snapshot(c, -10, 5); assert(snapshot);
  const auto* row = itcore_snapshot_row(snapshot, 0, nullptr, nullptr); assert(row);
  const auto id = row[0].grapheme_id;
  assert(itcore_set_history_limits(c, 0, 4096) == 0);
  assert(usage(c).scrollback_bytes <= 4096 && usage(c).history_limit_bytes == 4096);
  assert(itcore_history_rows(c) < rows);
  assert(itcore_set_history_limits(c, 1, 0) == 0);
  assert(itcore_history_rows(c) <= 1 && usage(c).history_limit_bytes == 4096);
  assert(itcore_set_history_limits(c, 0, 0) == 0);
  assert(itcore_set_history_limits(nullptr, 1, 1) == -1);
  itcore_destroy(c);
  size_t len = 0;
  assert(std::string(itcore_snapshot_grapheme(snapshot, id, &len)) == "x");
  assert(len == 1); itcore_snapshot_release(snapshot);
  // 运行时预算重设阈值，立即触发原有压缩路径。
  itcore::Screen screen(20, 5, 100, 64ull << 20);
  screen.pool().intern(std::string(500000, 'z'));
  const auto epoch = screen.pool().epoch();
  screen.set_history_limits(0, 16ull << 20);
  assert(screen.pool_budget() == 2ull << 20 && screen.pool().epoch() == epoch);
  screen.set_history_limits(0, 8ull << 20);
  assert(screen.pool_budget() == 1ull << 20 && screen.pool().epoch() > epoch);
  screen.set_history_limits(0, 1);
  assert(screen.pool_budget() == 1ull << 20);
}
static void snapshots() {
  assert(itcore_snapshot_row_bytes(nullptr) == 0 && itcore_snapshot_pool_bytes(nullptr) == 0);
  auto* c = make(); feed(c, "A");
  auto* snapshot = itcore_acquire_snapshot(c, 0, 5); assert(snapshot);
  const auto epoch = itcore_snapshot_epoch(snapshot);
  const auto rows = itcore_snapshot_row_bytes(snapshot);
  assert(rows > 0 && itcore_snapshot_pool_bytes(snapshot) == usage(c).pool_bytes);
  // 与写端追加及换 store 并发，读端不访问核心。
  std::atomic<bool> done{false}, reading{false};
  std::thread reader([&] {
    reading.store(true);
    while (!done.load()) {
      assert(itcore_snapshot_epoch(snapshot) == epoch);
      assert(itcore_snapshot_row_bytes(snapshot) == rows);
      assert(itcore_snapshot_pool_bytes(snapshot) > 0);
      const auto* row = itcore_snapshot_row(snapshot, 0, nullptr, nullptr);
      size_t len = 0;
      const auto* text = itcore_snapshot_grapheme(snapshot, row[0].grapheme_id, &len);
      assert(len == 1 && *text == 'A');
    }
  });
  while (!reading.load()) std::this_thread::yield();
  for (int i = 0; i < 100; ++i) feed(c, "\x1b]8;;https://example/" + std::to_string(i) + "\x1b\\x\x1b]8;;\x1b\\");
  const auto oldBytes = itcore_snapshot_pool_bytes(snapshot);
  feed(c, "\x1b[3J");
  assert(usage(c).pool_epoch != epoch && itcore_snapshot_pool_bytes(snapshot) == oldBytes);
  auto* newer = itcore_acquire_snapshot(c, 0, 5);
  assert(itcore_snapshot_epoch(newer) == usage(c).pool_epoch);
  assert(itcore_snapshot_pool_bytes(newer) == usage(c).pool_bytes);
  itcore_destroy(c); done.store(true); reader.join();
  itcore_snapshot_release(newer); itcore_snapshot_release(snapshot);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  if (mode == "measurement") measurement();
  else if (mode == "limits") limits();
  else if (mode == "snapshots") snapshots();
  else assert(false);
}
