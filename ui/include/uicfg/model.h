// 第 13 节配置模型（connections.json）
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uicfg {

enum class AuthMode { Agent, KeyFile, Interactive };
enum class SshTransport { Compatible, Direct };
enum class EndpointMode { Manual, HyperV };
enum class ConnectionKind { Ssh, Local };

struct HyperVEndpoint {
  std::string hostScope = "local";
  std::string vmId;
  std::optional<std::string> adapterId;
  std::string addressFamily = "ipv4";
  std::optional<std::string> fallbackHost;
};

struct Endpoint {
  EndpointMode mode = EndpointMode::Manual;
  std::optional<std::string> host;   // manual 必填；hyperv 不存解析缓存
  int port = 22;
  std::optional<HyperVEndpoint> hyperv;
};

struct SshSettings {
  std::string user;
  AuthMode authMode = AuthMode::Agent;
  std::optional<std::string> identityFile;  // keyFile 必填
  int connectTimeoutSeconds = 15;
  // ssh.transport 原文：nullopt = 跟随全局默认；非字符串存其 JSON 文本（必然非法）。
  std::optional<std::string> transport = std::nullopt;
};

// compatible / direct → 枚举；缺省、未知或非字符串 → nullopt，调用前应已校验。
std::optional<SshTransport> parseSshTransport(const SshSettings&);

struct LocalEnvironment {
  bool inherit = true;
  std::map<std::string, std::string> set;
  std::vector<std::string> unset;
};

struct LocalSettings {
  std::string executable;
  std::vector<std::string> arguments;
  std::optional<std::string> workingDirectory;
  LocalEnvironment environment;
  std::string closePolicy = "terminateSessionTree";
};

struct Connection {
  std::string id, name, groupId, appearanceProfileId;
  int order = 0;
  ConnectionKind kind = ConnectionKind::Ssh;
  std::optional<Endpoint> endpoint;  // 仅 ssh
  std::optional<SshSettings> ssh;    // 仅 ssh
  std::optional<LocalSettings> local;  // 仅 local
};

struct ValidationError {
  std::string field;
  std::string message;
};

// 12.2 连接编辑器校验；返回空表示通过
std::vector<ValidationError> validate(const Connection& c);

}  // namespace uicfg
