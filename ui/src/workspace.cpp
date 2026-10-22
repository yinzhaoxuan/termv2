#include "uicfg/workspace.h"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace uicfg {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

AppearanceOverride appearanceFrom(const json& j) {
  AppearanceOverride a;
  if (j.contains("fontSize") && !j.at("fontSize").is_null()) a.fontSize = j.at("fontSize").get<double>();
  if (j.contains("fontFamily") && !j.at("fontFamily").is_null()) a.fontFamily = j.at("fontFamily").get<std::string>();
  return a;
}

json appearanceTo(const AppearanceOverride& a) {
  return {{"fontSize", a.fontSize ? json(*a.fontSize) : json(nullptr)},
          {"fontFamily", a.fontFamily ? json(*a.fontFamily) : json(nullptr)}};
}

LayoutNode layoutFrom(const json& j) {
  const std::string type = j.at("type");
  if (type == "pane") {
    LayoutNode n = LayoutNode::pane(j.at("paneId"), j.value("connectionId", ""));
    if (j.contains("appearance")) n.appearance = appearanceFrom(j.at("appearance"));
    return n;
  }
  if (type == "split") {
    const std::string axis = j.at("axis");
    SplitAxis ax = SplitAxis::Columns;
    if (axis == "columns") ax = SplitAxis::Columns;
    else if (axis == "rows") ax = SplitAxis::Rows;
    else throw std::runtime_error("未知 split.axis: " + axis);
    double ratio = j.at("ratio").get<double>();
    if (!(ratio > 0.0 && ratio < 1.0)) throw std::runtime_error("split.ratio 必须在 (0,1)");
    const auto& kids = j.at("children");
    if (!kids.is_array() || kids.size() != 2) throw std::runtime_error("split 必须恰好两个子节点");
    return LayoutNode::split(ax, ratio, layoutFrom(kids[0]), layoutFrom(kids[1]));
  }
  throw std::runtime_error("未知 layout type: " + type);
}

json layoutTo(const LayoutNode& n) {
  if (n.kind == LayoutNode::Kind::Pane) {
    return {{"type", "pane"}, {"paneId", n.paneId}, {"connectionId", n.connectionId},
            {"appearance", appearanceTo(n.appearance)}};
  }
  return {{"type", "split"},
          {"axis", n.axis == SplitAxis::Columns ? "columns" : "rows"},
          {"ratio", n.ratio},
          {"children", json::array({layoutTo(*n.first), layoutTo(*n.second)})}};
}

void collectPaneIds(const LayoutNode& n, std::vector<std::string>& ids) {
  if (n.kind == LayoutNode::Kind::Pane) ids.push_back(n.paneId);
  else { collectPaneIds(*n.first, ids); collectPaneIds(*n.second, ids); }
}

enum class Parse { Ok, Newer, Bad };

Parse parseWorkspace(const fs::path& p, WorkspaceFile& out, std::string& why) {
  try {
    std::ifstream in(p, std::ios::binary);
    json j = json::parse(in);
    int v = j.at("schemaVersion");
    if (v > kSchemaVersion) { why = "schemaVersion " + std::to_string(v); return Parse::Newer; }
    if (v < 1) { why = "无效 schemaVersion"; return Parse::Bad; }
    WorkspaceFile f; f.schemaVersion = v;
    f.revision = j.value("revision", 0LL);
    f.connectionsRevision = j.value("connectionsRevision", 0LL);
    for (const auto& t : j.value("tabs", json::array())) {
      Tab tab;
      tab.title = t.value("title", "");
      tab.activePaneId = t.at("activePaneId");
      tab.root = layoutFrom(t.at("root"));
      f.tabs.push_back(std::move(tab));
    }
    out = std::move(f);
    return Parse::Ok;
  } catch (const std::exception& e) { why = e.what(); return Parse::Bad; }
}

}  // namespace

