// SPDX-License-Identifier: GPL-3.0-or-later
// DS-13：按连接选择传输的存储、校验与启动描述。
#include "uicfg/launch.h"
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace uicfg;
using nlohmann::json;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection ssh(const std::string& id) {
  Connection c; c.id = id; c.name = "远程";
  c.endpoint = Endpoint{EndpointMode::Manual, "server", 22, std::nullopt};
  c.ssh = SshSettings{"dev"};
  return c;
}

static json read(const fs::path& file) {
  std::ifstream in(file);
  return json::parse(in);
}

static void write(const fs::path& file, const json& document) {
  std::ofstream(file, std::ios::binary) << document.dump();
}

static void store() {
  const auto dir = fs::temp_directory_path() / "uicfg_ssh_transport_test";
  fs::remove_all(dir); fs::create_directories(dir);
  const auto file = dir / "connections.json";
  ConnectionsFile data; data.connections = {ssh("target"), ssh("other")};
  std::string err;
  CHECK(saveConnections(file, data, false, &err));
  const auto base = read(file);
  auto loaded = loadConnections(file);
  CHECK(loaded.status == LoadStatus::Ok && loaded.validation.ok());
  CHECK(!loaded.data.connections.at(0).ssh->transport);
  CHECK(saveConnections(file, loaded.data, false, &err));
  CHECK(!read(file)["connections"][0]["ssh"].contains("transport"));

  for (const auto& value : {json("compatible"), json("direct")}) {
    auto document = base;
    document["connections"][0]["ssh"]["transport"] = value;
    write(file, document);
    loaded = loadConnections(file);
    CHECK(loaded.status == LoadStatus::Ok && loaded.validation.ok());
    CHECK(loaded.data.connections.at(0).ssh->transport == value.get<std::string>());
    CHECK(saveConnections(file, loaded.data, false, &err));
    CHECK(read(file)["schemaVersion"] == kSchemaVersion);
    CHECK(read(file)["connections"][0]["ssh"]["transport"] == value);
    CHECK(!read(file)["connections"][1]["ssh"].contains("transport"));
    const auto again = loadConnections(file);
    CHECK(again.status == LoadStatus::Ok && again.validation.ok());
    CHECK(again.data.connections.at(0).ssh->transport == value.get<std::string>());
  }

  // 非法值只影响该连接的校验，但与其它非法字段一样阻止保存整个文件。
  for (const auto& value : {json("Direct"), json("future"), json(""), json(7), json(nullptr),
                            json{{"mode", "direct"}}, json::array({"direct"}), json(true)}) {
    auto document = base;
    document["connections"][0]["ssh"]["transport"] = value;
    write(file, document);
    loaded = loadConnections(file);
    CHECK(loaded.status == LoadStatus::Ok && !loaded.readOnly);
    CHECK(loaded.validation.fileErrors.empty());
    CHECK(loaded.validation.byConnection.size() == 1);
    CHECK(!loaded.validation.byConnection.count("other"));
    CHECK(validate(loaded.data.connections.at(1)).empty());
    const auto& settings = *loaded.data.connections.at(0).ssh;
    CHECK(settings.transport == (value.is_string() ? value.get<std::string>() : value.dump()));
    CHECK(!loaded.validation.ok());
    const auto& errors = loaded.validation.byConnection.at("target");
    CHECK(errors.size() == 1 && errors[0].field == "ssh.transport");
    const auto revision = loaded.data.revision;
    CHECK(!saveConnections(file, loaded.data, false, &err));
    CHECK(err == "连接 target 校验失败: 传输只能是 compatible 或 direct");
    CHECK(loaded.data.revision == revision);
    CHECK(read(file) == document);
  }
  // 多个字段非法时，仍返回首个校验错误。
  loaded.data.connections[0].endpoint->port = 0;
  const auto before = read(file);
  CHECK(!saveConnections(file, loaded.data, false, &err));
  CHECK(err == "连接 target 校验失败: 端口必须是 1–65535 的整数");
  CHECK(read(file) == before);
  fs::remove_all(dir);
}

