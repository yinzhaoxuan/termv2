// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_logic.h"
#include "uicfg/memory_ledger.h"
#include <cassert>
#include <memory>
#include <string>
#include <vector>
using Core = std::unique_ptr<itcore_core, decltype(&itcore_destroy)>;
using Snapshot = std::unique_ptr<itcore_snapshot, decltype(&itcore_snapshot_release)>;
static Core make(uint16_t cols = 80, uint64_t limit = 64ull << 20) {
  itcore_options opt{ITCORE_ABI_VERSION, sizeof(opt), cols, 5, 100000, limit};
  itcore_core* c = nullptr; assert(itcore_create(&opt, &c) == 0);
  return Core(c, itcore_destroy);
}
static void feed(itcore_core* c, const std::string& text) {
  const auto r = itcore_feed(c, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  assert(r.status == ITCORE_OK && r.accepted == text.size());
  while (auto* event = itcore_next_event(c)) itcore_event_release(event);
}
static itcore_memory_usage usage(itcore_core* c) {
  itcore_memory_usage u{}; u.struct_size = sizeof(u);
  assert(itcore_memory_usage_get(c, &u) == 0); return u;
}
static void totals() {
  it2app::SessionMemoryInputs input;
  input.core.total_bytes = 100; input.core.pool_epoch = 1;
  input.snapshotRowBytes = 20; input.snapshotPoolBytes = 50; input.snapshotEpoch = 1;
  input.archiveBytes = 7;
  assert(it2app::sessionMemoryTotal(input) == 107);
  input.hasSnapshot = true; assert(it2app::sessionMemoryTotal(input) == 127);
  input.snapshotEpoch = 2; assert(it2app::sessionMemoryTotal(input) == 177);
  input.core = {}; assert(it2app::sessionMemoryTotal(input) == 77);
  assert(it2app::sessionMemoryTotal({}) == 0);
}
static void sessionLimit() {
  constexpr uint64_t historyLimit = 144ull << 20;
  auto c = make(4096, historyLimit);
  for (int row = 0; row < 1600; ++row) feed(c.get(), "\x1b[4096Gx\r\n");
  const auto before = usage(c.get());
  const uicfg::LedgerLimits limits;
  assert(before.total_bytes > limits.session);
  const auto decision = uicfg::balanceMemory(
      {{"one", before.total_bytes, before.scrollback_bytes, before.history_limit_bytes}}, limits);
  assert(decision.trims.size() == 1);
  assert(itcore_set_history_limits(c.get(), 0, decision.trims[0].newLimit) == 0);
  feed(c.get(), "");
  assert(usage(c.get()).total_bytes <= limits.session);
  // 后续输出继续遵守缩减后的限额。
  for (int row = 0; row < 20; ++row) feed(c.get(), "\x1b[4096Gx\r\n");
  assert(usage(c.get()).total_bytes <= limits.session);
}
static void balance() {
  sessionLimit();
  constexpr uint64_t historyLimit = 64ull << 20;
  std::vector<Core> cores;
  std::vector<uicfg::SessionMemory> sessions;
  for (int i = 0; i < 10; ++i) {
    auto c = make(4096, historyLimit);
    // 最后一列输出，保留整行；少量输入即可达到真实的 64 MiB 历史限额。
    for (int row = 0; row < 700; ++row) feed(c.get(), "\x1b[4096Gx\r\n");
    const auto u = usage(c.get());
    assert(u.scrollback_bytes <= historyLimit && u.scrollback_bytes > historyLimit - (1ull << 20));
    sessions.push_back({std::to_string(i), u.total_bytes, u.scrollback_bytes, u.history_limit_bytes});
    cores.push_back(std::move(c));
  }
  const uicfg::LedgerLimits limits;
  const auto decision = uicfg::balanceMemory(sessions, limits);
  assert(!decision.trims.empty() && decision.total <= limits.app);
  for (const auto& trim : decision.trims) {
    const auto index = static_cast<size_t>(std::stoul(trim.paneId));
    assert(itcore_set_history_limits(cores[index].get(), 0, trim.newLimit) == 0);
  }
  uint64_t total = 0;
  for (const auto& c : cores) {
    feed(c.get(), "");
    const auto u = usage(c.get());
    assert(u.total_bytes <= limits.session && u.scrollback_bytes <= u.history_limit_bytes);
    total += u.total_bytes;
  }
  assert(total <= limits.app);
}
static void reconnect() {
  uint64_t baseline = 0;
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto c = make();
    const auto initial = usage(c.get());
    if (!attempt) baseline = initial.total_bytes;
    assert(initial.total_bytes == baseline);
    for (int line = 0; line < 12; ++line) feed(c.get(), "saved text\r\n");
    Snapshot snapshot(itcore_acquire_snapshot(c.get(), -5, 5), itcore_snapshot_release);
    assert(snapshot);
    it2app::SessionMemoryInputs input;
    input.core = usage(c.get()); input.hasSnapshot = true;
    input.snapshotEpoch = itcore_snapshot_epoch(snapshot.get());
    input.snapshotPoolBytes = itcore_snapshot_pool_bytes(snapshot.get());
    input.snapshotRowBytes = itcore_snapshot_row_bytes(snapshot.get());
    assert(it2app::sessionMemoryTotal(input) == input.core.total_bytes + input.snapshotRowBytes);
    feed(c.get(), "\x1b[3J");
    input.core = usage(c.get());
    assert(input.core.pool_epoch != input.snapshotEpoch);
    assert(it2app::sessionMemoryTotal(input) == input.core.total_bytes + input.snapshotRowBytes + input.snapshotPoolBytes);
    const auto* row = itcore_snapshot_row(snapshot.get(), 0, nullptr, nullptr); assert(row);
    size_t len = 0;
    const auto* text = itcore_snapshot_grapheme(snapshot.get(), row[0].grapheme_id, &len);
    assert(len == 1 && *text == 's');
    snapshot.reset();
    feed(c.get(), "\x1b" "c");
    assert(usage(c.get()).total_bytes == baseline);
  }
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  if (mode == "totals") totals();
  else if (mode == "balance") balance();
  else if (mode == "reconnect") reconnect();
  else assert(false);
}
