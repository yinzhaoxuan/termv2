#include <cstdio>
#include <fstream>
#include "uicfg/workspace.h"
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static void write(const fs::path& p, const char* s) { std::ofstream(p, std::ios::binary) << s; }

int main() {
  fs::path dir = fs::temp_directory_path() / "uicfg_ws_test";
  fs::remove_all(dir); fs::create_directories(dir);
  fs::path f = dir / "workspace.json";

  CHECK(loadWorkspace(f).status == LoadStatus::NotFound);

  WorkspaceFile w; w.connectionsRevision = 3;
  Tab t; t.title = "Ubuntu 开发机"; t.activePaneId = "pane-a";
  t.root = LayoutNode::split(SplitAxis::Columns, 0.4,
                             LayoutNode::pane("pane-a", "conn-ubuntu-dev"),
                             LayoutNode::pane("pane-b", "conn-ubuntu-dev"));
  w.tabs.push_back(std::move(t));
  CHECK(validateWorkspace(w).empty());

  std::string err;
  CHECK(saveWorkspace(f, w, false, &err) && w.revision == 1);
  auto r = loadWorkspace(f);
  CHECK(r.status == LoadStatus::Ok && r.data.revision == 1);
  CHECK(r.data.connectionsRevision == 3);
  CHECK(r.data.tabs.size() == 1);
  CHECK(r.data.tabs[0].root.kind == LayoutNode::Kind::Split);
  CHECK(r.data.tabs[0].root.ratio == 0.4);

  WorkspaceFile bad; bad.tabs.push_back(Tab{"t", LayoutNode::pane("p1", "c"), "missing"});
  CHECK(!validateWorkspace(bad).empty());
  CHECK(!saveWorkspace(dir / "bad.json", bad, false, &err));

  WorkspaceFile dup;
  dup.tabs.push_back(Tab{"t", LayoutNode::split(SplitAxis::Rows, 0.5,
                      LayoutNode::pane("p1", "c"), LayoutNode::pane("p1", "c")), "p1"});
  CHECK(!validateWorkspace(dup).empty());

  write(f, R"({"schemaVersion":1,"revision":1,"tabs":[{"title":"t","activePaneId":"a","root":{"type":"split","axis":"columns","ratio":1.5,"children":[{"type":"pane","paneId":"a","connectionId":"c"},{"type":"pane","paneId":"b","connectionId":"c"}]}}]})");
  auto badRatio = loadWorkspace(f);
  CHECK(badRatio.status == LoadStatus::Corrupt || badRatio.status == LoadStatus::RecoveredBackup);

  write(f, R"({"schemaVersion":99,"revision":1,"tabs":[]})");
  r = loadWorkspace(f);
  CHECK(r.status == LoadStatus::NewerSchema && r.readOnly);
  CHECK(!saveWorkspace(f, r.data, r.readOnly, &err));

  ConfigDirLock a, b;
  CHECK(a.tryAcquire(dir, &err));
  CHECK(!b.tryAcquire(dir, &err));
  a.release();
  CHECK(b.tryAcquire(dir, &err));
  b.release();

  // 连接删除 → MissingConnection；重新绑定；拒绝绑到不存在的 id；不按同名自动匹配
  ConnectionsFile cons;
  Connection c; c.id = "conn-a"; c.name = "Ubuntu 开发机"; c.kind = ConnectionKind::Ssh;
  c.endpoint = Endpoint{EndpointMode::Manual, std::string("h"), 22, std::nullopt};
  c.ssh = SshSettings{"u", AuthMode::Agent, std::nullopt, 15};
  cons.connections.push_back(c);
  Connection c2 = c; c2.id = "conn-b"; c2.name = "Ubuntu 开发机";  // 同名不同 id
  cons.connections.push_back(c2);

  WorkspaceFile ws;
  Tab tab; tab.title = "t"; tab.activePaneId = "p1";
  tab.root = LayoutNode::pane("p1", "conn-deleted");
  ws.tabs.push_back(std::move(tab));
  auto binds = resolvePaneBindings(ws, cons);
  CHECK(binds.size() == 1 && binds[0].status == PaneBindStatus::MissingConnection);

  CHECK(!rebindPane(ws, "p1", "conn-nope", cons, &err));  // 目标不存在
  CHECK(rebindPane(ws, "p1", "conn-b", cons, &err));       // 显式绑到 conn-b，即使有同名 conn-a
  binds = resolvePaneBindings(ws, cons);
  CHECK(binds[0].status == PaneBindStatus::Ok && binds[0].connectionId == "conn-b");
  CHECK(ws.tabs[0].root.connectionId == "conn-b");

  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}