static void checkError(const Connection& c, const std::string& field, const std::string& message) {
  const auto errors = validate(c);
  CHECK(errors.size() == 1);
  if (errors.size() != 1) return;
  CHECK(errors[0].field == field && errors[0].message == message);
  ConnectionsFile file; file.connections = {c, ssh("other")};
  const auto validation = validateConnectionsFile(file);
  CHECK(!validation.ok() && validation.fileErrors.empty() && validation.byConnection.size() == 1);
  CHECK(validation.byConnection.count(c.id) == 1);
  const auto report = reportValidation(file, validation);
  CHECK(report.statusNote == "connections.json 有 1 条连接配置无效，已禁止启动");
  CHECK((report.details == std::vector<std::string>{
      "connections.json 连接 " + c.id + "（" + field + "）：" + message}));
  const auto launch = describeLaunch(file, c.id, {});
  const auto* blocked = std::get_if<LaunchError>(&launch);
  CHECK(blocked && blocked->message == "连接配置无效（" + field + "）：" + message);
}

static void validation() {
  auto c = ssh("target");
  CHECK(!parseSshTransport(*c.ssh));
  for (const char* value : {"compatible", "direct"}) {
    c.ssh->transport = value;
    CHECK(validate(c).empty());
    CHECK(parseSshTransport(*c.ssh) == (std::string(value) == "direct"
          ? SshTransport::Direct : SshTransport::Compatible));
  }
  for (const char* value : {"Direct", "future", "", "7", "null", "{}", "[]", "true"}) {
    c.ssh->transport = value;
    CHECK(!parseSshTransport(*c.ssh));
    checkError(c, "ssh.transport", "传输只能是 compatible 或 direct");
  }
  for (auto auth : {AuthMode::KeyFile, AuthMode::Interactive}) {
    c.ssh->authMode = auth; c.ssh->identityFile = "C:/keys/id_ed25519";
    c.ssh->transport = "direct";
    checkError(c, "ssh.auth.mode", "直连只支持 agent 认证；改用 agent，或把 ssh.transport 改为 compatible");
    c.ssh->transport = "compatible";
    CHECK(validate(c).empty());
    c.ssh->transport.reset();
    CHECK(validate(c).empty());
  }
}

static void launch() {
  ConnectionsFile file; file.connections = {ssh("target")};
  auto& c = file.connections[0];
  for (bool hyperV : {false, true}) {
    c.endpoint->mode = hyperV ? EndpointMode::HyperV : EndpointMode::Manual;
    c.endpoint->host = hyperV ? std::nullopt : std::optional<std::string>("server");
    c.endpoint->hyperv = hyperV ? std::optional<HyperVEndpoint>(HyperVEndpoint{"local", "vm-id"})
                              : std::nullopt;
    for (const auto& transport : {std::optional<std::string>{},
                                  std::optional<std::string>{"compatible"},
                                  std::optional<std::string>{"direct"}}) {
      c.ssh->transport = transport;
      const auto desc = describeLaunch(file, c.id, {});
      const auto* hv = std::get_if<HyperVLaunchDesc>(&desc);
      const auto* result = hyperV ? (hv ? &hv->ssh : nullptr) : std::get_if<SshLaunchDesc>(&desc);
      CHECK(result);
      if (!result) continue;
      CHECK(result->transport == parseSshTransport(*c.ssh));
      CHECK(result->host == (hyperV ? "" : "server"));
      CHECK(result->user == "dev" && result->port == 22);
    }
    c.ssh->transport = "Direct";
    checkError(c, "ssh.transport", "传输只能是 compatible 或 direct");
    c.ssh->transport = "direct"; c.ssh->authMode = AuthMode::Interactive;
    checkError(c, "ssh.auth.mode", "直连只支持 agent 认证；改用 agent，或把 ssh.transport 改为 compatible");
    c.ssh->authMode = AuthMode::Agent;
  }
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "store" || mode == "all") store();
  if (mode == "validate" || mode == "all") validation();
  if (mode == "launch" || mode == "all") launch();
  return fails != 0;
}
