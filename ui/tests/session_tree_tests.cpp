#include "uicfg/session_tree.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg;
static Connection ssh(std::string id, std::string name, std::string g, std::string host, int order) {
  Connection c; c.id = id; c.name = name; c.groupId = g; c.order = order;
  c.endpoint = Endpoint{}; c.endpoint->host = host; c.ssh = SshSettings{"u"}; return c;
}
int main() {
  ConnectionsFile cf;
  cf.groups = {{"local", "本地", 2}, {"dev", "开发", 1}, {"empty", "空", 3}};
  cf.connections = {ssh("c2", "Build", "dev", "10.0.0.2", 2), ssh("c1", "Ubuntu", "dev", "192.168.77.20", 1),
                    ssh("c3", "Orphan", "gone", "o.example", 0)};
  Connection ps; ps.id = "c4"; ps.name = "PowerShell"; ps.kind = ConnectionKind::Local; ps.groupId = "local";
  ps.local = LocalSettings{"pwsh.exe"}; cf.connections.push_back(ps);
  auto t = buildSessionTree(cf, "", {});
  CHECK(t.size() == 4 && t[0].id == "dev" && t[1].id == "local" && t[2].id == "empty" && t[3].id.empty());
  CHECK(t[0].items[0].connectionId == "c1" && t[0].items[1].connectionId == "c2");
  CHECK(t[3].items[0].connectionId == "c3" && t[1].items[0].host == "pwsh.exe");
  auto f = buildSessionTree(cf, "UBU", {});
  CHECK(f.size() == 1 && f[0].items.size() == 1 && f[0].items[0].name == "Ubuntu");
  f = buildSessionTree(cf, "10.0.0", {});
  CHECK(f.size() == 1 && f[0].items[0].connectionId == "c2");
  CHECK(buildSessionTree(cf, "zzz", {}).empty());

  SessionSnapshots snaps;
  snaps.onSessionStarted("p1", cf.connections[1]); snaps.onSessionStarted("p2", cf.connections[1]);
  t = buildSessionTree(cf, "", snaps.runningByConnection());
  CHECK(t[0].items[0].runningSessions == 2 && t[0].items[0].hasRunning() && !t[0].items[1].hasRunning());
  // 修改：只影响新 Session
  auto edited = cf.connections[1]; edited.endpoint->host = "10.9.9.9";
  CHECK(updateConnection(cf, edited));
  CHECK(snaps.snapshot("p1")->endpoint->host == std::string("192.168.77.20"));
  // 删除：运行中的保留快照
  CHECK(deleteConnection(cf, "c1") && !deleteConnection(cf, "c1"));
  CHECK(snaps.snapshot("p2") && snaps.snapshot("p2")->name == "Ubuntu");
  snaps.onSessionEnded("p1"); CHECK(!snaps.snapshot("p1"));
  // 恢复布局：引用缺失标记
  WorkspaceFile w; Tab tab; tab.root = LayoutNode::split(SplitAxis::Columns, .5, LayoutNode::pane("p1", "c1"), LayoutNode::pane("p3", "c2"));
  w.tabs.push_back(std::move(tab));
  auto miss = missingReferencePanes(w, cf);
  CHECK(miss.size() == 1 && miss[0].paneId == "p1" && miss[0].status == PaneBindStatus::MissingConnection);
  if (!fails) std::printf("session_tree_tests OK\n");
  return fails ? 1 : 0;
}
