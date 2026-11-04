// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/known_hosts.h"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static std::string read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream text; text << in.rdbuf();
  return text.str();
}
static void lineValidation() {
  const uishell::HostKeyOffer key{"example.com", "ssh-ed25519", "cmF3", "SHA256:test"};
  CHECK(knownHostsLine(key) == "example.com ssh-ed25519 cmF3\n");
  for (const char* host : {"example.com", "[example.com]:2222", "[2001:db8::1]:2222", "hv-guid"}) {
    auto valid = key; valid.knownHostsName = host;
    CHECK(knownHostsLine(valid) == std::string(host) + " ssh-ed25519 cmF3\n");
  }
  for (const std::string& host : std::vector<std::string>{"", "two hosts", "host\t", "host\n", "host\r", "host\v", "host\f",
                                 "one,two", "@revoked", "|1|hash", "host\x7f", std::string("h\0ost", 5)}) {
    auto bad = key; bad.knownHostsName = host;
    CHECK(!knownHostsLine(bad));
  }
  for (const char* type : {"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "ssh-rsa"}) {
    auto valid = key; valid.keyType = type;
    CHECK(knownHostsLine(valid));
  }
  for (const char* type : {"", "ssh-dss", "rsa-sha2-256", "unknown", "ssh-rsa\n", "ssh-ed25519-cert-v01@openssh.com"}) {
    auto bad = key; bad.keyType = type;
    CHECK(!knownHostsLine(bad));
  }
  for (const char* encoded : {"AAAA", "Zg==", "Zm8=", "AP+/"}) {
    auto valid = key; valid.keyBase64 = encoded;
    CHECK(knownHostsLine(valid));
  }
  for (const std::string& encoded : std::vector<std::string>{"", "A", "AAA", "AAAAA", "!!!!", "AA A", "AAA\n", "=AAA", "A===", "AA=A", "====", std::string("AA\0A", 4)}) {
    auto bad = key; bad.keyBase64 = encoded;
    CHECK(!knownHostsLine(bad));
  }
}
static void appendFile() {
  const auto dir = fs::temp_directory_path() / "uicfg_known_hosts_test";
  fs::remove_all(dir);
  const auto file = dir / "ssh" / "known_hosts";
  const std::string line = "example.com ssh-ed25519 cmF3\n";
  std::string error = "old error";
  CHECK(appendKnownHost(file, line, &error));
  CHECK(error.empty() && read(file) == line);
  CHECK(!fs::exists(file.string() + ".tmp"));
  const auto written = fs::last_write_time(file);
  CHECK(appendKnownHost(file, line, &error));
  CHECK(read(file) == line && fs::last_write_time(file) == written);

  std::ofstream(file, std::ios::binary | std::ios::trunc) << "# comment\r\nother ssh-rsa AAAA";
  CHECK(appendKnownHost(file, line, &error));
  CHECK(read(file) == "# comment\r\nother ssh-rsa AAAA\n" + line);
  const std::string rsa = "example.com ssh-rsa AAAA\n";
  CHECK(appendKnownHost(file, rsa, &error));
  CHECK(read(file) == "# comment\r\nother ssh-rsa AAAA\n" + line + rsa);
  std::ofstream(file, std::ios::binary | std::ios::trunc) << line.substr(0, line.size() - 1);
  CHECK(appendKnownHost(file, line, &error));
  CHECK(read(file) == line.substr(0, line.size() - 1));
  std::ofstream(file, std::ios::binary | std::ios::trunc) << line.substr(0, line.size() - 1) + "\r\n";
  CHECK(appendKnownHost(file, line, &error));
  CHECK(read(file) == line.substr(0, line.size() - 1) + "\r\n");

  // 用非空目录阻止临时文件创建，所有平台都能稳定触发写失败。
  const auto before = read(file);
  const fs::path tmp = file.string() + ".tmp";
  fs::create_directory(tmp);
  std::ofstream(tmp / "keep") << "keep";
  CHECK(!appendKnownHost(file, rsa, &error));
  CHECK(!error.empty() && read(file) == before && read(tmp / "keep") == "keep");
  CHECK(appendKnownHost(file, line, &error)); // 重复行无需写权限或临时文件
  CHECK(error.empty() && read(file) == before);
  CHECK(!appendKnownHost(dir / "ssh", line, &error));
  CHECK(!error.empty() && read(file) == before);
  CHECK(!appendKnownHost(file, "incomplete", &error));
  CHECK(!error.empty() && read(file) == before);
  fs::remove_all(dir);
}
int main() {
  lineValidation();
  appendFile();
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
