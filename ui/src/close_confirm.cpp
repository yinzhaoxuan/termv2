#include "uicfg/close_confirm.h"
namespace uicfg {
bool stateNeedsConfirm(uishell::SessionState s) {
  using S = uishell::SessionState;
  return s == S::Starting || s == S::Running || s == S::Reconnecting;
}
static void collect(const LayoutNode& n, std::vector<std::string>& out) {
  if (n.kind == LayoutNode::Kind::Pane) { out.push_back(n.paneId); return; }
  if (n.first) collect(*n.first, out);
  if (n.second) collect(*n.second, out);
}
std::vector<std::string> paneIds(const LayoutNode& root) { std::vector<std::string> v; collect(root, v); return v; }

CloseSummary summarizeClose(CloseScope scope, const std::vector<PaneSessionInfo>& panes) {
  CloseSummary s; s.scope = scope;
  for (auto& p : panes) {
    if (!stateNeedsConfirm(p.state)) continue;
    std::string line = p.title.empty() ? p.connectionName : p.title;
    if (!p.connectionName.empty() && p.connectionName != line) line += " (" + p.connectionName + ")";
    s.items.push_back(line);
  }
  s.affectedCount = static_cast<int>(s.items.size());
  s.needsConfirm = s.affectedCount > 0;
  if (s.needsConfirm) {
    const char* what = scope == CloseScope::Pane ? "关闭此窗格" : scope == CloseScope::Tab ? "关闭此标签" : "退出应用";
    s.message = std::string(what) + "将终止 " + std::to_string(s.affectedCount) + " 个运行中的会话：";
    for (size_t i = 0; i < s.items.size(); ++i) s.message += (i ? "、" : "") + s.items[i];
  }
  return s;
}
CloseSummary summarizeClosePane(const PaneSessionInfo& p) { return summarizeClose(CloseScope::Pane, {p}); }
static void gather(const Tab& t, const PaneInfoLookup& lk, std::vector<PaneSessionInfo>& out) {
  for (auto& id : paneIds(t.root)) if (auto i = lk(id)) out.push_back(*i);
}
CloseSummary summarizeCloseTab(const Tab& t, const PaneInfoLookup& lk) {
  std::vector<PaneSessionInfo> v; gather(t, lk, v); return summarizeClose(CloseScope::Tab, v);
}
CloseSummary summarizeQuit(const std::vector<Tab>& tabs, const PaneInfoLookup& lk) {
  std::vector<PaneSessionInfo> v; for (auto& t : tabs) gather(t, lk, v); return summarizeClose(CloseScope::App, v);
}
}  // namespace uicfg
