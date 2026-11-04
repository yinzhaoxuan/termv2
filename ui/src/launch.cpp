#include "uicfg/launch.h"
#include "environment_names.h"
#include <algorithm>
#include <array>

namespace uicfg {

std::optional<EnvVars> composeEnvironment(const LocalEnvironment& env, const EnvVars& parent) {
  if (env.inherit && env.set.empty() && env.unset.empty()) return std::nullopt;
  // 不继承父环境时仍保留 Windows 的基本系统路径。
  constexpr std::array kAlwaysKept = {"systemroot", "systemdrive", "windir"};
  std::map<std::string, std::pair<std::string, std::string>> values;
  std::string systemRoot = "C:\\Windows";
  for (const auto& entry : parent) {
    auto name = detail::foldEnvironmentName(entry.first);
    if (name == "systemroot") systemRoot = entry.second;
    if (env.inherit || std::find(kAlwaysKept.begin(), kAlwaysKept.end(), name) != kAlwaysKept.end())
      values[name] = entry;
  }
  for (const auto& name : env.unset) values.erase(detail::foldEnvironmentName(name));
  for (const auto& entry : env.set) values[detail::foldEnvironmentName(entry.first)] = entry;
  // LaunchSpec 将空环境视为继承，结果必须非空。
  if (values.empty()) values["systemroot"] = {"SystemRoot", systemRoot};
  EnvVars result;
  for (const auto& [name, entry] : values) result.push_back(entry);
  return result;
}

LaunchDesc describeLaunch(const ConnectionsFile& file, const std::string& id, const LaunchContext& ctx) {
  if (id.empty()) return LocalLaunchDesc{};
  auto it = std::find_if(file.connections.begin(), file.connections.end(),
                         [&](const Connection& c) { return c.id == id; });
  if (it == file.connections.end()) return LaunchError{"连接配置不存在"};
  auto errors = validateInFile(file, *it);
  if (!errors.empty()) return LaunchError{launchBlockedMessage(errors)};
  const auto& c = *it;
  if (c.kind == ConnectionKind::Local) {
    const auto& local = *c.local;
    auto cwd = local.workingDirectory;
    if (!cwd && !ctx.userHome.empty()) cwd = ctx.userHome;
    return LocalLaunchDesc{local.executable, local.arguments, cwd,
                           composeEnvironment(local.environment, ctx.parentEnvironment)};
  }
  const auto& ep = *c.endpoint;
  const auto& settings = *c.ssh;
  SshLaunchDesc ssh{ep.host.value_or(""), static_cast<uint16_t>(ep.port), settings.user,
                    settings.authMode, settings.identityFile.value_or(""),
                    static_cast<uint32_t>(settings.connectTimeoutSeconds), parseSshTransport(settings)};
  if (ep.mode == EndpointMode::Manual) return ssh;
  ssh.host.clear();
  const auto& hv = *ep.hyperv;
  return HyperVLaunchDesc{ssh, hv.vmId, hv.adapterId, hv.addressFamily, hv.fallbackHost};
}

}  // namespace uicfg
