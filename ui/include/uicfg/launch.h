// SPDX-License-Identifier: GPL-3.0-or-later
// DS-2：平台无关的启动描述；Win32 边界只做转换与接线。
#pragma once
#include "uicfg/store.h"
#include <utility>
#include <variant>

namespace uicfg {

using EnvVars = std::vector<std::pair<std::string, std::string>>;

struct LocalLaunchDesc {
  std::string executable;  // 空 = 默认 shell，仅空 connectionId
  std::vector<std::string> arguments;
  std::optional<std::string> workingDirectory;
  std::optional<EnvVars> environment;
};
struct SshLaunchDesc {
  std::string host;
  uint16_t port;
  std::string user;
  AuthMode auth;
  std::string identityFile;
  uint32_t connectTimeoutSec;
  std::optional<SshTransport> transport = std::nullopt;
};
struct HyperVLaunchDesc {
  SshLaunchDesc ssh;  // host 留空，由 Hyper-V 解析填入
  std::string vmId;
  std::optional<std::string> adapterId;
  std::string addressFamily;
  std::optional<std::string> fallbackHost;
  bool startIfOff = false;
};
struct LaunchError { std::string message; };
using LaunchDesc = std::variant<LocalLaunchDesc, SshLaunchDesc, HyperVLaunchDesc, LaunchError>;

struct LaunchContext {
  std::string userHome;
  EnvVars parentEnvironment;
};

LaunchDesc describeLaunch(const ConnectionsFile&, const std::string& connectionId, const LaunchContext&);

// nullopt = 原样继承；有值时按名称排序且非空（LaunchSpec 的空环境表示继承）。
std::optional<EnvVars> composeEnvironment(const LocalEnvironment&, const EnvVars& parent);

}  // namespace uicfg
