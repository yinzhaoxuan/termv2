#include "uicfg/session_tree.h"
#include <algorithm>
#include <cctype>
namespace uicfg {
static std::string lower(std::string s) {
  for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
static std::string hostOf(const Connection& c) {
  if (c.kind == ConnectionKind::Local) return c.local ? c.local->executable : "";
  if (!c.endpoint) return "";
  if (c.endpoint->mode == EndpointMode::HyperV && c.endpoint->hyperv) return c.endpoint->hyperv->vmId;
  return c.endpoint->host.value_or("");
}
std::vector<TreeGroup> buildSessionTree(const ConnectionsFile& cf, const std::string& filter,
                                        const std::map<std::string, int>& running) {
  std::vector<Group> gs = cf.groups;
  std::stable_sort(gs.begin(), gs.end(), [](auto& a, auto& b) { return a.order < b.order; });
  std::vector<TreeGroup> out;
  for (auto& g : gs) out.push_back({g.id, g.name, {}});
  TreeGroup ungrouped{"", "未分组", {}};
  std::vector<const Connection*> cs;
  for (auto& c : cf.connections) cs.push_back(&c);
  std::stable_sort(cs.begin(), cs.end(), [](auto a, auto b) { return a->order < b->order; });
  std::string f = lower(filter);
  for (auto* c : cs) {
    TreeItem it{c->id, c->name, hostOf(*c), c->kind, 0};
    if (auto r = running.find(c->id); r != running.end()) it.runningSessions = r->second;
    if (!f.empty() && lower(it.name).find(f) == std::string::npos && lower(it.host).find(f) == std::string::npos) continue;
    auto g = std::find_if(out.begin(), out.end(), [&](auto& x) { return x.id == c->groupId; });
    (g != out.end() && !c->groupId.empty() ? g->items : ungrouped.items).push_back(std::move(it));
  }
  if (!ungrouped.items.empty()) out.push_back(std::move(ungrouped));
  if (!f.empty()) out.erase(std::remove_if(out.begin(), out.end(), [](auto& g) { return g.items.empty(); }), out.end());
  return out;
}
const Connection* SessionSnapshots::snapshot(const std::string& p) const {
  auto it = snaps_.find(p); return it == snaps_.end() ? nullptr : &it->second;
}
std::map<std::string, int> SessionSnapshots::runningByConnection() const {
  std::map<std::string, int> m; for (auto& [_, c] : snaps_) ++m[c.id]; return m;
}
bool deleteConnection(ConnectionsFile& cf, const std::string& id) {
  auto n = std::erase_if(cf.connections, [&](auto& c) { return c.id == id; });
  return n > 0;
}
bool updateConnection(ConnectionsFile& cf, const Connection& c) {
  for (auto& x : cf.connections) if (x.id == c.id) { x = c; return true; }
  return false;
}
std::vector<PaneBindInfo> missingReferencePanes(const WorkspaceFile& w, const ConnectionsFile& cf) {
  auto all = resolvePaneBindings(w, cf);
  std::erase_if(all, [](auto& b) { return b.status == PaneBindStatus::Ok; });
  return all;
}
}  // namespace uicfg
