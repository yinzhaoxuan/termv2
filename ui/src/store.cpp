#include "uicfg/store.h"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

namespace uicfg {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

template <class T> std::optional<T> opt(const json& j, const char* k) {
  if (!j.contains(k) || j.at(k).is_null()) return std::nullopt;
  return j.at(k).get<T>();
}

AuthMode authFrom(const std::string& s) {
  if (s == "agent") return AuthMode::Agent;
  if (s == "keyFile") return AuthMode::KeyFile;
  if (s == "interactive") return AuthMode::Interactive;
  throw std::runtime_error("未知 auth.mode: " + s);
}
const char* authTo(AuthMode m) {
  switch (m) { case AuthMode::Agent: return "agent"; case AuthMode::KeyFile: return "keyFile"; default: return "interactive"; }
}
json nullable(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }

Connection connFrom(const json& j) {
  Connection c;
  c.id = j.at("id"); c.name = j.at("name"); c.groupId = j.value("groupId", "");
  c.order = j.value("order", 0);
  c.appearanceProfileId = j.value("appearanceProfileId", "appearance-default");
  const std::string kind = j.at("kind");
  if (kind == "ssh") {
    c.kind = ConnectionKind::Ssh;
    const auto& e = j.at("endpoint");
    Endpoint ep;
    const std::string mode = e.at("mode");
    if (mode == "manual") ep.mode = EndpointMode::Manual;
    else if (mode == "hyperv") ep.mode = EndpointMode::HyperV;
    else throw std::runtime_error("未知 endpoint.mode: " + mode);
    ep.host = opt<std::string>(e, "host");
    ep.port = e.value("port", 22);
    if (e.contains("hyperv") && !e.at("hyperv").is_null()) {
      const auto& h = e.at("hyperv");
      HyperVEndpoint hv;
      hv.hostScope = h.value("hostScope", "local"); hv.vmId = h.at("vmId");
      hv.adapterId = opt<std::string>(h, "adapterId");
      hv.addressFamily = h.value("addressFamily", "ipv4");
      hv.fallbackHost = opt<std::string>(h, "fallbackHost");
      ep.hyperv = hv;
    }
    c.endpoint = ep;
    const auto& s = j.at("ssh");
    if (s.value("configSource", "managed") != "managed")
      throw std::runtime_error("首版只支持 configSource=managed");
    SshSettings ss;
    ss.user = s.at("user");
    ss.authMode = authFrom(s.at("auth").at("mode"));
    ss.identityFile = opt<std::string>(s.at("auth"), "identityFile");
    ss.connectTimeoutSeconds = s.value("connectTimeoutSeconds", 15);
    c.ssh = ss;
  } else if (kind == "local") {
    c.kind = ConnectionKind::Local;
    const auto& l = j.at("local");
    LocalSettings ls;
    ls.executable = l.at("executable");
    ls.arguments = l.value("arguments", std::vector<std::string>{});
    ls.workingDirectory = opt<std::string>(l, "workingDirectory");
    c.local = ls;
  } else {
    throw std::runtime_error("未知 kind: " + kind);
  }
  return c;
}

json connTo(const Connection& c) {
  json j{{"id", c.id}, {"name", c.name}, {"groupId", c.groupId},
         {"appearanceProfileId", c.appearanceProfileId}, {"order", c.order}};
  if (c.kind == ConnectionKind::Ssh) {
    const auto& ep = *c.endpoint;
    json e{{"mode", ep.mode == EndpointMode::Manual ? "manual" : "hyperv"},
           {"host", nullable(ep.host)}, {"port", ep.port}, {"hyperv", nullptr}};
    if (ep.hyperv)
      e["hyperv"] = {{"hostScope", ep.hyperv->hostScope}, {"vmId", ep.hyperv->vmId},
                     {"adapterId", nullable(ep.hyperv->adapterId)},
                     {"addressFamily", ep.hyperv->addressFamily},
                     {"fallbackHost", nullable(ep.hyperv->fallbackHost)}};
    j["kind"] = "ssh"; j["endpoint"] = e;
    j["ssh"] = {{"user", c.ssh->user}, {"configSource", "managed"},
                {"auth", {{"mode", authTo(c.ssh->authMode)}, {"identityFile", nullable(c.ssh->identityFile)}}},
                {"hostKeyPolicy", "ask"},
                {"connectTimeoutSeconds", c.ssh->connectTimeoutSeconds}};
  } else {
    j["kind"] = "local";
    j["local"] = {{"executable", c.local->executable}, {"arguments", c.local->arguments},
                  {"workingDirectory", nullable(c.local->workingDirectory)},
                  {"environment", {{"inherit", true}, {"set", json::object()}, {"unset", json::array()}}},
                  {"closePolicy", "terminateSessionTree"}};
  }
  return j;
}

enum class Parse { Ok, Newer, Bad };

Parse parseFile(const fs::path& p, ConnectionsFile& out, std::string& why) {
  try {
    std::ifstream in(p, std::ios::binary);
    json j = json::parse(in);
    int v = j.at("schemaVersion");
    if (v > kSchemaVersion) { why = "schemaVersion " + std::to_string(v) + " 高于支持的版本"; return Parse::Newer; }
    if (v < 1) { why = "无效 schemaVersion"; return Parse::Bad; }
    ConnectionsFile f; f.schemaVersion = v; f.revision = j.value("revision", 0LL);
    for (const auto& g : j.value("groups", json::array()))
      f.groups.push_back({g.at("id"), g.at("name"), g.value("order", 0)});
    for (const auto& c : j.value("connections", json::array()))
      f.connections.push_back(connFrom(c));
    out = std::move(f);
    return Parse::Ok;
  } catch (const std::exception& e) { why = e.what(); return Parse::Bad; }
}

}  // namespace (helpers below are public)

