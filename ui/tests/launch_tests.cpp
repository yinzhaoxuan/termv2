#include <cstdio>
#include "uicfg/launch.h"
using namespace uicfg;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection local() {
  Connection c; c.id = "local"; c.name = "本地"; c.kind = ConnectionKind::Local;
  c.local = LocalSettings{"pwsh.exe", {"-NoLogo", "中文 空格"}, std::nullopt};
  return c;
}

static Connection ssh() {
  Connection c; c.id = "ssh"; c.name = "远程";
  c.endpoint = Endpoint{EndpointMode::Manual, "server", 65535, std::nullopt};
  c.ssh = SshSettings{"dev", AuthMode::KeyFile, "C:/keys/中文", 600};
  return c;
}

int main() {
  ConnectionsFile file;
  LaunchContext ctx{"C:/Users/测试", {{"Path", "parent"}}};
  auto desc = describeLaunch(file, "", ctx);
  CHECK(std::holds_alternative<LocalLaunchDesc>(desc));
  const auto& empty = std::get<LocalLaunchDesc>(desc);
  CHECK(empty.executable.empty() && empty.arguments.empty());
  CHECK(!empty.workingDirectory && !empty.environment);
  CHECK(std::get<LaunchError>(describeLaunch(file, "missing", ctx)).message == "连接配置不存在");

  file.connections = {local(), ssh()};
  desc = describeLaunch(file, "local", ctx);
  const auto& l = std::get<LocalLaunchDesc>(desc);
  CHECK(l.executable == "pwsh.exe" && l.arguments == file.connections[0].local->arguments);
  CHECK(l.workingDirectory == ctx.userHome && !l.environment);
  CHECK(!std::get<LocalLaunchDesc>(describeLaunch(file, "local", {})).workingDirectory);
  auto& settings = *file.connections[0].local;
  settings.workingDirectory = "D:/项目";
  settings.environment.set = {{"pAtH", "custom"}};
  desc = describeLaunch(file, "local", ctx);
  const auto& explicitLocal = std::get<LocalLaunchDesc>(desc);
  CHECK(explicitLocal.workingDirectory == "D:/项目");
  CHECK((explicitLocal.environment == EnvVars{{"pAtH", "custom"}}));

  desc = describeLaunch(file, "ssh", ctx);
  const auto s = std::get<SshLaunchDesc>(desc);
  CHECK(s.host == "server" && s.port == uint16_t{65535} && s.user == "dev");
  CHECK(s.auth == AuthMode::KeyFile && s.identityFile == "C:/keys/中文" && s.connectTimeoutSec == 600);
  auto& ep = *file.connections[1].endpoint;
  ep.mode = EndpointMode::HyperV; ep.host.reset();
  ep.hyperv = HyperVEndpoint{"local", "vm-id", "adapter-id", "ipv6", "fallback-host"};
  desc = describeLaunch(file, "ssh", ctx);
  CHECK(std::holds_alternative<HyperVLaunchDesc>(desc));
  const auto& h = std::get<HyperVLaunchDesc>(desc);
  CHECK(h.ssh.host.empty() && h.ssh.port == 65535 && h.ssh.user == "dev");
  CHECK(h.ssh.auth == s.auth && h.ssh.identityFile == "C:/keys/中文" && h.ssh.connectTimeoutSec == 600);
  CHECK(h.vmId == "vm-id" && h.adapterId == "adapter-id" && h.addressFamily == "ipv6");
  CHECK(h.fallbackHost == "fallback-host" && !h.startIfOff);

  for (int port : {0, 65536, 65558}) {
    ep.port = port;
    CHECK(std::get<LaunchError>(describeLaunch(file, "ssh", ctx)).message ==
          "连接配置无效（endpoint.port）：端口必须是 1–65535 的整数");
  }
  ep.port = 22;
  file.connections.push_back(file.connections[1]);
  CHECK(std::get<LaunchError>(describeLaunch(file, "ssh", ctx)).message == "连接配置无效（id）：连接 ID 重复");
  file.connections.pop_back();
  file.connections[0].groupId = "missing";
  CHECK(std::get<LaunchError>(describeLaunch(file, "local", ctx)).message == "连接配置无效（groupId）：分组不存在");
  file.connections[0].groupId.clear();
  file.connections[0].local->closePolicy = "futurePolicy";
  CHECK(std::get<LaunchError>(describeLaunch(file, "local", ctx)).message == "连接配置无效（local.closePolicy）：不支持的关闭策略");
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
