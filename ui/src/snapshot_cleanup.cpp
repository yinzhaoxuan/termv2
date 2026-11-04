// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/snapshot_cleanup.h"
#include <algorithm>
#include <string_view>

namespace uicfg {
namespace fs = std::filesystem;
namespace {

bool allDigits(std::string_view part) {
  return !part.empty() && std::all_of(part.begin(), part.end(),
                                     [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool isSnapshotName(const fs::path& leaf) {
  std::string name;
  for (auto ch : leaf.native()) {
    if (static_cast<unsigned int>(ch) > 0x7f) return false;
    name.push_back(static_cast<char>(ch));
  }
  constexpr std::string_view prefix = "ssh-", suffix = ".conf";
  if (!name.starts_with(prefix) || !name.ends_with(suffix)) return false;
  std::string_view middle(name);
  middle.remove_prefix(prefix.size());
  middle.remove_suffix(suffix.size());
  int parts = 0;
  while (true) {
    auto dash = middle.find('-');
    if (!allDigits(middle.substr(0, dash))) return false;
    ++parts;
    if (dash == std::string_view::npos) break;
    middle.remove_prefix(dash + 1);
  }
  return parts == 2 || parts == 3;
}

}  // namespace

CleanupResult cleanupStaleSnapshots(const fs::path& snapshotDir) {
  CleanupResult result;
  std::error_code ec;
  fs::directory_iterator it(snapshotDir, ec), end;
  if (ec) {
    if (ec != std::errc::no_such_file_or_directory)
      result.errors.push_back("读取快照目录失败：" + ec.message());
    return result;
  }
  while (it != end) {
    if (isSnapshotName(it->path().filename())) {
      // 不跟随符号链接，也不进入子目录。
      const auto status = it->symlink_status(ec);
      if (ec) result.errors.push_back("读取快照文件状态失败：" + ec.message());
      else if (fs::is_regular_file(status)) {
        if (fs::remove(it->path(), ec)) ++result.removed;
        if (ec) result.errors.push_back("删除快照失败：" + it->path().filename().string() + "：" + ec.message());
      }
    }
    it.increment(ec);
    if (ec) { result.errors.push_back("遍历快照目录失败：" + ec.message()); break; }
  }
  return result;
}

}  // namespace uicfg
