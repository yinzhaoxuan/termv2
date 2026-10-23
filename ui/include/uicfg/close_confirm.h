#pragma once
// 第 12.3：关闭窗格／标签／应用的确认汇总。只看 SessionState，永远只弹一次。
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/workspace.h"
#include "uishell/pane_backend.h"

namespace uicfg {

struct PaneSessionInfo {
  std::string paneId;
  std::string title;           // 窗格标题（会话配置名优先）
  std::string connectionName;  // 连接名；本地 shell 可为空
  uishell::SessionState state = uishell::SessionState::Starting;
};

enum class CloseScope { Pane, Tab, App };

struct CloseSummary {
  CloseScope scope = CloseScope::Pane;
  bool needsConfirm = false;           // 有任一 Starting/Running/Reconnecting
  int affectedCount = 0;               // 需要终止的运行中 Session 数
  std::vector<std::string> items;      // 每个运行中 Session 一行："标题 (连接名)"
  std::string message;                 // 汇总后的单条提示文案
};

bool stateNeedsConfirm(uishell::SessionState s);
std::vector<std::string> paneIds(const LayoutNode& root);

using PaneInfoLookup = std::function<std::optional<PaneSessionInfo>(const std::string& paneId)>;

CloseSummary summarizeClose(CloseScope scope, const std::vector<PaneSessionInfo>& panes);
CloseSummary summarizeClosePane(const PaneSessionInfo& pane);
CloseSummary summarizeCloseTab(const Tab& tab, const PaneInfoLookup& lookup);
CloseSummary summarizeQuit(const std::vector<Tab>& tabs, const PaneInfoLookup& lookup);

}  // namespace uicfg