bool writeDurable(const fs::path& tmp, const std::string& bytes, std::string* err) {
#ifdef _WIN32
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = "创建临时文件失败: " + std::to_string(GetLastError()); return false; }
  DWORD written = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(h);
  if (!ok && err) *err = "写临时文件失败: " + std::to_string(GetLastError());
  CloseHandle(h);
  return ok;
#else
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out << bytes; out.flush();
  if (!out && err) *err = "写临时文件失败";
  return static_cast<bool>(out);
#endif
}

bool replaceFile(const fs::path& tmp, const fs::path& target, std::string* err) {
#ifdef _WIN32
  // 杀毒/索引服务短暂占用时重试；仍失败就报错，绝不退回非原子写
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DWORD e = GetLastError();
    if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) {
      if (err) *err = "MoveFileExW 失败: " + std::to_string(e);
      return false;
    }
    Sleep(50);
  }
  if (err) *err = "目标文件被占用，重试 5 次后仍无法替换";
  return false;
#else
  std::error_code ec; fs::rename(tmp, target, ec);
  if (ec && err) *err = ec.message();
  return !ec;
#endif
}



LoadResult loadConnections(const fs::path& file) {
  LoadResult r;
  std::error_code ec;
  if (!fs::exists(file, ec)) { r.status = LoadStatus::NotFound; return r; }
  std::string why;
  switch (parseFile(file, r.data, why)) {
    case Parse::Ok: r.status = LoadStatus::Ok; return r;
    case Parse::Newer: r.status = LoadStatus::NewerSchema; r.readOnly = true; r.detail = why; return r;
    case Parse::Bad: break;
  }
  r.detail = "主文件无法解析: " + why;
  fs::path bak = file; bak += ".bak";
  std::string why2;
  if (fs::exists(bak, ec) && parseFile(bak, r.data, why2) == Parse::Ok) {
    r.status = LoadStatus::RecoveredBackup;
    return r;
  }
  r.status = LoadStatus::Corrupt;
  r.readOnly = true;  // 不静默覆盖损坏文件，等用户选择恢复方式
  r.data = {};
  return r;
}

bool saveConnections(const fs::path& file, ConnectionsFile& data, bool readOnly, std::string* error) {
  if (readOnly) { if (error) *error = "只读（高版本或损坏），拒绝写回"; return false; }
  for (const auto& c : data.connections)
    if (auto errs = validate(c); !errs.empty()) {
      if (error) *error = "连接 " + c.id + " 校验失败: " + errs.front().message;
      return false;
    }
  json j{{"schemaVersion", kSchemaVersion}, {"revision", data.revision + 1},
         {"groups", json::array()}, {"connections", json::array()}};
  for (const auto& g : data.groups) j["groups"].push_back({{"id", g.id}, {"name", g.name}, {"order", g.order}});
  for (const auto& c : data.connections) j["connections"].push_back(connTo(c));

  fs::path tmp = file; tmp += ".tmp";
  if (!writeDurable(tmp, j.dump(2), error)) { std::error_code ec; fs::remove(tmp, ec); return false; }
  std::error_code ec;
  if (fs::exists(file, ec)) {
    // 仅当当前主文件可解析时才更新 .bak，保证备份始终是"最近一次可解析版本"
    ConnectionsFile probe; std::string why;
    if (parseFile(file, probe, why) == Parse::Ok) {
      fs::path bak = file; bak += ".bak";
      fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
    }
  }
  if (!replaceFile(tmp, file, error)) { fs::remove(tmp, ec); return false; }
  data.revision += 1;
  return true;
}

}  // namespace uicfg
