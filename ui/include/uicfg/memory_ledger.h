// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace uicfg {
struct LedgerLimits {
  std::uint64_t session = 128ull << 20;
  std::uint64_t app = 512ull << 20;
  std::uint64_t newSessionEstimate = 16ull << 20;
  std::uint64_t minHistory = 4ull << 20;
};
struct SessionMemory { std::string paneId; std::uint64_t total, history, historyLimit; };
struct HistoryTrim { std::string paneId; std::uint64_t newLimit; };
struct LedgerDecision { std::uint64_t total = 0; std::vector<HistoryTrim> trims; };
// 拉取式账本，不含传输队列、显存与 DWrite 缓存。total 为应用 trim 后的估计值。
LedgerDecision balanceMemory(const std::vector<SessionMemory>&, const LedgerLimits&);
bool admitNewSession(std::uint64_t currentTotal, const LedgerLimits&);
std::string memoryRefusedMessage(std::uint64_t currentTotal, const LedgerLimits&);
std::string historyTrimmedNote(const std::string& connectionName, std::uint64_t newLimit);
std::string memoryLine(std::uint64_t bytes);
}  // namespace uicfg
