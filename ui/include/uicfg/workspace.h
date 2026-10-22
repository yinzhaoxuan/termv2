#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/model.h"
#include "uicfg/store.h"

namespace uicfg {

struct AppearanceOverride {
  std::optional<double> fontSize;
  std::optional<std::string> fontFamily;
};

enum class SplitAxis { Columns, Rows };

struct LayoutNode {
  enum class Kind { Pane, Split } kind = Kind::Pane;
  std::string paneId;
  std::string connectionId;
  AppearanceOverride appearance;
  SplitAxis axis = SplitAxis::Columns;
  double ratio = 0.5;
  std::unique_ptr<LayoutNode> first;
  std::unique_ptr<LayoutNode> second;

  static LayoutNode pane(std::string id, std::string conn) {
    LayoutNode n; n.kind = Kind::Pane; n.paneId = std::move(id); n.connectionId = std::move(conn); return n;
  }
  static LayoutNode split(SplitAxis ax, double r, LayoutNode a, LayoutNode b) {
    LayoutNode n; n.kind = Kind::Split; n.axis = ax; n.ratio = r;
    n.first = std::make_unique<LayoutNode>(std::move(a));
    n.second = std::make_unique<LayoutNode>(std::move(b));
    return n;
  }
};

struct Tab {
  std::string title;
  LayoutNode root;
  std::string activePaneId;
};

struct WorkspaceFile {
  int schemaVersion = kSchemaVersion;
  long long revision = 0;
  long long connectionsRevision = 0;
  std::vector<Tab> tabs;
};

struct WorkspaceLoadResult {
  LoadStatus status = LoadStatus::NotFound;
  WorkspaceFile data;
  bool readOnly = false;
  std::string detail;
};

WorkspaceLoadResult loadWorkspace(const std::filesystem::path& file);
bool saveWorkspace(const std::filesystem::path& file, WorkspaceFile& data,
                   bool readOnly, std::string* error);
std::vector<ValidationError> validateWorkspace(const WorkspaceFile& w);

enum class PaneBindStatus { Ok, MissingConnection, EmptyConnectionId };

struct PaneBindInfo {
  std::string paneId;
  std::string connectionId;
  PaneBindStatus status = PaneBindStatus::EmptyConnectionId;
};

std::vector<PaneBindInfo> resolvePaneBindings(const WorkspaceFile& workspace,
                                              const ConnectionsFile& connections);
bool rebindPane(WorkspaceFile& workspace, const std::string& paneId,
                const std::string& newConnectionId,
                const ConnectionsFile& connections, std::string* error);

class ConfigDirLock {
 public:
  ConfigDirLock() = default;
  ~ConfigDirLock() { release(); }
  ConfigDirLock(const ConfigDirLock&) = delete;
  ConfigDirLock& operator=(const ConfigDirLock&) = delete;
  bool tryAcquire(const std::filesystem::path& configDir, std::string* error);
  void release();
  bool held() const { return held_; }
 private:
  bool held_ = false;
  std::filesystem::path lockPath_;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace uicfg
