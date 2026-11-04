// SPDX-License-Identifier: GPL-3.0-or-later
// 启动时清理残留 SSH 快照
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace uicfg {

struct CleanupResult { int removed = 0; std::vector<std::string> errors; };

// 持有配置目录锁、尚未启动会话时调用；只删目录第一层中匹配旧/新命名的普通快照文件。
CleanupResult cleanupStaleSnapshots(const std::filesystem::path& snapshotDir);

}  // namespace uicfg
