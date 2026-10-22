#include <cstdio>
#include "uicfg/model.h"
using namespace uicfg;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection ssh(std::string host, int port, std::string user) {
  Connection c; c.name = "Ubuntu 开发机"; c.kind = ConnectionKind::Ssh;
  c.endpoint = Endpoint{EndpointMode::Manual, host, port, std::nullopt};
  c.ssh = SshSettings{user}; return c;
}

int main() {
  CHECK(validate(ssh("192.168.1.10", 22, "dev")).empty());
  CHECK(validate(ssh("fe80::1", 22, "dev")).empty());           // IPv6
  CHECK(!validate(ssh("h", 0, "dev")).empty());
  CHECK(!validate(ssh("h", 65536, "dev")).empty());
  CHECK(!validate(ssh("-oProxyCommand=x", 22, "dev")).empty()); // 伪装选项
  CHECK(!validate(ssh("h\nx", 22, "dev")).empty());
  CHECK(!validate(ssh("h", 22, "dev; rm -rf /")).empty());
  auto blank = ssh("h", 22, "dev"); blank.name = "   ";
  CHECK(!validate(blank).empty());
  auto key = ssh("h", 22, "dev"); key.ssh->authMode = AuthMode::KeyFile;
  CHECK(!validate(key).empty());
  key.ssh->identityFile = "C:\\Users\\测试 用户\\.ssh\\id_ed25519";  // Unicode+空格路径
  CHECK(validate(key).empty());
  Connection loc; loc.name = "PowerShell"; loc.kind = ConnectionKind::Local;
  loc.local = LocalSettings{"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", {"-NoLogo"}, std::nullopt};
  CHECK(validate(loc).empty());
  loc.ssh = SshSettings{"x"};
  CHECK(!validate(loc).empty());
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
