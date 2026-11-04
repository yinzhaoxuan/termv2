// SPDX-License-Identifier: GPL-3.0-or-later
// DS-14：连接编辑器的表单、分组、菜单与保存冲突。
#include "uicfg/connection_form.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Connection manual() {
  Connection c;
  c.id = "conn-1"; c.name = "服务器"; c.groupId = "group-1";
  c.appearanceProfileId = "custom"; c.order = 7;
  c.endpoint = Endpoint{EndpointMode::Manual, "server", 2222, std::nullopt};
  c.ssh = SshSettings{"dev", AuthMode::KeyFile, "C:/keys/id", 37, "compatible"};
  return c;
}
static Connection hyperv() {
  auto c = manual();
  c.endpoint = Endpoint{EndpointMode::HyperV, std::nullopt, 2200,
      HyperVEndpoint{"future-scope", "vm-guid", "adapter-id", "ipv6", "fallback"}};
  c.ssh->authMode = AuthMode::Agent; c.ssh->identityFile.reset(); c.ssh->transport = "direct";
  return c;
}
static Connection local() {
  auto c = manual(); c.kind = ConnectionKind::Local; c.endpoint.reset(); c.ssh.reset();
  c.local = LocalSettings{"pwsh.exe", {"-NoLogo", "  text with spaces  "}, "C:/work",
                          LocalEnvironment{false, {{"A", "B"}}, {"TEMP"}}, "terminateSessionTree"};
  return c;
}
static ConnectionsFile fileWith(const Connection& c) {
  ConnectionsFile cf; cf.groups = {{"group-1", "开发", 0}, {"group-2", "生产", 8}};
  cf.connections = {c}; return cf;
}
// 逐项比较全部模型字段，包含表单不负责的字段。
static bool same(const Connection& a, const Connection& b) {
  if (a.id != b.id || a.name != b.name || a.groupId != b.groupId ||
      a.appearanceProfileId != b.appearanceProfileId || a.order != b.order || a.kind != b.kind ||
      a.endpoint.has_value() != b.endpoint.has_value() || a.ssh.has_value() != b.ssh.has_value() ||
      a.local.has_value() != b.local.has_value()) return false;
  if (a.endpoint) {
    const auto& x = *a.endpoint; const auto& y = *b.endpoint;
    if (x.mode != y.mode || x.host != y.host || x.port != y.port || x.hyperv.has_value() != y.hyperv.has_value()) return false;
    if (x.hyperv) {
      const auto& h = *x.hyperv; const auto& k = *y.hyperv;
      if (h.hostScope != k.hostScope || h.vmId != k.vmId || h.adapterId != k.adapterId ||
          h.addressFamily != k.addressFamily || h.fallbackHost != k.fallbackHost) return false;
    }
  }
  if (a.ssh) {
    const auto& x = *a.ssh; const auto& y = *b.ssh;
    if (x.user != y.user || x.authMode != y.authMode || x.identityFile != y.identityFile ||
        x.connectTimeoutSeconds != y.connectTimeoutSeconds || x.transport != y.transport) return false;
  }
  if (a.local) {
    const auto& x = *a.local; const auto& y = *b.local;
    if (x.executable != y.executable || x.arguments != y.arguments || x.workingDirectory != y.workingDirectory ||
        x.closePolicy != y.closePolicy || x.environment.inherit != y.environment.inherit ||
        x.environment.set != y.environment.set || x.environment.unset != y.environment.unset) return false;
  }
  return true;
}
static void roundtrip() {
  for (auto c : {manual(), hyperv(), local()}) {
    const auto result = applyConnectionForm(formFromConnection(c), c, fileWith(c));
    CHECK(result.errors.empty() && result.connection);
    if (result.connection) CHECK(same(c, *result.connection));
  }
  auto c = local(); c.local->closePolicy = "future-policy";
  auto result = applyConnectionForm(formFromConnection(c), c, fileWith(c));
  CHECK(result.connection && same(c, *result.connection));
  CHECK(!validate(*result.connection).empty());
}
static bool hasError(const std::vector<ValidationError>& errors, const std::string& field) {
  return std::any_of(errors.begin(), errors.end(), [&](const auto& e) { return e.field == field; });
}
static void fields() {
  auto c = manual(); const auto cf = fileWith(c);
  for (const auto& text : {"abc", "", "+22", "-1", "1.5", "22x", "999999999999999999999999"}) {
    auto f = formFromConnection(c); f.port = text; f.connectTimeout = text;
    auto r = applyConnectionForm(f, c, cf);
    CHECK(!r.connection && r.errors.size() == 2);
    CHECK(hasError(r.errors, "endpoint.port") && hasError(r.errors, "ssh.connectTimeoutSeconds"));
    auto invalid = c; invalid.endpoint->port = 0; invalid.ssh->connectTimeoutSeconds = 0;
    const auto expected = validate(invalid);
    for (const auto& e : r.errors)
      CHECK(std::any_of(expected.begin(), expected.end(), [&](const auto& x) { return x.field == e.field && x.message == e.message; }));
  }
  for (const auto& text : {"0", "65536", "2147483647"}) {
    auto f = formFromConnection(c); f.port = text;
    auto r = applyConnectionForm(f, c, cf);
    CHECK(r.connection && hasError(validate(*r.connection), "endpoint.port"));
  }
  for (const auto& text : {"0", "601", "2147483647"}) {
    auto f = formFromConnection(c); f.connectTimeout = text;
    auto r = applyConnectionForm(f, c, cf);
    CHECK(r.connection && hasError(validate(*r.connection), "ssh.connectTimeoutSeconds"));
  }
  auto f = formFromConnection(c);
  f.name = "  新名称\t"; f.host = " server2\r\n"; f.user = " user \t";
  f.identityFile = " C:/key2 "; f.port = " 00022\t"; f.connectTimeout = "\n600 ";
  auto r = applyConnectionForm(f, c, cf); CHECK(r.connection);
  if (!r.connection) return;
  CHECK(r.connection->name == "新名称" && r.connection->endpoint->host == "server2");
  CHECK(r.connection->ssh->user == "user" && r.connection->ssh->identityFile == "C:/key2");
  CHECK(r.connection->endpoint->port == 22 && r.connection->ssh->connectTimeoutSeconds == 600);
  for (auto auth : {AuthMode::Agent, AuthMode::Interactive}) {
    f.auth = auth; r = applyConnectionForm(f, c, cf); CHECK(!r.connection->ssh->identityFile);
  }
  f = formFromConnection(hyperv()); f.adapterId = " \t"; f.fallbackHost = "\r\n"; f.vmId = " vm2 ";
  r = applyConnectionForm(f, hyperv(), cf);
  CHECK(!r.connection->endpoint->hyperv->adapterId && !r.connection->endpoint->hyperv->fallbackHost);
  CHECK(r.connection->endpoint->hyperv->vmId == "vm2");
  f = formFromConnection(local()); f.workingDirectory = " "; f.executable = " pwsh.exe ";
  f.argumentsText = "a\r\n\n b \n\r\nc\r\n  \n";
  r = applyConnectionForm(f, local(), cf);
  CHECK(!r.connection->local->workingDirectory && r.connection->local->executable == "pwsh.exe");
  CHECK((r.connection->local->arguments == std::vector<std::string>{"a", " b ", "c", "  "}));

  f = formFromConnection(c); f.kind = FormKind::Local; f.executable = "pwsh.exe";
  r = applyConnectionForm(f, c, cf);
  CHECK(!r.connection->endpoint && !r.connection->ssh && r.connection->local);
  CHECK(r.connection->local->environment.inherit && r.connection->local->environment.set.empty());
  f = formFromConnection(local()); f.kind = FormKind::SshManual;
  r = applyConnectionForm(f, local(), cf);
  CHECK(!r.connection->local && r.connection->endpoint && r.connection->ssh);
  f = formFromConnection(c); f.kind = FormKind::SshHyperV; f.vmId = "vm";
  r = applyConnectionForm(f, c, cf); CHECK(!r.connection->endpoint->host && r.connection->endpoint->hyperv);
  f = formFromConnection(hyperv()); f.kind = FormKind::SshManual; f.host = "server";
  r = applyConnectionForm(f, hyperv(), cf); CHECK(r.connection->endpoint->host && !r.connection->endpoint->hyperv);

  for (const auto& raw : {"future", "7", "null", "{}", ""}) {
    c.ssh->transport = raw; f = formFromConnection(c);
    CHECK(!f.transport); r = applyConnectionForm(f, c, cf);
    CHECK(r.connection->ssh->transport == raw && hasError(validate(*r.connection), "ssh.transport"));
    f.transport = SshTransport::Compatible; r = applyConnectionForm(f, c, cf);
    CHECK(r.connection->ssh->transport == "compatible" && validate(*r.connection).empty());
    f.transport = SshTransport::Direct; f.auth = AuthMode::Agent; r = applyConnectionForm(f, c, cf);
    CHECK(r.connection->ssh->transport == "direct" && validate(*r.connection).empty());
  }
  c.ssh->transport = "compatible"; f = formFromConnection(c); f.transport.reset();
  CHECK(!applyConnectionForm(f, c, cf).connection->ssh->transport);
  f.transport = SshTransport::Direct;
  CHECK(hasError(validate(*applyConnectionForm(f, c, cf).connection), "ssh.auth.mode"));
}
static void creation() {
  auto cf = fileWith(manual()); auto other = manual(); other.id = "conn-3"; other.order = 40;
  cf.connections.push_back(other);
  CHECK(newConnectionId(cf) == "conn-2");
  auto f = newConnectionForm("group-1");
  CHECK(f.appearanceProfileId == "appearance-default" && f.port == "22" && f.connectTimeout == "15");
  CHECK(f.auth == AuthMode::Agent && !f.transport && f.argumentsEditable);
  f.name = "new"; f.host = "server"; f.user = "dev";
  auto r = applyConnectionForm(f, std::nullopt, cf);
  CHECK(r.connection->id == "conn-2" && r.connection->order == 41);
  f.groupId = "group-2"; r = applyConnectionForm(f, std::nullopt, cf);
  CHECK(r.connection->order == 0);
  cf.connections.back().groupId = "group-2";
  r = applyConnectionForm(f, cf.connections.front(), cf);
  CHECK(r.connection->order == 41 && r.connection->id == "conn-1");
  f.groupId = "group-1"; r = applyConnectionForm(f, cf.connections.front(), cf);
  CHECK(r.connection->order == 7);
}
static void groups() {
  ConnectionsFile cf;
  CHECK(groupNameError(cf, " \r\n")); CHECK(groupNameError(cf, "a\tb"));
  CHECK(groupNameError(cf, std::string("a\0b", 3))); CHECK(groupNameError(cf, "a\x7f"));
  CHECK(groupNameError(cf, std::string(65, 'x'))); CHECK(!groupNameError(cf, std::string(64, 'x')));
  std::string chineseName;
  for (int i = 0; i < 21; ++i) chineseName += "组";
  CHECK(!groupNameError(cf, chineseName));
  CHECK(groupNameError(cf, chineseName + "组"));
  CHECK(addGroup(cf, "  开发 ") == "group-1" && cf.groups[0].order == 0 && cf.groups[0].name == "开发");
  cf.groups.push_back({"group-3", "Production", 20});
  CHECK(addGroup(cf, "生产") == "group-2" && cf.groups.back().order == 21);
  CHECK(groupNameError(cf, " 开发 ")); CHECK(!groupNameError(cf, "开发", "group-1"));
  CHECK(!groupNameError(cf, "production"));
  CHECK(!renameGroup(cf, "group-1", "生产")); CHECK(!renameGroup(cf, "missing", "新组"));
  CHECK(renameGroup(cf, "group-1", " 测试 ") && cf.groups[0].name == "测试");
  CHECK(addGroup(cf, "生产").empty());
  auto a = manual(); a.groupId = "group-1"; a.order = 9;
  auto b = a; b.id = "b"; b.order = 2;
  auto u = a; u.id = "u"; u.groupId.clear(); u.order = 30;
  auto stay = a; stay.id = "stay"; stay.groupId = "group-2";
  cf.connections = {a, b, u, stay};
  CHECK(deleteGroup(cf, "group-1") == 2);
  CHECK(cf.connections[0].groupId.empty() && cf.connections[0].order == 32);
  CHECK(cf.connections[1].groupId.empty() && cf.connections[1].order == 31);
  CHECK(cf.connections[2].order == 30 && cf.connections[3].groupId == "group-2");
  CHECK(deleteGroup(cf, "missing") == 0 && deleteGroup(cf, "") == 0);
  CHECK(deleteGroup(cf, "group-3") == 0);
  CHECK(cf.groups.size() == 1 && cf.groups[0].id == "group-2");
}
static void menus() {
  using A = TreeAction;
  for (auto target : {TreeTarget::Empty, TreeTarget::LocalShell, TreeTarget::Group, TreeTarget::Ungrouped, TreeTarget::Connection}) {
    auto menu = treeMenuFor(target, false);
    CHECK(menu.back().action == A::NewGroup && menu.back().label == "新建分组…");
    if (target == TreeTarget::Connection) {
      CHECK(menu.size() == 6);
      CHECK(menu[0].action == A::Connect && menu[0].label == "连接");
      CHECK(menu[1].action == A::EditConnection && menu[1].label == "编辑…");
      CHECK(menu[2].action == A::DeleteConnection && menu[2].label == "删除…");
      CHECK(menu[3].label.empty() && !menu[3].enabled && menu[4].action == A::NewConnection);
    } else if (target == TreeTarget::Group) {
      CHECK(menu.size() == 5 && menu[0].action == A::NewConnection && menu[0].label == "在此分组新建连接…");
      CHECK(menu[1].action == A::RenameGroup && menu[2].action == A::DeleteGroup && menu[3].label.empty());
    } else CHECK(menu.size() == 2 && menu[0].action == A::NewConnection);
    const auto ro = treeMenuFor(target, true);
    CHECK(ro.size() == menu.size() + 1 && ro.back().label == "connections.json 只读，无法编辑" && !ro.back().enabled);
    for (std::size_t i = 0; i < menu.size(); ++i) {
      CHECK(ro[i].label == menu[i].label && ro[i].action == menu[i].action);
      CHECK(menu[i].enabled == !menu[i].label.empty());
      CHECK(ro[i].enabled == (!ro[i].label.empty() && ro[i].action == A::Connect));
    }
  }
}
static void prompts() {
  auto c = manual(); c.name = "web-1";
  CHECK(deleteConnectionPrompt(c, 0) == "删除连接「web-1」？");
  CHECK(deleteConnectionPrompt(c, 2) == "删除连接「web-1」？\n正在运行的 2 个窗格不受影响，但关闭后无法再从这里重连。");
  Group group{"g", "生产", 0};
  CHECK(deleteGroupPrompt(group, 0) == "删除分组「生产」？");
  CHECK(deleteGroupPrompt(group, 3) == "删除分组「生产」？其中 3 个连接将移到「未分组」。");
  CHECK(hyperVListError("ModuleMissing", "") == "未安装 Hyper-V PowerShell 模块");
  CHECK(hyperVListError("AccessDenied", "denied").find("没有权限") == 0);
  CHECK(hyperVListError("Timeout", "") == "查询 Hyper-V 虚拟机超时");
  CHECK(hyperVListError("Failed", "details") == "查询 Hyper-V 虚拟机失败：details");
  CHECK(hyperVListError("BadOutput", "") == "查询 Hyper-V 虚拟机失败");
}
static void stamps() {
  const auto dir = fs::temp_directory_path() / "uicfg_connection_form_stamps";
  fs::remove_all(dir); fs::create_directories(dir);
  const auto path = dir / "connections.json";
  auto cf = fileWith(manual()); auto stamp = fileStamp(path); std::string error;
  CHECK(!stamp.exists);
  CHECK(saveConnectionsChecked(path, cf, false, &stamp, &error));
  CHECK(stamp.exists && stamp == fileStamp(path) && cf.revision == 1);
  auto oldStamp = stamp;
  cf.connections[0].name = "另一个名称";
  CHECK(saveConnectionsChecked(path, cf, false, &stamp, &error));
  CHECK(stamp == fileStamp(path) && cf.revision == 2 && stamp != oldStamp);
  CHECK(fs::exists(path.string() + ".bak"));
  CHECK(loadConnections(path.string() + ".bak").data.connections[0].name == "服务器");
  const auto revision = cf.revision;
  auto rejected = [&] {
    const auto before = fileStamp(path);
    const auto expectedBefore = stamp;
    CHECK(!saveConnectionsChecked(path, cf, false, &stamp, &error));
    CHECK(stamp == expectedBefore);
    CHECK(error == "connections.json 已在程序外被修改；为避免覆盖，请重启后再编辑");
    CHECK(cf.revision == revision && fileStamp(path) == before);
  };
  std::ofstream(path, std::ios::app) << " "; rejected(); // 大小变化不依赖时间精度。
  stamp = fileStamp(path); fs::last_write_time(path, stamp.mtime + std::chrono::seconds(10)); rejected();
  stamp = fileStamp(path);
  // 同大小内容修改配合显式 mtime 变化，避免文件系统时间精度造成偶发结果。
  std::ifstream in(path, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)), {}); in.close();
  bytes[0] = '['; std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
  fs::last_write_time(path, stamp.mtime + std::chrono::seconds(10)); rejected();
  stamp = fileStamp(path); fs::remove(path); rejected();
  stamp = fileStamp(path);
  CHECK(!saveConnectionsChecked(path, cf, true, &stamp, &error) && !fs::exists(path));
  CHECK(!saveConnectionsChecked(path, cf, false, nullptr, &error));
  cf.connections.push_back(manual()); cf.connections.back().id = "bad"; cf.connections.back().endpoint->port = 0;
  CHECK(!saveConnectionsChecked(path, cf, false, &stamp, &error));
  CHECK(error == "连接 bad 校验失败: 端口必须是 1–65535 的整数" && !fs::exists(path));
  CHECK(!saveConnectionsChecked(dir, cf, false, &stamp, &error));
  CHECK(error.find("读取 connections.json 文件戳失败：") == 0 && fs::is_directory(dir));
  CHECK(cf.revision == revision && !stamp.exists);
  fs::remove_all(dir);
}
static void uneditableArguments() {
  for (const auto& args : {std::vector<std::string>{"", "a"}, {"a\nb", "c"}, {"a\rb"}, {""}}) {
    auto c = local(); c.local->arguments = args;
    auto f = formFromConnection(c); CHECK(!f.argumentsEditable);
    f.name = "改名称"; f.argumentsText = "丢弃这段文本";
    auto r = applyConnectionForm(f, c, fileWith(c));
    CHECK(r.connection->name == "改名称" && r.connection->local->arguments == args);
    CHECK(validate(*r.connection).empty());
  }
  auto c = local(); auto f = formFromConnection(c);
  CHECK(f.argumentsEditable && f.argumentsText == "-NoLogo\r\n  text with spaces  ");
  c.local->arguments.clear(); f = formFromConnection(c);
  CHECK(f.argumentsEditable && f.argumentsText.empty());
}
int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "roundtrip" || mode == "all") roundtrip();
  if (mode == "fields" || mode == "all") fields();
  if (mode == "creation" || mode == "all") creation();
  if (mode == "groups" || mode == "all") groups();
  if (mode == "menus" || mode == "all") menus();
  if (mode == "prompts" || mode == "all") prompts();
  if (mode == "stamps" || mode == "all") stamps();
  if (mode == "arguments" || mode == "all") uneditableArguments();
  return fails != 0;
}
