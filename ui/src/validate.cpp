#include "uicfg/store.h"
#include "environment_names.h"
#include <algorithm>
#include <set>

namespace uicfg {
namespace {

std::string trim(const std::string& s) {
  auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool hasControl(const std::string& s) {
  for (unsigned char ch : s)
    if (ch < 0x20 || ch == 0x7f) return true;
  return false;
}

bool hasNul(const std::string& s) { return s.find('\0') != std::string::npos; }

bool validEnvironmentName(const std::string& name) {
  return !name.empty() && !hasNul(name) && name.find('=') == std::string::npos;
}

void checkPath(const char* field, const std::string& value, std::vector<ValidationError>& errs) {
  if (hasNul(value)) errs.push_back({field, "路径不能含 NUL"});
  else if (value.find_first_of("\r\n") != std::string::npos)
    errs.push_back({field, "路径不能含 CR/LF"});
}

void validateLocal(const LocalSettings& local, std::vector<ValidationError>& errs) {
  auto err = [&](const char* f, const char* m) { errs.push_back({f, m}); };
  if (local.executable.empty()) err("local.executable", "必须选择可执行文件");
  else checkPath("local.executable", local.executable, errs);
  for (const auto& a : local.arguments)
    if (hasNul(a)) { err("local.arguments", "参数不能含 NUL"); break; }
  if (local.workingDirectory) checkPath("local.workingDirectory", *local.workingDirectory, errs);
  std::set<std::string> names;
  for (const auto& [name, value] : local.environment.set) {
    if (!validEnvironmentName(name)) err("local.environment.set", "环境变量名不能为空，不能含 = 或 NUL");
    if (hasNul(value)) err("local.environment.set", "环境变量值不能含 NUL");
    if (!names.insert(detail::foldEnvironmentName(name)).second)
      err("local.environment.set", "环境变量名不能重复（不区分大小写）");
  }
  for (const auto& name : local.environment.unset) {
    if (!validEnvironmentName(name)) err("local.environment.unset", "环境变量名不能为空，不能含 = 或 NUL");
    if (names.count(detail::foldEnvironmentName(name)))
      err("local.environment.unset", "环境变量不能同时出现在 set 和 unset 中");
  }
  if (local.closePolicy != "terminateSessionTree") err("local.closePolicy", "不支持的关闭策略");
}

}  // namespace

std::optional<SshTransport> parseSshTransport(const SshSettings& ssh) {
  if (!ssh.transport) return std::nullopt;
  if (*ssh.transport == "compatible") return SshTransport::Compatible;
  if (*ssh.transport == "direct") return SshTransport::Direct;
  return std::nullopt;
}

std::vector<ValidationError> validate(const Connection& c) {
  std::vector<ValidationError> errs;
  auto err = [&](const char* f, const char* m) { errs.push_back({f, m}); };

  if (trim(c.name).empty()) err("name", "名称不能为空");

  if (c.kind == ConnectionKind::Ssh) {
    if (c.local) err("local", "SSH 连接不能包含 local 字段");
    if (!c.endpoint || !c.ssh) {
      err("endpoint", "SSH 连接必须有 endpoint 和 ssh");
      return errs;
    }
    const auto& ep = *c.endpoint;
    if (ep.port < 1 || ep.port > 65535) err("endpoint.port", "端口必须是 1–65535 的整数");
    if (ep.mode == EndpointMode::Manual) {
      if (!ep.host || trim(*ep.host).empty()) err("endpoint.host", "手动模式必须填写主机");
      else if (hasControl(*ep.host)) err("endpoint.host", "主机不能含控制字符");
      else if ((*ep.host)[0] == '-') err("endpoint.host", "主机不能以 - 开头（会被当成选项）");
      if (ep.hyperv) err("endpoint.hyperv", "手动模式下 hyperv 必须为空");
    } else {
      if (!ep.hyperv || ep.hyperv->vmId.empty()) err("endpoint.hyperv.vmId", "Hyper-V 模式必须选择虚拟机");
      if (ep.host) err("endpoint.host", "Hyper-V 模式不保存地址");
    }
    const auto& ssh = *c.ssh;
    const auto transport = parseSshTransport(ssh);
    if (ssh.transport && !transport)
      err("ssh.transport", "传输只能是 compatible 或 direct");
    if (transport == SshTransport::Direct && ssh.authMode != AuthMode::Agent)
      err("ssh.auth.mode", "直连只支持 agent 认证；改用 agent，或把 ssh.transport 改为 compatible");
    if (ssh.connectTimeoutSeconds < 1 || ssh.connectTimeoutSeconds > 600)
      err("ssh.connectTimeoutSeconds", "连接超时必须是 1–600 秒的整数");
    const std::string u = trim(ssh.user);
    if (u.empty()) err("ssh.user", "用户名不能为空");
    else if (hasControl(u) || u[0] == '-' ||
             u.find_first_of(" @;|&`$") != std::string::npos)
      err("ssh.user", "用户名不能包含空格、@ 或命令字符");
    if (ssh.authMode == AuthMode::KeyFile) {
      if (!ssh.identityFile || ssh.identityFile->empty()) err("ssh.auth.identityFile", "密钥文件模式必须选择路径");
      else if (hasNul(*ssh.identityFile)) err("ssh.auth.identityFile", "路径不能含 NUL");
      // 与 ssh -F 快照生成器保持一致：引号、% 与 ~ 会触发 OpenSSH token 展开，编辑器提前拒绝
      else if (ssh.identityFile->find_first_of("\"'%") != std::string::npos ||
               ssh.identityFile->find('~') != std::string::npos)
        err("ssh.auth.identityFile", "密钥路径不能含引号、% 或 ~，请使用完整绝对路径");
    }
  } else {
    if (c.endpoint || c.ssh) err("endpoint", "本地连接不能包含 SSH 字段");
    if (!c.local) { err("local", "本地连接必须有 local"); return errs; }
    validateLocal(*c.local, errs);
  }
  return errs;
}

bool FileValidation::ok() const {
  return fileErrors.empty() && byConnection.empty();
}

std::vector<ValidationError> validateInFile(const ConnectionsFile& data, const Connection& c) {
  auto errs = validate(c);
  if (c.id.empty()) errs.push_back({"id", "连接 ID 不能为空"});
  int matches = 0;
  for (const auto& other : data.connections) if (other.id == c.id) ++matches;
  if (matches > 1) errs.push_back({"id", "连接 ID 重复"});
  if (!c.groupId.empty() && std::none_of(data.groups.begin(), data.groups.end(),
                                        [&](const Group& group) { return group.id == c.groupId; }))
    errs.push_back({"groupId", "分组不存在"});
  return errs;
}

FileValidation validateConnectionsFile(const ConnectionsFile& data) {
  FileValidation result;
  std::set<std::string> groupIds;
  for (const auto& group : data.groups)
    if (!groupIds.insert(group.id).second)
      result.fileErrors.push_back({"groups.id", "组 ID 重复：" + group.id});
  for (const auto& c : data.connections) {
    auto errs = validateInFile(data, c);
    if (!errs.empty()) {
      // 同 ID 的每条连接都保留自己的错误，不能覆盖前一条。
      auto& combined = result.byConnection[c.id];
      combined.insert(combined.end(), errs.begin(), errs.end());
    }
  }
  return result;
}

std::string launchBlockedMessage(const std::vector<ValidationError>& errors) {
  if (errors.empty()) return {};
  return "连接配置无效（" + errors.front().field + "）：" + errors.front().message;
}

ValidationReport reportValidation(const ConnectionsFile& data, const FileValidation& validation) {
  ValidationReport report;
  if (validation.ok()) return report;
  int invalid = 0;
  for (const auto& c : data.connections)
    if (validation.byConnection.count(c.id)) ++invalid;
  report.statusNote = "connections.json 有 " + std::to_string(invalid) + " 条连接配置无效，已禁止启动";
  if (!validation.fileErrors.empty())
    report.statusNote += "；文件级配置错误 " + std::to_string(validation.fileErrors.size()) + " 项";
  for (const auto& error : validation.fileErrors)
    report.details.push_back("connections.json（" + error.field + "）：" + error.message);
  for (const auto& [id, errors] : validation.byConnection)
    for (const auto& error : errors)
      report.details.push_back("connections.json 连接 " + id + "（" + error.field + "）：" + error.message);
  return report;
}

}  // namespace uicfg
