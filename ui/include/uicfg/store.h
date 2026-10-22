// 第 13.3 节：connections.json 读写、schema 检查、原子保存与备份
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "uicfg/model.h"

namespace uicfg {

inline constexpr int kSchemaVersion = 1;

struct Group { std::string id, name; int order = 0; };

struct ConnectionsFile {
  int schemaVersion = kSchemaVersion;
  long long revision = 0;
  std::vector<Group> groups;
  std::vector<Connection> connections;
};

enum class LoadStatus {
  Ok,
  NotFound,          // 首次运行
  RecoveredBackup,   // 主文件损坏，已从 .bak 读取
  NewerSchema,       // 高版本：只读，禁止写回
  Corrupt            // 主文件和备份都无法解析：展示恢复选项
};

struct LoadResult {
  LoadStatus status = LoadStatus::NotFound;
  ConnectionsFile data;
  bool readOnly = false;
  std::string detail;
};

LoadResult loadConnections(const std::filesystem::path& file);

// 同目录临时文件 + 替换；替换前把旧文件留作 .bak。失败时原文件不变。
// readOnly 的数据拒绝保存。成功后 revision 自增。
bool saveConnections(const std::filesystem::path& file, ConnectionsFile& data,
                     bool readOnly, std::string* error);

}  // namespace uicfg
