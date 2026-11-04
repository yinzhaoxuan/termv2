// DS-15：按规则登记；所有样本均为虚构值。
#include "uicfg/diag_bundle.h"
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace uicfg;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static std::string redact(const std::string& s) { return redactDiagnosticText(s, {}); }
static bool contains(const std::string& s, const std::string& value) { return s.find(value) != std::string::npos; }
static const std::string kGuid = "12345678-abcd-1234-abcd-123456789abc";
static const std::string kFingerprint = "SHA256:" + std::string(42, 'A') + "B";

static void known() {
  const RedactionContext ctx{{{"user", "root"}, {"path", "C:\\Users\\root\\keys"},
      {"host", "long-server.example"}, {"host", "lab"}, {"user", "ab"}, {"host", "x"}}};
  CHECK(redactDiagnosticText("c:/USERS/ROOT/keys root ROOT rootfs _root root_ root2 2root", ctx) ==
      "<path-1> <user-1> <user-1> rootfs _root root_ root2 2root");
  CHECK(redactDiagnosticText("lab LONG-SERVER.EXAMPLE Lab long-server.example", ctx) ==
      "<host-1> <host-2> <host-1> <host-2>");
  CHECK(redactDiagnosticText("ab x abcd prefixlab labs lab", ctx) == "ab x abcd prefixlab labs <host-1>");
  CHECK(redactDiagnosticText("root@lab c:/users/root/keys C:\\Users\\root\\keys", ctx) ==
      "<user-1>@<host-1> <path-1> <path-1>");
  CHECK(redactDiagnosticText("C:/USERS/ROOT/keys/file", ctx) == "<path-1>/file");
  CHECK(redactDiagnosticText("lab root", ctx) == "<host-1> <user-1>");
  // 记号不再参与扫描，类别名恰好是已知值时也不损坏。
  CHECK(redactDiagnosticText("server host user", {{{"host", "server"}, {"user", "host"}, {"host", "user"}}}) ==
      "<host-1> <user-1> <host-2>");
  CHECK(redactDiagnosticText("192.0.2.9", {{{"host", "ab"}}}) == "<ipv4-1>");
  CHECK(redactDiagnosticText("王小 王小明", {{{"user", "王小"}, {"user", "王小明"}}}) == "王小 <user-1>");
  CHECK(redactDiagnosticText(R"(\private\keys /PRIVATE/keys)", {{{"path", "/private/keys"}}}) == "<path-1> <path-1>");
  CHECK(redactDiagnosticText(R"(/private/keys \PRIVATE\keys)", {{{"path", R"(\private\keys)"}}}) == "<path-1> <path-1>");
  CHECK(redactDiagnosticText("", ctx).empty());
}
static void fingerprint() {
  const auto other = "SHA256:" + std::string(43, 'Z');
  CHECK(redact(kFingerprint + " " + other + " " + kFingerprint) == "<fingerprint-1> <fingerprint-2> <fingerprint-1>");
  CHECK(redact(kFingerprint + "=") == "<fingerprint-1>");
  for (const auto& value : {"SHA256:" + std::string(42, 'A'), "SHA256:" + std::string(44, 'A'),
                            kFingerprint + "==", "sha256:" + std::string(43, 'A')}) CHECK(redact(value) == value);
  const auto alphabet = "SHA256:" + std::string(41, 'A') + "+/=";
  CHECK(redact(alphabet) == "<fingerprint-1>");
}
static void guid() {
  CHECK(redact(kGuid + " {" + kGuid + "} hv-" + kGuid + " hv-{" + kGuid + "}") ==
      "<guid-1> <guid-2> <guid-3> <guid-4>");
  CHECK(redact("hv-" + kGuid + " hv-" + kGuid) == "<guid-1> <guid-1>");
  for (const auto& value : std::vector<std::string>{"x" + kGuid, kGuid + "0", "1234567-abcd-1234-abcd-123456789abc",
                           "12345678-zzzz-1234-abcd-123456789abc"}) CHECK(redact(value) == value);
}
static void ipv4() {
  CHECK(redact("192.0.2.1 198.51.100.255 192.0.2.1") == "<ipv4-1> <ipv4-2> <ipv4-1>");
  CHECK(redact("[203.0.113.1]:22") == "[<ipv4-1>]:22");
  for (const char* value : {"127.0.0.1", "0.0.0.0", "999.1.1.1", "1.2.3.256", "1.2.3", "1.2.3.4.5", "1234.1.2.3"}) CHECK(redact(value) == value);
}
static void ipv6() {
  CHECK(redact("2001:db8::1234 [fe80::abcd%eth0] :: 2001:db8::1234") ==
      "<ipv6-1> <ipv6-2> <ipv6-3> <ipv6-1>");
  CHECK(redact("[2001:DB8::abcd]:22 fe80::1%12") == "<ipv6-1>:22 <ipv6-2>");
  for (const char* value : {"a:b:c", "1:2:3:4", "1:2:3:4:5:6:7:8"}) CHECK(redact(value) == "<ipv6-1>");
  for (const char* value : {"::1", "[::1]", "[::1%lo]", "12:34:56", "elapsed_ms=12:34", "2026-11-04 12:34:56",
      "2001:db8:::1", "2001::db8::1", "12345::1", "2001:db8::g", "[fe80::1%]", "[2001:db8::1", "1:2:3:4:5:6:7:8:9"}) CHECK(redact(value) == value);
}
static void pipe() {
  CHECK(redact(R"(agent \\.\pipe\private-agent \\.\pipe\another-agent \\.\pipe\private-agent)") ==
      R"(agent \\.\pipe\<pipe-1> \\.\pipe\<pipe-2> \\.\pipe\<pipe-1>)");
  CHECK(redact(R"(agent="\\.\pipe\private-agent")") == R"(agent="\\.\pipe\<pipe-1>")");
  for (const char* value : {R"(\\.\pipe\openssh-ssh-agent)", R"(\\.\PIPE\OPENSSH-SSH-AGENT)", R"(\\.\pipe\)", R"(\.\pipe\agent)"}) CHECK(redact(value) == value);
}
static void home() {
  CHECK(redact(R"(C:\Users\Alice\keys D:/uSeRs/Bob/file)") == R"(C:\Users\<user-home>\keys D:\Users\<user-home>\file)");
  for (const char* value : {R"(C:\Users\Public\file)", R"(C:/Users/DEFAULT/file)", R"(C:\Users\Alice)", R"(C:\User\Alice\file)"}) CHECK(redact(value) == value);
  CHECK(redact(R"(C:\Users\Alice Smith\file)") == R"(C:\Users\<user-home>\file)");
  CHECK(redact(R"(Z:\Users\测试\file)") == R"(Z:\Users\<user-home>\file)");
}
static ConnectionsFile connections() {
  ConnectionsFile file; file.revision = 7;
  Connection c; c.id = "connection-secret"; c.name = "name-secret";
  c.groupId = "group-secret"; c.appearanceProfileId = "profile-secret";
  c.endpoint = Endpoint{EndpointMode::Manual, "server-secret.example", 22, std::nullopt};
  c.ssh = SshSettings{}; c.ssh->user = "user-secret"; c.ssh->identityFile = "C:/keys/key-secret";
  file.connections.push_back(c);
  c.id = "hv-secret"; c.endpoint->mode = EndpointMode::HyperV; c.endpoint->host.reset();
  c.endpoint->hyperv = HyperVEndpoint{}; c.endpoint->hyperv->vmId = "{" + kGuid + "}";
  c.endpoint->hyperv->fallbackHost = "fallback-secret.example";
  c.ssh->transport = "compatible"; file.connections.push_back(c);
  c.id = "direct-secret"; c.ssh->transport = "direct"; file.connections.push_back(c);
  c.id = "local-secret"; c.kind = ConnectionKind::Local; c.endpoint.reset(); c.ssh.reset();
  c.local = LocalSettings{}; c.local->executable = "C:/bin/exe-secret"; c.local->workingDirectory = "C:/work/directory-secret";
  file.connections.push_back(c);
  return file;
}
static void context() {
  const auto file = connections();
  const auto ctx = redactionContextFrom(file, {{"user", "Windows-secret"}, {"path", "C:/Users/Windows-secret"},
      {"path", "C:/config-secret"}, {"host", "computer-secret"}});
  for (const auto& [category, value] : ctx.known) {
    CHECK(value.size() >= 3);
    CHECK(!contains(redactDiagnosticText(value, ctx), value));
    CHECK(contains(redactDiagnosticText(value, ctx), "<" + category + "-"));
  }
  CHECK(redactDiagnosticText("hv-" + kGuid, ctx) == "<vm-1>");
  CHECK(redactDiagnosticText("hv-{" + kGuid + "}", ctx) == "<vm-1>");
  CHECK(redactDiagnosticText("{" + kGuid + "}", ctx) == "<vm-1>");
}
static void write(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }
static std::string read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static void logs() {
  const auto dir = fs::temp_directory_path() / "uicfg_diag_logs_test";
  fs::remove_all(dir); fs::create_directories(dir);
  auto sections = readDiagLogs(dir, 12);
  CHECK(sections.size() == 3);
  CHECK(sections[0].name == "pane-diag.log.2" && sections[1].name == "pane-diag.log.1" && sections[2].name == "pane-diag.log");
  for (const auto& section : sections) CHECK(!section.text && section.note == "缺失");
  write(dir / "pane-diag.log.2", "old\r\nline\n");
  write(dir / "pane-diag.log.1", "0123456789partial\r\nlast\r\n");
  write(dir / "pane-diag.log", "new\n");
  sections = readDiagLogs(dir, 12);
  CHECK(sections[0].text == "old\nline\n" && sections[0].note == "10 字节");
  CHECK(sections[1].text == "last\n" && contains(sections[1].note, "截断：只保留最后 12 字节"));
  CHECK(sections[2].text == "new\n");
  write(dir / "pane-diag.log", "0123456789ab\nlast\n");
  CHECK(readDiagLogs(dir, 6)[2].text == "last\n");
  // 即使截断点恰好在行首，仍按约定跳过第一行。
  write(dir / "pane-diag.log", "prefix\nfirst\nlast\n");
  CHECK(readDiagLogs(dir, 11)[2].text == "last\n");
  write(dir / "pane-diag.log", "long-line-without-newline");
  CHECK(readDiagLogs(dir, 5)[2].text == "");
  CHECK(readDiagLogs(dir, 0)[2].text == "");
  const std::string bytes = std::string("a\rb\r\n") + char(0xff) + '\0';
  write(dir / "pane-diag.log", bytes);
  CHECK(readDiagLogs(dir)[2].text == std::string("a\rb\n") + char(0xff) + '\0');
  write(dir / "pane-diag.log", "");
  CHECK(readDiagLogs(dir)[2].text == "");
  fs::remove(dir / "pane-diag.log"); fs::create_directory(dir / "pane-diag.log");
  sections = readDiagLogs(dir);
  CHECK(!sections[2].text && contains(sections[2].note, "读取失败："));
  fs::remove_all(dir);
}
static std::string joined(const std::vector<std::string>& lines) {
  std::string out; for (const auto& line : lines) out += line + '\n'; return out;
}
static void summary() {
  auto conns = connections();
  auto settings = defaultSettings();
  settings.osc52 = Osc52Settings{true, false, 123};
  settings.appearanceProfiles["profile-secret"] = AppearanceProfile{};
  settings.appearanceProfiles["bad-secret"].invalidField = "bad-field-secret";
  settings.keybindings["pasteCtrlV"] = ""; settings.keybindings["copyOrInterrupt"] = "";
  settings.keybindings["unknown-action-secret"] = "secret-chord";
  FileValidation validation; validation.byConnection["connection-secret"].push_back({"host", "error-secret"});
  const auto text = joined(configSummaryLines(conns, LoadStatus::Ok, validation, settings, LoadStatus::RecoveredBackup));
  CHECK(contains(text, "已读取，revision 7，4 个连接（SSH 手动 1 / Hyper-V 2 / 本地 1），校验未通过 1 个"));
  CHECK(contains(text, "传输 = 跟随默认 1 / 兼容 1 / 直连 1"));
  CHECK(contains(text, "settings.json = 已从备份恢复；字体 Cascadia Mono 12；osc52 = 写开 / 读关 / 123；confirmMultilinePaste = 默认"));
  CHECK(contains(text, "外观 Profile = 2 个（无效 1）"));
  CHECK(contains(text, "改过的快捷键 = copyOrInterrupt, pasteCtrlV"));
  for (const auto& [category, value] : redactionContextFrom(conns, {}).known) { (void)category; CHECK(!contains(text, value)); }
  for (const char* value : {"connection-secret", "name-secret", "group-secret", "profile-secret", "bad-secret", "bad-field-secret", "error-secret", "unknown-action-secret", "secret-chord"}) CHECK(!contains(text, value));
  conns.connections[0].ssh->transport = "transport-secret";
  settings.osc52Raw = "\"raw-secret\""; settings.osc52 = Osc52Settings{};
  settings.confirmMultilinePasteRaw = "\"paste-secret\"";
  settings.appearanceProfilesRaw = "\"profiles-secret\"";
  auto invalid = joined(configSummaryLines(conns, LoadStatus::NewerSchema, {}, settings, LoadStatus::Corrupt));
  CHECK(contains(invalid, "高版本（只读）") && contains(invalid, "损坏（使用默认值）"));
  CHECK(contains(invalid, "无效，按全关") && contains(invalid, "confirmMultilinePaste = 无效（默认）") && contains(invalid, " / 无效 1"));
  CHECK(!contains(invalid, "secret"));
  for (bool on : {true, false}) {
    settings.confirmMultilinePasteRaw.reset(); settings.confirmMultilinePaste = on;
    CHECK(contains(joined(configSummaryLines({}, LoadStatus::NotFound, {}, settings, LoadStatus::Ok)),
        std::string("confirmMultilinePaste = ") + (on ? "开" : "关")));
  }
}
static void bundle() {
  auto conns = connections();
  DiagEnvironment env{"2026-11-04 21:03:11 +08:00", "0b72f82", "Release / MinGW / Direct 已编译", "10.0.22631",
      {{"ITERM2WIN_SSH_DIRECT", "1"}, {"ITERM2WIN_OSC52_PING", "(未设置)"}}};
  const std::string log = "v=1 sid=3 attempt=1 phase=ssh.direct.hostkey elapsed_ms=12 err=0 bytes=0 msg=ssh-ed25519 " + kFingerprint +
      "\nv=1 sid=3 attempt=1 phase=ssh.direct.agent elapsed_ms=13 err=0 bytes=0 msg=agent=\\\\.\\pipe\\private-agent\n"
      "v=1 sid=3 attempt=1 phase=session.end elapsed_ms=14 err=1 bytes=0 msg=user-secret@server-secret.example 192.0.2.10 [fe80::abcd%3] hv-" + kGuid + " C:\\Users\\Unknown\\work\\";
  const auto output = buildDiagnosticBundle(env, configSummaryLines(conns, LoadStatus::Ok, {}, defaultSettings(), LoadStatus::Ok),
      {{"pane-diag.log.2", std::nullopt, "缺失"}, {"pane-diag.log.1", log, "100 字节"}, {"pane-diag.log", log, "截断"}}, redactionContextFrom(conns, {}));
  for (const auto& value : {kFingerprint, kGuid, std::string("private-agent"), std::string("user-secret"), std::string("server-secret.example"), std::string("192.0.2.10"), std::string("fe80::abcd"), std::string("Unknown")}) CHECK(!contains(output, value));
  for (const char* value : {"# it2w 诊断包", "[构建与环境]", "提交（配置时） = 0b72f82", "生成时间 = 2026-11-04 21:03:11 +08:00", "Windows = 10.0.22631", "[配置摘要]", "[日志 pane-diag.log.2]（缺失）", "[日志 pane-diag.log.1]", "[日志 pane-diag.log]", "<fingerprint-1>", "<pipe-1>", "<host-1>", "<user-1>", "<ipv4-1>", "<ipv6-1>", "<vm-1>", "<user-home>", "phase=session.end"}) CHECK(contains(output, value));
  // 连环境字段、节标题和摘要都走脱敏。
  CHECK(!contains(buildDiagnosticBundle({"", "", "server-secret.example", "", {}}, {"user-secret"},
      {{"server-secret.example", std::nullopt, "缺失"}}, redactionContextFrom(conns, {})), "secret"));
}
static void large() {
  const auto dir = fs::temp_directory_path() / "uicfg_diag_large_test";
  fs::remove_all(dir); fs::create_directories(dir);
  const std::string line = "v=1 sid=3 attempt=1 phase=session.end elapsed_ms=12 err=1 bytes=0 msg=large-secret-user@large-secret.example 192.0.2.10 \\PRIVATE\\large-secret\\work connection closed\n";
  std::string text;
  while (text.size() <= kDiagLogTailBytes) text += line;
  for (const char* name : {"pane-diag.log.2", "pane-diag.log.1", "pane-diag.log"}) write(dir / name, text);
  const auto sections = readDiagLogs(dir);
  for (const auto& section : sections) {
    CHECK(section.text && section.text->size() <= kDiagLogTailBytes);
    CHECK(contains(section.note, "截断"));
    if (section.text) {
      CHECK(section.text->starts_with("v=1 sid="));
      CHECK(section.text->ends_with("connection closed\n"));
    }
  }
  RedactionContext ctx{{{"host", "large-secret.example"}, {"user", "large-secret-user"},
      {"path", "/private/large-secret/work"}}};
  // 模拟约二十个连接的已知值；额外值均不出现在日志中。
  for (int i = 0; i < 19; ++i) {
    const auto suffix = std::to_string(i);
    ctx.known.emplace_back("host", "unused-host-" + suffix + ".example");
    ctx.known.emplace_back("user", "unused-user-" + suffix);
    ctx.known.emplace_back("path", "C:/unused-path-" + suffix + "/keys");
  }
  CHECK(ctx.known.size() == 60);
  const auto output = buildDiagnosticBundle({}, {}, sections, ctx);
  CHECK(output.size() > kDiagLogTailBytes);
  CHECK(!contains(output, "secret") && !contains(output, "192.0.2.10"));
  CHECK(contains(output, "<host-1>") && !contains(output, "<host-2>"));
  CHECK(contains(output, "<user-1>") && !contains(output, "<user-2>"));
  CHECK(contains(output, "<path-1>") && !contains(output, "<path-2>"));
  CHECK(contains(output, "<ipv4-1>") && !contains(output, "<ipv4-2>"));
  fs::remove_all(dir);
}
static void filename() {
  CHECK(diagnosticFileName("20261104-210311") == "it2w-diag-20261104-210311.txt");
}
static void save() {
  const auto dir = fs::temp_directory_path() / "uicfg_diag_save_test";
  fs::remove_all(dir); fs::create_directories(dir);
  const auto file = dir / "诊断.txt";
  const std::string bytes = "# it2w 诊断包\n<host-1>\n";
  std::string error;
  CHECK(saveDiagnosticBundle(file, bytes, &error));
  CHECK(read(file) == bytes && !fs::exists(dir / "诊断.txt.tmp"));
  CHECK(saveDiagnosticBundle(file, "替换\n", &error) && read(file) == "替换\n");
  CHECK(!saveDiagnosticBundle(dir / "missing" / "diag.txt", bytes, &error) && !error.empty());
  const auto target = dir / "occupied"; fs::create_directory(target); write(target / "keep", "keep");
  CHECK(!saveDiagnosticBundle(target, bytes, &error));
  CHECK(read(target / "keep") == "keep" && !fs::exists(dir / "occupied.tmp"));
  fs::remove_all(dir);
}
int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  const std::pair<const char*, void(*)()> cases[] = {{"known", known}, {"fingerprint", fingerprint}, {"guid", guid},
      {"ipv4", ipv4}, {"ipv6", ipv6}, {"pipe", pipe}, {"home", home}, {"context", context}, {"logs", logs},
      {"summary", summary}, {"bundle", bundle}, {"large", large}, {"filename", filename}, {"save", save}};
  for (const auto& [name, run] : cases) if (mode == "all" || mode == name) run();
  return fails != 0;
}