std::vector<ValidationError> validateWorkspace(const WorkspaceFile& w) {
  std::vector<ValidationError> errs;
  auto err = [&](const char* f, const char* m) { errs.push_back({f, m}); };
  for (const auto& tab : w.tabs) {
    std::vector<std::string> ids;
    collectPaneIds(tab.root, ids);
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
      err("tabs.paneId", "paneId 必须唯一");
    if (std::find(ids.begin(), ids.end(), tab.activePaneId) == ids.end())
      err("tabs.activePaneId", "活动窗格必须存在于布局树");
  }
  return errs;
}

WorkspaceLoadResult loadWorkspace(const fs::path& file) {
  WorkspaceLoadResult r;
  std::error_code ec;
  if (!fs::exists(file, ec)) { r.status = LoadStatus::NotFound; return r; }
  std::string why;
  switch (parseWorkspace(file, r.data, why)) {
    case Parse::Ok: r.status = LoadStatus::Ok; return r;
    case Parse::Newer: r.status = LoadStatus::NewerSchema; r.readOnly = true; r.detail = why; return r;
    case Parse::Bad: break;
  }
  r.detail = "主文件无法解析: " + why;
  fs::path bak = file; bak += ".bak";
  std::string why2;
  if (fs::exists(bak, ec) && parseWorkspace(bak, r.data, why2) == Parse::Ok) {
    r.status = LoadStatus::RecoveredBackup;
    return r;
  }
  r.status = LoadStatus::Corrupt;
  r.readOnly = true;
  r.data = {};
  return r;
}

bool saveWorkspace(const fs::path& file, WorkspaceFile& data, bool readOnly, std::string* error) {
  if (readOnly) { if (error) *error = "只读，拒绝写回"; return false; }
  if (auto errs = validateWorkspace(data); !errs.empty()) {
    if (error) *error = errs.front().message;
    return false;
  }
  json j{{"schemaVersion", kSchemaVersion}, {"revision", data.revision + 1},
         {"connectionsRevision", data.connectionsRevision}, {"tabs", json::array()}};
  for (const auto& t : data.tabs)
    j["tabs"].push_back({{"title", t.title}, {"activePaneId", t.activePaneId}, {"root", layoutTo(t.root)}});

  fs::path tmp = file; tmp += ".tmp";
  if (!writeDurable(tmp, j.dump(2), error)) { std::error_code ec; fs::remove(tmp, ec); return false; }
  std::error_code ec;
  if (fs::exists(file, ec)) {
    WorkspaceFile probe; std::string why;
    if (parseWorkspace(file, probe, why) == Parse::Ok) {
      fs::path bak = file; bak += ".bak";
      fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
    }
  }
  if (!replaceFile(tmp, file, error)) { fs::remove(tmp, ec); return false; }
  data.revision += 1;
  return true;
}

bool ConfigDirLock::tryAcquire(const fs::path& configDir, std::string* error) {
  if (held_) { if (error) *error = "锁已持有"; return false; }
  std::error_code ec;
  fs::create_directories(configDir, ec);
  lockPath_ = fs::weakly_canonical(configDir, ec);
  if (ec) lockPath_ = configDir;
  lockPath_ /= ".write.lock";
#ifdef _WIN32
  HANDLE h = CreateFileW(lockPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (error) *error = (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
                          ? "配置目录已有写入实例"
                          : ("加锁失败: " + std::to_string(e));
    return false;
  }
  handle_ = h; held_ = true; return true;
#else
  int fd = ::open(lockPath_.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) { if (error) *error = "无法创建锁文件"; return false; }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    if (error) *error = "配置目录已有写入实例";
    return false;
  }
  fd_ = fd; held_ = true; return true;
#endif
}

void ConfigDirLock::release() {
  if (!held_) return;
#ifdef _WIN32
  if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_ = nullptr; }
#else
  if (fd_ >= 0) { flock(fd_, LOCK_UN); ::close(fd_); fd_ = -1; }
  std::error_code ec; fs::remove(lockPath_, ec);
#endif
  held_ = false;
}

}  // namespace uicfg
