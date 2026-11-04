// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <fstream>
#include "uicfg/snapshot_cleanup.h"
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  fs::path dir = fs::temp_directory_path() / "uicfg_snapshot_cleanup_test";
  fs::remove_all(dir); fs::create_directories(dir);
  const auto snapshots = dir / "snapshots";
  fs::create_directory(snapshots);
  for (const char* name : {"ssh-1-1.conf", "ssh-12-3-4.conf", "ssh-a-1.conf", "known_hosts",
                           "ssh-1.conf", "ssh-1-2-3-4.conf", "ssh--1.conf", "ssh-1-.conf",
                           "ssh-1-1.conf.bak", "other-ssh-1-1.conf", "ssh-9-9.CONF",
                           "ssh-.conf", "ssh-1--2.conf", "ssh-1-2-.conf", "ssh-1-2x.conf"})
    std::ofstream(snapshots / name) << "保留内容";
  const fs::path nonAscii = snapshots / fs::path(u8"ssh-１-2.conf");
  std::ofstream(nonAscii) << "保留内容";
  auto result = cleanupStaleSnapshots(snapshots);
  CHECK(result.removed == 2 && result.errors.empty());
  CHECK(!fs::exists(snapshots / "ssh-1-1.conf"));
  CHECK(!fs::exists(snapshots / "ssh-12-3-4.conf"));
  for (const char* name : {"ssh-a-1.conf", "known_hosts", "ssh-1.conf", "ssh-1-2-3-4.conf",
                           "ssh--1.conf", "ssh-1-.conf", "ssh-1-1.conf.bak", "other-ssh-1-1.conf", "ssh-9-9.CONF",
                           "ssh-.conf", "ssh-1--2.conf", "ssh-1-2-.conf", "ssh-1-2x.conf"})
    CHECK(fs::is_regular_file(snapshots / name));
  CHECK(fs::is_regular_file(nonAscii));

  // 同名子目录及其中的快照不动；符号链接也不作为普通文件删除。
  fs::create_directory(snapshots / "ssh-1-1.conf");
  std::ofstream(snapshots / "ssh-1-1.conf" / "ssh-2-2.conf") << "嵌套内容";
  std::error_code ec;
  fs::create_symlink("known_hosts", snapshots / "ssh-5-6.conf", ec);
  const bool hasSymlink = !ec;  // Windows 创建符号链接可能需要额外权限
  result = cleanupStaleSnapshots(snapshots);
  CHECK(result.removed == 0 && result.errors.empty());
  CHECK(fs::is_directory(snapshots / "ssh-1-1.conf"));
  CHECK(fs::is_regular_file(snapshots / "ssh-1-1.conf" / "ssh-2-2.conf"));
  if (hasSymlink) CHECK(fs::is_symlink(snapshots / "ssh-5-6.conf"));

  result = cleanupStaleSnapshots(dir / "missing");
  CHECK(result.removed == 0 && result.errors.empty());
  result = cleanupStaleSnapshots(snapshots / "known_hosts");
  CHECK(result.removed == 0 && !result.errors.empty());

  fs::remove_all(dir);
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
