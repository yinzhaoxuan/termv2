#include "uicfg/close_confirm.h"
#include <map>
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg; using S = uishell::SessionState;
int main() {
  CHECK(stateNeedsConfirm(S::Starting) && stateNeedsConfirm(S::Running) && stateNeedsConfirm(S::Reconnecting));
  CHECK(!stateNeedsConfirm(S::Exited) && !stateNeedsConfirm(S::Failed));
  CHECK(!summarizeClosePane({"a", "Ubuntu", "Ubuntu", S::Exited}).needsConfirm);
  auto p = summarizeClosePane({"a", "vim", "Ubuntu", S::Running});
  CHECK(p.needsConfirm && p.affectedCount == 1 && p.items[0] == "vim (Ubuntu)");
  std::map<std::string, PaneSessionInfo> m{{"a", {"a", "Ubuntu", "Ubuntu", S::Running}},
    {"b", {"b", "Ubuntu", "Ubuntu", S::Failed}}, {"c", {"c", "PS", "", S::Reconnecting}},
    {"d", {"d", "x", "Dev", S::Exited}}};
  PaneInfoLookup lk = [&](const std::string& id) -> std::optional<PaneSessionInfo> {
    auto it = m.find(id); if (it == m.end()) return std::nullopt; return it->second; };
  Tab t1; t1.root = LayoutNode::split(SplitAxis::Columns, .5, LayoutNode::pane("a", "c1"),
      LayoutNode::split(SplitAxis::Rows, .5, LayoutNode::pane("b", "c1"), LayoutNode::pane("c", "")));
  CHECK(paneIds(t1.root).size() == 3);
  auto ts = summarizeCloseTab(t1, lk);
  CHECK(ts.needsConfirm && ts.affectedCount == 2 && ts.items.size() == 2 && !ts.message.empty());
  Tab t2; t2.root = LayoutNode::pane("d", "c2");
  CHECK(!summarizeCloseTab(t2, lk).needsConfirm);
  std::vector<Tab> tabs; tabs.push_back(std::move(t1)); tabs.push_back(std::move(t2));
  auto q = summarizeQuit(tabs, lk);   // 一次汇总，不按窗格弹
  CHECK(q.scope == CloseScope::App && q.affectedCount == 2);
  CHECK(!summarizeQuit({}, lk).needsConfirm);
  if (!fails) std::printf("close_confirm_tests OK\n");
  return fails ? 1 : 0;
}
