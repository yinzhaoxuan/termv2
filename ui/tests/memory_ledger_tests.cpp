// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/memory_ledger.h"
#include "uicfg/diag_bundle.h"
#include <algorithm>
#include <cassert>
#include <limits>
using namespace uicfg;
static constexpr std::uint64_t MiB = 1ull << 20;
int main() {
  LedgerLimits limits;
  auto d = balanceMemory({{"one", 150 * MiB, 140 * MiB, 200 * MiB}}, limits);
  assert(d.total == 128 * MiB && d.trims.size() == 1 && d.trims[0].newLimit == 118 * MiB);
  std::vector<SessionMemory> sessions{{"c", 120 * MiB, 80 * MiB, 100 * MiB},
      {"b", 120 * MiB, 90 * MiB, 100 * MiB}, {"a", 120 * MiB, 90 * MiB, 100 * MiB}};
  limits.app = 300 * MiB;
  d = balanceMemory(sessions, limits);
  assert(d.total == limits.app && d.trims.size() == 1);
  assert(d.trims[0].paneId == "a" && d.trims[0].newLimit == 30 * MiB);
  std::reverse(sessions.begin(), sessions.end());
  auto reversed = balanceMemory(sessions, limits);
  assert(reversed.total == d.total && reversed.trims.size() == d.trims.size());
  assert(reversed.trims[0].paneId == d.trims[0].paneId && reversed.trims[0].newLimit == d.trims[0].newLimit);
  limits.app = 100 * MiB;
  d = balanceMemory(sessions, limits);
  assert(d.trims.size() == 3 && d.total > limits.app);
  std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) { return a.paneId < b.paneId; });
  do {
    const auto permuted = balanceMemory(sessions, limits);
    assert(permuted.total == d.total && permuted.trims.size() == d.trims.size());
    for (size_t i = 0; i < d.trims.size(); ++i)
      assert(permuted.trims[i].paneId == d.trims[i].paneId && permuted.trims[i].newLimit == d.trims[i].newLimit);
  } while (std::next_permutation(sessions.begin(), sessions.end(),
      [](const auto& a, const auto& b) { return a.paneId < b.paneId; }));
  limits.app = 1;
  d = balanceMemory({{"a", 150 * MiB, 100 * MiB, 100 * MiB}, {"b", 80 * MiB, MiB, 2 * MiB}}, limits);
  assert(d.trims.size() == 1 && d.trims[0].newLimit == limits.minHistory);
  assert(d.total == 134 * MiB);
  // 两步合并为一条；空余上限不作为已释放内存。
  limits.app = 100 * MiB;
  d = balanceMemory({{"a", 150 * MiB, 100 * MiB, 200 * MiB}}, limits);
  assert(d.trims.size() == 1 && d.trims[0].newLimit == 50 * MiB && d.total == 100 * MiB);
  d = balanceMemory({{"a", 50 * MiB, 10 * MiB, 200 * MiB}}, limits);
  assert(d.trims.empty() && d.total == 50 * MiB);
  assert(balanceMemory({}, limits).total == 0);
  limits = {};
  assert(admitNewSession(limits.app - limits.newSessionEstimate, limits));
  assert(!admitNewSession(limits.app - limits.newSessionEstimate + 1, limits));
  assert(!admitNewSession(std::numeric_limits<std::uint64_t>::max(), limits));
  assert(memoryLine(12 * MiB + 3 * MiB / 10) == "12.3 MiB");
  assert(memoryLine(MiB - 1) == "1.0 MiB" && memoryLine(0) == "0.0 MiB");
  assert(memoryRefusedMessage(430 * MiB + MiB / 2, limits) ==
      "内存不足：已用 430.5 MiB，上限 512 MiB。请关闭一些窗格后重试。");
  assert(historyTrimmedNote("web-1", 37 * MiB) == "「web-1」的回滚历史已缩减到 37.0 MiB（内存上限）");
  DiagEnvironment env;
  env.memoryTotal = 100 * MiB; env.memoryMax = 60 * MiB; env.memorySessions = 2;
  const auto bundle = buildDiagnosticBundle(env, {}, {}, {});
  assert(bundle.find("内存（账本）= 合计 100.0 MiB / 512 MiB，2 个会话；最大 60.0 MiB") != std::string::npos);
  assert(bundle.find("账本不含传输队列") != std::string::npos);
}
