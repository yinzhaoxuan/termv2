#include <cstdio>
#include <fstream>
#include "uicfg/store.h"
#include <nlohmann/json.hpp>
using namespace uicfg;
using nlohmann::json;
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

static int countField(const std::vector<ValidationError>& errors, const std::string& field) {
  int count = 0;
  for (const auto& error : errors) if (error.field == field) ++count;
  return count;
}

static void localRoundTrip(const fs::path& file) {
  Connection c; c.id = "local"; c.name = "本地"; c.kind = ConnectionKind::Local;
  c.local = LocalSettings{"pwsh.exe", {"-NoLogo", "中文 参数"}, "C:/项目"};
  auto& local = *c.local;
  local.environment.inherit = false;
  local.environment.set = {{"TEST", "中文 值"}, {"Path", "C:/程序"}, {"EMPTY", ""}};
  local.environment.unset = {"TEMP", "OTHER"};
  ConnectionsFile data; data.connections.push_back(c);
  std::string err;
  CHECK(saveConnections(file, data, false, &err));
  auto loaded = loadConnections(file);
  CHECK(loaded.status == LoadStatus::Ok && loaded.validation.ok());
  const auto& result = *loaded.data.connections.at(0).local;
  CHECK(result.executable == local.executable && result.arguments == local.arguments);
  CHECK(result.workingDirectory == local.workingDirectory);
  CHECK(result.environment.inherit == local.environment.inherit);
  CHECK(result.environment.set == local.environment.set && result.environment.unset == local.environment.unset);
  CHECK(result.closePolicy == local.closePolicy);
  CHECK(saveConnections(file, loaded.data, false, &err));
  json document; { std::ifstream in(file); in >> document; }
  CHECK(document["connections"][0]["local"]["environment"]["set"]["TEST"] == "中文 值");
  auto& stored = document["connections"][0]["local"];
  stored["closePolicy"] = "futurePolicy";
  write(file, document.dump().c_str());
  loaded = loadConnections(file);
  CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly);
  CHECK(loaded.data.connections.at(0).local->closePolicy == "futurePolicy");
  CHECK(countField(loaded.validation.byConnection.at("local"), "local.closePolicy") == 1);
  // 未知策略保留原值；校验拒绝保存，不能把它静默改成默认值。
  CHECK(!saveConnections(file, loaded.data, false, &err));
  { std::ifstream in(file); CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == document.dump()); }
  stored.erase("environment"); stored.erase("closePolicy");
  write(file, document.dump().c_str());
  loaded = loadConnections(file);
  CHECK(loaded.status == LoadStatus::Ok && loaded.validation.ok());
  const auto defaults = *loaded.data.connections.at(0).local;
  CHECK(defaults.environment.inherit && defaults.environment.set.empty() && defaults.environment.unset.empty());
  CHECK(defaults.closePolicy == "terminateSessionTree");
  CHECK(saveConnections(file, loaded.data, false, &err));
  { std::ifstream in(file); in >> document; }
  CHECK(document["connections"][0]["local"]["environment"]["set"].is_object());
  CHECK(document["connections"][0]["local"]["environment"]["unset"].is_array());
}

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
  CHECK(r.validation.ok() && !r.readOnly);
  CHECK(r.data.connections.size() == 1 && r.data.connections[0].name == "Ubuntu 开发机");
  CHECK(r.data.connections[0].endpoint->hyperv->vmId == "00000000-0000-0000-0000-000000000001");

  CHECK(saveConnections(f, r.data, false, &err) && r.data.revision == 2);
  CHECK(fs::exists(dir / "connections.json.bak"));
  CHECK(!fs::exists(dir / "connections.json.tmp"));

  // 非法 JSON → 从备份恢复
  write(f, "{ not json");
  r = loadConnections(f);
  CHECK(r.status == LoadStatus::RecoveredBackup && r.data.revision == 1);
  CHECK(r.validation.ok() && !r.readOnly);
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

  const auto base = json::parse(R"({"schemaVersion":1,"connections":[
    {"id":"target","name":"待校验","kind":"ssh","endpoint":{"mode":"manual","host":"h","port":22},
     "ssh":{"user":"u","auth":{"mode":"agent"},"connectTimeoutSeconds":15}},
    {"id":"other","name":"正常连接","kind":"ssh","endpoint":{"mode":"manual","host":"other"},
     "ssh":{"user":"other-user","auth":{"mode":"agent"}}}]})");
  auto load = [&](const json& document) {
    write(f2, document.dump().c_str());
    return loadConnections(f2);
  };
  auto checkOther = [&](const LoadResult& loaded) {
    CHECK(loaded.data.connections.size() == 2);
    CHECK(loaded.validation.byConnection.count("other") == 0);
    const auto& other = loaded.data.connections.at(1);
    CHECK(other.id == "other" && other.name == "正常连接");
    CHECK(other.endpoint->host == "other" && other.endpoint->port == 22);
    CHECK(other.ssh->user == "other-user" && other.ssh->connectTimeoutSeconds == 15);
    CHECK(validate(other).empty());
  };
  // 非法类型/越界数字不能抛异常或截断，也不能影响其它连接。
  struct PortCase { const char* value; int expected; };
  for (const auto& test : {PortCase{"0", 0}, {"65536", 65536}, {"65558", 65558},
                          {"\"22\"", 0}, {"22.5", 0}, {"true", 0}, {"1e10", 0},
                          {"2147483648", 0}, {"-2147483649", 0},
                          {"18446744073709551615", 0}, {"18446744073709551616", 0},
                          {"22.0", 0}, {"null", 0}, {"{}", 0}, {"[]", 0}}) {
    auto document = base;
    document["connections"][0]["endpoint"]["port"] = json::parse(test.value);
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(loaded.validation.fileErrors.empty() && loaded.validation.byConnection.size() == 1);
    CHECK(loaded.data.connections.at(0).endpoint->port == test.expected);
    CHECK(countField(loaded.validation.byConnection.at("target"), "endpoint.port") == 1);
    CHECK(countField(validate(loaded.data.connections.at(0)), "endpoint.port") == 1);
    checkOther(loaded);
    CHECK(!saveConnections(f2, loaded.data, false, &err));
    std::ifstream in(f2, std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == document.dump());
  }
  for (const char* value : {"0", "601", "\"15\"", "15.5", "true", "1e10", "2147483648",
                            "-2147483649", "18446744073709551615", "15.0", "null"}) {
    auto document = base;
    document["connections"][0]["ssh"]["connectTimeoutSeconds"] = json::parse(value);
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(countField(loaded.validation.byConnection.at("target"), "ssh.connectTimeoutSeconds") == 1);
    CHECK(countField(validate(loaded.data.connections.at(0)), "ssh.connectTimeoutSeconds") == 1);
    checkOther(loaded);
    CHECK(!saveConnections(f2, loaded.data, false, &err));
  }
  for (int port : {1, 65535}) for (int timeout : {1, 600}) {
    auto document = base;
    document["connections"][0]["endpoint"]["port"] = port;
    document["connections"][0]["ssh"]["connectTimeoutSeconds"] = timeout;
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && loaded.validation.ok());
    CHECK(loaded.data.connections.at(0).endpoint->port == port);
    CHECK(loaded.data.connections.at(0).ssh->connectTimeoutSeconds == timeout);
  }
  {
    auto document = base;
    document["connections"][0]["endpoint"].erase("port");
    document["connections"][0]["ssh"].erase("connectTimeoutSeconds");
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && loaded.validation.ok());
    CHECK(loaded.data.connections.at(0).endpoint->port == 22);
    CHECK(loaded.data.connections.at(0).ssh->connectTimeoutSeconds == 15);
  }
  {
    auto document = base; document["connections"][1]["id"] = "target";
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(loaded.validation.fileErrors.empty() && loaded.validation.byConnection.size() == 1);
    CHECK(countField(loaded.validation.byConnection.at("target"), "id") == 2);
  }
  {
    auto document = base; document["connections"][0]["groupId"] = "missing";
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(countField(loaded.validation.byConnection.at("target"), "groupId") == 1);
    checkOther(loaded);
    document["groups"] = json::array({{{"id", "missing"}, {"name", "开发"}}});
    CHECK(load(document).validation.ok());
  }
  {
    auto document = base;
    document["groups"] = json::array({{{"id", "dev"}, {"name", "开发"}}, {{"id", "dev"}, {"name", "重复"}}});
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(loaded.validation.byConnection.empty());
    CHECK(countField(loaded.validation.fileErrors, "groups.id") == 1);
  }
  {
    auto document = base; document["connections"][0]["id"] = "";
    auto loaded = load(document);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly && !loaded.validation.ok());
    CHECK(countField(loaded.validation.byConnection.at(""), "id") == 1);
    checkOther(loaded);
  }
  {
    // 备份里的非法字段也要校验，但恢复状态和可编辑性保持原样。
    auto document = base; document["connections"][0]["endpoint"]["port"] = "22";
    write(dir / "c2.json.bak", document.dump().c_str());
    write(f2, "{ not json");
    auto loaded = loadConnections(f2);
    CHECK(loaded.status == LoadStatus::RecoveredBackup && !loaded.readOnly && !loaded.validation.ok());
    CHECK(countField(loaded.validation.byConnection.at("target"), "endpoint.port") == 1);
    checkOther(loaded);
  }

  localRoundTrip(dir / "local.json");
  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
