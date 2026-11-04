// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/connection_form.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace uicfg {
namespace {
std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n\v\f");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n\v\f") - b + 1);
}
std::optional<std::string> optionalText(const std::string& s) {
  auto value = trim(s);
  return value.empty() ? std::nullopt : std::optional<std::string>(std::move(value));
}
bool decimal(const std::string& text, int& value) {
  const auto s = trim(text);
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
  const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}
std::vector<std::string> arguments(const std::string& text) {
  std::vector<std::string> out;
  std::string_view remaining(text);
  while (!remaining.empty()) {
    const auto end = remaining.find('\n');
    auto line = remaining.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) out.emplace_back(line);
    if (end == std::string_view::npos) break;
    remaining.remove_prefix(end + 1);
  }
  return out;
}
int afterOrder(std::optional<int> maximum) {
  if (!maximum) return 0;
  // 手改到 int 上限时仍保持稳定次序，避免加法溢出。
  return *maximum == (std::numeric_limits<int>::max)() ? *maximum : *maximum + 1;
}
int nextConnectionOrder(const ConnectionsFile& cf, const std::string& groupId) {
  std::optional<int> maximum;
  for (const auto& c : cf.connections)
    if (c.groupId == groupId && (!maximum || c.order > *maximum)) maximum = c.order;
  return afterOrder(maximum);
}
template<class Items> std::string freeId(const Items& items, const char* prefix) {
  std::set<std::string> used;
  for (const auto& item : items) used.insert(item.id);
  for (std::size_t n = 1; ; ++n) {
    auto id = prefix + std::to_string(n);
    if (!used.count(id)) return id;
  }
}
}  // namespace

ConnectionForm newConnectionForm(const std::string& groupId) {
  ConnectionForm form;
  form.groupId = groupId;
  form.appearanceProfileId = "appearance-default";
  return form;
}
ConnectionForm formFromConnection(const Connection& c) {
  ConnectionForm f;
  f.name = c.name; f.groupId = c.groupId; f.appearanceProfileId = c.appearanceProfileId;
  if (c.kind == ConnectionKind::Local) {
    f.kind = FormKind::Local;
    if (c.local) {
      f.executable = c.local->executable;
      f.workingDirectory = c.local->workingDirectory.value_or("");
      for (const auto& arg : c.local->arguments) {
        if (arg.empty() || arg.find_first_of("\r\n") != std::string::npos) f.argumentsEditable = false;
      }
      for (std::size_t i = 0; i < c.local->arguments.size(); ++i) {
        if (i) f.argumentsText += "\r\n";
        f.argumentsText += c.local->arguments[i];
      }
    }
  } else {
    if (c.endpoint) {
      f.port = std::to_string(c.endpoint->port);
      f.host = c.endpoint->host.value_or("");
      if (c.endpoint->mode == EndpointMode::HyperV) {
        f.kind = FormKind::SshHyperV;
        if (c.endpoint->hyperv) {
          f.vmId = c.endpoint->hyperv->vmId;
          f.adapterId = c.endpoint->hyperv->adapterId.value_or("");
          f.fallbackHost = c.endpoint->hyperv->fallbackHost.value_or("");
        }
      }
    }
    if (c.ssh) {
      f.user = c.ssh->user; f.auth = c.ssh->authMode;
      f.identityFile = c.ssh->identityFile.value_or("");
      f.transport = parseSshTransport(*c.ssh);
      f.connectTimeout = std::to_string(c.ssh->connectTimeoutSeconds);
    }
  }
  return f;
}
std::string newConnectionId(const ConnectionsFile& cf) { return freeId(cf.connections, "conn-"); }

FormApplyResult applyConnectionForm(const ConnectionForm& f, const std::optional<Connection>& base,
                                   const ConnectionsFile& cf) {
  Connection c = base.value_or(Connection{});
  c.name = trim(f.name); c.groupId = f.groupId; c.appearanceProfileId = f.appearanceProfileId;
  if (!base) c.id = newConnectionId(cf);
  if (!base || base->groupId != c.groupId) c.order = nextConnectionOrder(cf, c.groupId);
  bool portOk = true, timeoutOk = true;
  if (f.kind == FormKind::Local) {
    c.kind = ConnectionKind::Local;
    auto local = base && base->kind == ConnectionKind::Local ? base->local.value_or(LocalSettings{}) : LocalSettings{};
    local.executable = trim(f.executable);
    local.workingDirectory = optionalText(f.workingDirectory);
    if (f.argumentsEditable) local.arguments = arguments(f.argumentsText);
    c.local = std::move(local); c.endpoint.reset(); c.ssh.reset();
  } else {
    c.kind = ConnectionKind::Ssh; c.local.reset();
    Endpoint ep;
    portOk = decimal(f.port, ep.port);
    if (!portOk) ep.port = 0;
    if (f.kind == FormKind::SshManual) ep.host = trim(f.host);
    else {
      ep.mode = EndpointMode::HyperV;
      auto hv = base && base->endpoint ? base->endpoint->hyperv.value_or(HyperVEndpoint{}) : HyperVEndpoint{};
      hv.vmId = trim(f.vmId); hv.adapterId = optionalText(f.adapterId); hv.fallbackHost = optionalText(f.fallbackHost);
      ep.hyperv = std::move(hv);
    }
    SshSettings ssh;
    ssh.user = trim(f.user); ssh.authMode = f.auth;
    if (f.auth == AuthMode::KeyFile) ssh.identityFile = optionalText(f.identityFile);
    timeoutOk = decimal(f.connectTimeout, ssh.connectTimeoutSeconds);
    if (!timeoutOk) ssh.connectTimeoutSeconds = 0;
    if (base && base->ssh && base->ssh->transport && f.transport == parseSshTransport(*base->ssh))
      ssh.transport = base->ssh->transport;
    else if (f.transport)
      ssh.transport = *f.transport == SshTransport::Direct ? "direct" : "compatible";
    c.endpoint = std::move(ep); c.ssh = std::move(ssh);
  }
  FormApplyResult result;
  if (!portOk || !timeoutOk) {
    // 非十进制值用校验器的同一条文案；范围检查仍由调用方校验。
    for (const auto& error : validate(c))
      if ((!portOk && error.field == "endpoint.port") ||
          (!timeoutOk && error.field == "ssh.connectTimeoutSeconds")) result.errors.push_back(error);
  } else result.connection = std::move(c);
  return result;
}

