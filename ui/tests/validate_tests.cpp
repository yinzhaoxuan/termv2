// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include "uicfg/store.h"
using namespace uicfg;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection ssh(std::string host, int port, std::string user) {
  Connection c; c.name = "Ubuntu 开发机"; c.kind = ConnectionKind::Ssh;
  c.endpoint = Endpoint{EndpointMode::Manual, host, port, std::nullopt};
  c.ssh = SshSettings{user}; return c;
}

static int countField(const std::vector<ValidationError>& errors, const std::string& field) {
  int count = 0;
  for (const auto& error : errors) if (error.field == field) ++count;
  return count;
}

int main() {
  CHECK(validate(ssh("192.168.1.10", 22, "dev")).empty());
  CHECK(validate(ssh("fe80::1", 22, "dev")).empty());           // IPv6
  CHECK(!validate(ssh("h", 0, "dev")).empty());
  CHECK(!validate(ssh("h", 65536, "dev")).empty());
  for (int port : {0, 65536, 65558})
    CHECK(countField(validate(ssh("h", port, "dev")), "endpoint.port") == 1);
  for (int port : {1, 65535}) CHECK(validate(ssh("h", port, "dev")).empty());
  for (int timeout : {0, 601}) {
    auto c = ssh("h", 22, "dev"); c.ssh->connectTimeoutSeconds = timeout;
    CHECK(countField(validate(c), "ssh.connectTimeoutSeconds") == 1);
  }
  for (int timeout : {1, 600}) {
    auto c = ssh("h", 22, "dev"); c.ssh->connectTimeoutSeconds = timeout;
    CHECK(validate(c).empty());
  }
  CHECK(!validate(ssh("-oProxyCommand=x", 22, "dev")).empty()); // 伪装选项
  CHECK(!validate(ssh("h\nx", 22, "dev")).empty());
  CHECK(!validate(ssh("h", 22, "dev; rm -rf /")).empty());
  auto blank = ssh("h", 22, "dev"); blank.name = "   ";
  CHECK(!validate(blank).empty());
  auto key = ssh("h", 22, "dev"); key.ssh->authMode = AuthMode::KeyFile;
  CHECK(!validate(key).empty());
  key.ssh->identityFile = "C:\\Users\\测试 用户\\.ssh\\id_ed25519";  // Unicode+空格路径
  CHECK(validate(key).empty());
  for (const char* bad : {"C:\\k\\a\"b", "C:\\k\\%h", "~/.ssh/id", "C:\\it's"}) {
    auto k2 = key; k2.ssh->identityFile = bad; CHECK(!validate(k2).empty());
  }
  Connection loc; loc.name = "PowerShell"; loc.kind = ConnectionKind::Local;
  loc.local = LocalSettings{"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", {"-NoLogo"}, std::nullopt};
  CHECK(validate(loc).empty());
  for (const auto& bad : {std::string("a\0b", 3), std::string("a\rb"), std::string("a\nb")}) {
    auto invalid = loc; invalid.local->executable = bad;
    CHECK(countField(validate(invalid), "local.executable") == 1);
    invalid = loc; invalid.local->workingDirectory = bad;
    CHECK(countField(validate(invalid), "local.workingDirectory") == 1);
  }
  {
    auto invalid = loc; invalid.local->executable.clear();
    CHECK(countField(validate(invalid), "local.executable") == 1);
    invalid = loc; invalid.local->arguments = {std::string("a\0b", 3)};
    CHECK(countField(validate(invalid), "local.arguments") == 1);
  }
  for (const auto& name : {std::string(), std::string("A=B"), std::string("A\0B", 3)}) {
    auto invalid = loc; invalid.local->environment.set = {{name, "value"}};
    CHECK(countField(validate(invalid), "local.environment.set") == 1);
    invalid = loc; invalid.local->environment.unset = {name};
    CHECK(countField(validate(invalid), "local.environment.unset") == 1);
  }
  {
    auto invalid = loc; invalid.local->environment.set = {{"A", std::string("a\0b", 3)}};
    CHECK(countField(validate(invalid), "local.environment.set") == 1);
    invalid = loc; invalid.local->environment.set = {{"Path", "a"}, {"PATH", "b"}};
    CHECK(countField(validate(invalid), "local.environment.set") == 1);
    invalid = loc; invalid.local->environment.set = {{"Path", "a"}};
    invalid.local->environment.unset = {"pATH"};
    CHECK(countField(validate(invalid), "local.environment.unset") == 1);
    invalid = loc; invalid.local->closePolicy = "futurePolicy";
    const auto errors = validate(invalid);
    CHECK(errors.size() == 1 && errors[0].field == "local.closePolicy" && errors[0].message == "不支持的关闭策略");
  }
  {
    auto valid = loc; valid.local->arguments = {"多行\r\n参数"};
    valid.local->workingDirectory = "C:/中文 路径";
    valid.local->environment.set = {{"Path", ""}, {"VALUE", "多行\r\n值"}};
    valid.local->environment.unset = {"OTHER", "other"};
    CHECK(validate(valid).empty());
  }
  loc.ssh = SshSettings{"x"};
  CHECK(!validate(loc).empty());

  ConnectionsFile file;
  file.groups.push_back({"dev", "开发", 0});
  auto c = ssh("h", 22, "dev"); c.id = "a"; c.groupId = "dev";
  file.connections.push_back(c);
  CHECK(validateConnectionsFile(file).ok());
  CHECK(validateInFile(file, file.connections[0]).empty());
  CHECK(launchBlockedMessage({}).empty());
  CHECK(launchBlockedMessage({{"id", "连接 ID 不能为空"}, {"groupId", "分组不存在"}}) ==
        "连接配置无效（id）：连接 ID 不能为空");
  {
    const auto report = reportValidation(file, validateConnectionsFile(file));
    CHECK(report.statusNote.empty() && report.details.empty());
  }
  {
    // 单条连接的字段和文件上下文错误都返回，首条错误仍按原顺序报告。
    auto invalid = file;
    auto& target = invalid.connections[0];
    target.id.clear(); target.groupId = "missing"; target.endpoint->port = 0;
    auto errors = validateInFile(invalid, target);
    CHECK(errors.size() == 3);
    CHECK(countField(errors, "endpoint.port") == 1);
    CHECK(countField(errors, "id") == 1);
    CHECK(countField(errors, "groupId") == 1);
    CHECK(launchBlockedMessage(errors) == "连接配置无效（endpoint.port）：端口必须是 1–65535 的整数");
    const auto report = reportValidation(invalid, validateConnectionsFile(invalid));
    CHECK(report.statusNote == "connections.json 有 1 条连接配置无效，已禁止启动");
    CHECK((report.details == std::vector<std::string>{
      "connections.json 连接 （endpoint.port）：端口必须是 1–65535 的整数",
      "connections.json 连接 （id）：连接 ID 不能为空",
      "connections.json 连接 （groupId）：分组不存在"}));
  }
  {
    // 重复 ID 的每条连接都记 id 错误，并保留各自的字段错误。
    auto duplicate = file;
    duplicate.connections[0].endpoint->port = 65558;
    c.ssh->connectTimeoutSeconds = 601;
    duplicate.connections.push_back(c);
    auto validation = validateConnectionsFile(duplicate);
    CHECK(!validation.ok() && validation.fileErrors.empty());
    CHECK(validation.byConnection.size() == 1);
    CHECK(countField(validation.byConnection.at("a"), "id") == 2);
    CHECK(countField(validation.byConnection.at("a"), "endpoint.port") == 1);
    CHECK(countField(validation.byConnection.at("a"), "ssh.connectTimeoutSeconds") == 1);
    const auto first = validateInFile(duplicate, duplicate.connections[0]);
    const auto second = validateInFile(duplicate, duplicate.connections[1]);
    CHECK(first.size() == 2 && second.size() == 2);
    CHECK(countField(first, "id") == 1 && countField(second, "id") == 1);
    CHECK(countField(first, "endpoint.port") == 1);
    CHECK(countField(first, "ssh.connectTimeoutSeconds") == 0);
    CHECK(countField(second, "ssh.connectTimeoutSeconds") == 1);
    CHECK(countField(second, "endpoint.port") == 0);
    const auto report = reportValidation(duplicate, validation);
    CHECK(report.statusNote == "connections.json 有 2 条连接配置无效，已禁止启动");
    CHECK((report.details == std::vector<std::string>{
      "connections.json 连接 a（endpoint.port）：端口必须是 1–65535 的整数",
      "connections.json 连接 a（id）：连接 ID 重复",
      "connections.json 连接 a（ssh.connectTimeoutSeconds）：连接超时必须是 1–600 秒的整数",
      "connections.json 连接 a（id）：连接 ID 重复"}));
  }
  {
    auto unknown = file; unknown.connections[0].groupId = "missing";
    auto validation = validateConnectionsFile(unknown);
    CHECK(!validation.ok() && validation.fileErrors.empty());
    CHECK(countField(validation.byConnection.at("a"), "groupId") == 1);
    CHECK(launchBlockedMessage(validateInFile(unknown, unknown.connections[0])) ==
          "连接配置无效（groupId）：分组不存在");
    unknown.connections[0].groupId.clear();  // 未分组合法
    CHECK(validateConnectionsFile(unknown).ok());
    CHECK(validateInFile(unknown, unknown.connections[0]).empty());
  }
  {
    auto duplicate = file; duplicate.groups.push_back(file.groups[0]);
    auto validation = validateConnectionsFile(duplicate);
    CHECK(!validation.ok() && validation.byConnection.empty());
    CHECK(countField(validation.fileErrors, "groups.id") == 1);
    const auto report = reportValidation(duplicate, validation);
    CHECK(report.statusNote == "connections.json 有 0 条连接配置无效，已禁止启动；文件级配置错误 1 项");
    CHECK((report.details == std::vector<std::string>{"connections.json（groups.id）：组 ID 重复：dev"}));
    duplicate.connections[0].groupId = "missing";
    const auto combined = reportValidation(duplicate, validateConnectionsFile(duplicate));
    CHECK(combined.statusNote == "connections.json 有 1 条连接配置无效，已禁止启动；文件级配置错误 1 项");
    CHECK((combined.details == std::vector<std::string>{
      "connections.json（groups.id）：组 ID 重复：dev",
      "connections.json 连接 a（groupId）：分组不存在"}));
  }
  {
    auto emptyId = file; emptyId.connections[0].id.clear();
    auto validation = validateConnectionsFile(emptyId);
    CHECK(!validation.ok());
    CHECK(countField(validation.byConnection.at(""), "id") == 1);
    CHECK(launchBlockedMessage(validateInFile(emptyId, emptyId.connections[0])) ==
          "连接配置无效（id）：连接 ID 不能为空");
  }
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
