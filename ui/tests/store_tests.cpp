#include <cstdio>
#include <fstream>
#include "uicfg/store.h"
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection sample() {
  Connection c; c.id = "conn-ubuntu-dev"; c.name = "Ubuntu 开发机"; c.groupId = "group-dev";
  c.appearanceProfileId = "appearance-default"; c.kind = ConnectionKind::Ssh;
  c.endpoint = Endpoint{EndpointMode::HyperV, std::nullopt, 22,
                        HyperVEndpoint{"local", "00000000-0000-0000-0000-000000000001", std::nullopt, "ipv4", std::nullopt}};
  c.ssh = SshSettings{"developer", AuthMode::Agent, std::nullopt, 15};
  return c;
}
static void write(const fs::path& p, const char* s) { std::ofstream(p, std::ios::binary) << s; }

int main() {
  fs::path dir = fs::temp_directory_path() / "uicfg_store_test";
  fs::remove_all(dir); fs::create_directories(dir);
  fs::path f = dir / "connections.json";

  CHECK(loadConnections(f).status == LoadStatus::NotFound);

  ConnectionsFile d; d.groups.push_back({"group-dev", "开发", 0}); d.connections.push_back(sample());
  std::string err;
  CHECK(saveConnections(f, d, false, &err) && d.revision == 1);
  auto r = loadConnections(f);
  CHECK(r.status == LoadStatus::Ok && r.data.revision == 1);
  CHECK(r.data.connections.size() == 1 && r.data.connections[0].name == "Ubuntu 开发机");
  CHECK(r.data.connections[0].endpoint->hyperv->vmId == "00000000-0000-0000-0000-000000000001");

  CHECK(saveConnections(f, r.data, false, &err) && r.data.revision == 2);
  CHECK(fs::exists(dir / "connections.json.bak"));
  CHECK(!fs::exists(dir / "connections.json.tmp"));

  // 非法 JSON → 从备份恢复
  write(f, "{ not json");
  r = loadConnections(f);
  CHECK(r.status == LoadStatus::RecoveredBackup && r.data.revision == 1);
  // 恢复后保存不能把损坏文件覆盖进备份
  CHECK(saveConnections(f, r.data, false, &err));
  CHECK(loadConnections(dir / "connections.json.bak").status == LoadStatus::Ok);

  // 主文件和备份都损坏 → Corrupt + 只读
  write(f, "x"); write(dir / "connections.json.bak", "y");
  r = loadConnections(f);
  CHECK(r.status == LoadStatus::Corrupt && r.readOnly);
  CHECK(!saveConnections(f, r.data, r.readOnly, &err));

  // 高版本 schema → 只读，文件不被改写
  write(f, R"({"schemaVersion": 99, "revision": 5})");
  r = loadConnections(f);
  CHECK(r.status == LoadStatus::NewerSchema && r.readOnly);
  CHECK(!saveConnections(f, r.data, r.readOnly, &err));
  { std::ifstream in(f); std::string s((std::istreambuf_iterator<char>(in)), {}); CHECK(s.find("99") != std::string::npos); }

  // 校验失败的数据不落盘
  ConnectionsFile bad; auto c = sample(); c.name = " "; bad.connections.push_back(c);
  fs::path f2 = dir / "c2.json";
  CHECK(!saveConnections(f2, bad, false, &err) && !fs::exists(f2));

  // 未知 configSource 拒绝
  write(f2, R"({"schemaVersion":1,"connections":[{"id":"a","name":"a","kind":"ssh","endpoint":{"mode":"manual","host":"h","port":22},"ssh":{"user":"u","configSource":"user","auth":{"mode":"agent"}}}]})");
  CHECK(loadConnections(f2).status == LoadStatus::Corrupt);

  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