std::optional<std::string> groupNameError(const ConnectionsFile& cf, const std::string& name,
                                        const std::string& exceptGroupId) {
  const auto value = trim(name);
  if (value.empty()) return "分组名称不能为空";
  for (unsigned char ch : value)
    if (ch < 0x20 || ch == 0x7f) return "分组名称不能含控制字符";
  if (value.size() > 64) return "分组名称不能超过 64 字节";
  for (const auto& group : cf.groups)
    if (group.id != exceptGroupId && group.name == value) return "分组名称已存在";
  return std::nullopt;
}
std::string addGroup(ConnectionsFile& cf, const std::string& name) {
  if (groupNameError(cf, name)) return {};
  const auto id = freeId(cf.groups, "group-");
  std::optional<int> maximum;
  for (const auto& group : cf.groups)
    if (!maximum || group.order > *maximum) maximum = group.order;
  cf.groups.push_back({id, trim(name), afterOrder(maximum)});
  return id;
}
bool renameGroup(ConnectionsFile& cf, const std::string& id, const std::string& name) {
  if (groupNameError(cf, name, id)) return false;
  for (auto& group : cf.groups) if (group.id == id) { group.name = trim(name); return true; }
  return false;
}
int deleteGroup(ConnectionsFile& cf, const std::string& id) {
  if (id.empty() || !std::erase_if(cf.groups, [&](const Group& group) { return group.id == id; })) return 0;
  std::vector<Connection*> moved;
  for (auto& c : cf.connections) if (c.groupId == id) moved.push_back(&c);
  std::stable_sort(moved.begin(), moved.end(), [](auto a, auto b) { return a->order < b->order; });
  int order = nextConnectionOrder(cf, "");
  for (auto* c : moved) { c->groupId.clear(); c->order = order; order = afterOrder(order); }
  return static_cast<int>(moved.size());
}
std::string deleteConnectionPrompt(const Connection& c, int runningSessions) {
  auto text = "删除连接「" + c.name + "」？";
  if (runningSessions > 0) text += "\n正在运行的 " + std::to_string(runningSessions) + " 个窗格不受影响，但关闭后无法再从这里重连。";
  return text;
}
std::string deleteGroupPrompt(const Group& group, int connectionCount) {
  auto text = "删除分组「" + group.name + "」？";
  if (connectionCount > 0) text += "其中 " + std::to_string(connectionCount) + " 个连接将移到「未分组」。";
  return text;
}
std::string hyperVListError(const std::string& name, const std::string& message) {
  std::string text;
  if (name == "ModuleMissing") text = "未安装 Hyper-V PowerShell 模块";
  else if (name == "AccessDenied") text = "没有权限查询 Hyper-V 虚拟机，请以管理员身份运行或加入 Hyper-V Administrators";
  else if (name == "Timeout") text = "查询 Hyper-V 虚拟机超时";
  else text = "查询 Hyper-V 虚拟机失败";
  if (!message.empty()) text += "：" + message;
  return text;
}
std::vector<TreeMenuItem> treeMenuFor(TreeTarget target, bool readOnly) {
  using A = TreeAction;
  std::vector<TreeMenuItem> items;
  auto add = [&](A action, const char* label) { items.push_back({action, label, !readOnly || action == A::Connect}); };
  auto separator = [&] { items.push_back({A::Connect, "", false}); };
  if (target == TreeTarget::Connection) {
    add(A::Connect, "连接"); add(A::EditConnection, "编辑…"); add(A::DeleteConnection, "删除…"); separator();
  }
  if (target == TreeTarget::Group) {
    add(A::NewConnection, "在此分组新建连接…"); add(A::RenameGroup, "重命名分组…"); add(A::DeleteGroup, "删除分组…"); separator();
  } else add(A::NewConnection, "新建连接…");
  add(A::NewGroup, "新建分组…");
  if (readOnly) items.push_back({A::NewGroup, "connections.json 只读，无法编辑", false});
  return items;
}
}  // namespace uicfg
