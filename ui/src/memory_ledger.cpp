// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/memory_ledger.h"
#include <algorithm>
#include <limits>

namespace uicfg {
namespace {
struct Candidate {
  const SessionMemory* source;
  std::uint64_t history, limit, total;
};
// 仅按实际历史的减少量扣减，调低空余限额不虚扣。
void trimHistory(Candidate& c, std::uint64_t excess, std::uint64_t minimum) {
  if (c.history <= minimum) return;
  const auto target = c.history - std::min(excess, c.history - minimum);
  if (target >= c.limit) return;
  const auto removed = std::min(c.history - target, c.total);
  c.limit = target;
  c.history -= removed;
  c.total -= removed;
}
std::uint64_t totalOf(const std::vector<Candidate>& candidates) {
  std::uint64_t total = 0;
  for (const auto& c : candidates) {
    if (c.total > std::numeric_limits<std::uint64_t>::max() - total)
      return std::numeric_limits<std::uint64_t>::max();
    total += c.total;
  }
  return total;
}
}  // namespace
LedgerDecision balanceMemory(const std::vector<SessionMemory>& sessions, const LedgerLimits& limits) {
  std::vector<Candidate> candidates;
  for (const auto& s : sessions) {
    Candidate c{&s, s.history, s.historyLimit, s.total};
    if (c.total > limits.session) trimHistory(c, c.total - limits.session, limits.minHistory);
    candidates.push_back(c);
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    if (a.source->history != b.source->history) return a.source->history > b.source->history;
    return a.source->paneId < b.source->paneId;
  });
  auto total = totalOf(candidates);
  for (auto& c : candidates) {
    if (total <= limits.app) break;
    trimHistory(c, total - limits.app, limits.minHistory);
    total = totalOf(candidates);
  }
  LedgerDecision result{total, {}};
  for (const auto& c : candidates)
    if (c.limit < c.source->historyLimit) result.trims.push_back({c.source->paneId, c.limit});
  return result;
}
bool admitNewSession(std::uint64_t currentTotal, const LedgerLimits& limits) {
  return currentTotal <= limits.app && limits.newSessionEstimate <= limits.app - currentTotal;
}
std::string memoryLine(std::uint64_t bytes) {
  constexpr std::uint64_t mib = 1ull << 20;
  // 商与余数分开，避免整数溢出及浮点、区域设置差异。
  auto whole = bytes / mib;
  auto tenth = ((bytes % mib) * 10 + mib / 2) / mib;
  if (tenth == 10) { ++whole; tenth = 0; }
  return std::to_string(whole) + "." + std::to_string(tenth) + " MiB";
}
std::string memoryRefusedMessage(std::uint64_t currentTotal, const LedgerLimits& limits) {
  const auto cap = limits.app % (1ull << 20) == 0 ? std::to_string(limits.app >> 20) + " MiB" : memoryLine(limits.app);
  return "内存不足：已用 " + memoryLine(currentTotal) + "，上限 " + cap +
      "。请关闭一些窗格后重试。";
}
std::string historyTrimmedNote(const std::string& connectionName, std::uint64_t newLimit) {
  return "「" + connectionName + "」的回滚历史已缩减到 " + memoryLine(newLimit) + "（内存上限）";
}
}  // namespace uicfg
