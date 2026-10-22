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

  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
