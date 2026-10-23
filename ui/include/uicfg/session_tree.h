#pragma once
// 第 12.1 / 12.3：左侧会话树模型（分组、搜索、运行标记、删除／修改连接语义）
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/store.h"
#include "uicfg/workspace.h"

namespace uicfg {

struct TreeItem {
  std::string connectionId, name, host;  // host：ssh 手动地址 / hyperv vmId / 本地可执行文件
  ConnectionKind kind = ConnectionKind::Ssh;
  int runningSessions = 0;
  bool hasRunning() const { return runningSessions > 0; }
};
struct TreeGroup {
  std::string id, name;   // id 为空：未分组（groupId 缺失或指向不存在的分组）
  std::vector<TreeItem> items;
};

// 按 Group::order、Connection::order 排序；filter 对名称与主机做不区分大小写的子串匹配，
// 过滤后空分组省略（filter 为空时保留空分组）
std::vector<TreeGroup> buildSessionTree(const ConnectionsFile& cf, const std::string& filter,
                                        const std::map<std::string, int>& runningByConnection);

// 运行中 Session 的连接快照：Session 启动时拷贝，之后编辑／删除连接不影响它
class SessionSnapshots {
 public:
  void onSessionStarted(const std::string& paneId, const Connection& c) { snaps_[paneId] = c; }
  void onSessionEnded(const std::string& paneId) { snaps_.erase(paneId); }
  const Connection* snapshot(const std::string& paneId) const;
  std::map<std::string, int> runningByConnection() const;
 private:
  std::map<std::string, Connection> snaps_;
};

// 删除连接配置：只删引用，不碰运行中 Session；返回是否删到
bool deleteConnection(ConnectionsFile& cf, const std::string& connectionId);
// 修改连接配置：替换同 id 的条目，仅影响后续新建 Session
bool updateConnection(ConnectionsFile& cf, const Connection& c);
// 恢复布局后引用缺失的占位窗格（MissingConnection / EmptyConnectionId）
std::vector<PaneBindInfo> missingReferencePanes(const WorkspaceFile& w, const ConnectionsFile& cf);

}  // namespace uicfg
