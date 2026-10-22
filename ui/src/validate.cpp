#include "uicfg/model.h"

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

}  // namespace

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
    const std::string u = trim(ssh.user);
    if (u.empty()) err("ssh.user", "用户名不能为空");
    else if (hasControl(u) || u[0] == '-' ||
             u.find_first_of(" @;|&`$") != std::string::npos)
      err("ssh.user", "用户名不能包含空格、@ 或命令字符");
    if (ssh.authMode == AuthMode::KeyFile) {
      if (!ssh.identityFile || ssh.identityFile->empty()) err("ssh.auth.identityFile", "密钥文件模式必须选择路径");
      else if (hasNul(*ssh.identityFile)) err("ssh.auth.identityFile", "路径不能含 NUL");
    }
  } else {
    if (c.endpoint || c.ssh) err("endpoint", "本地连接不能包含 SSH 字段");
    if (!c.local) { err("local", "本地连接必须有 local"); return errs; }
    if (c.local->executable.empty()) err("local.executable", "必须选择可执行文件");
    else if (hasNul(c.local->executable)) err("local.executable", "路径不能含 NUL");
    for (const auto& a : c.local->arguments)
      if (hasNul(a)) { err("local.arguments", "参数不能含 NUL"); break; }
  }
  return errs;
}

}  // namespace uicfg
