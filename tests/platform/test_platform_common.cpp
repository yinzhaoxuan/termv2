// SSH-CFG-01（生成器部分）、IO-01（调度逻辑部分）的 Linux 单测。
#include "io_queues.h"
#include "ssh_snapshot.h"
#include <cstdio>
#include <string>

using namespace it2w;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static bool has(const std::string &s, const std::string &line) { return s.find("\n  " + line + "\n") != std::string::npos; }

static SshTarget base() {
    SshTarget t; t.hostName = "192.0.2.10"; t.user = "tester";
    t.appKnownHostsFile = "C:\\Users\\测试 用户\\AppData\\it2w\\app_known_hosts";
    t.emptyKnownHostsFile = "C:/ProgramData/it2w/empty_known_hosts";
    return t;
}

static void test_snapshot() {
    auto r = buildSshSnapshot(base());
    CHECK(r.text && !r.error);
    const std::string &s = *r.text;
    CHECK(s.find("\nHost it2w-target\n") != std::string::npos);
    for (auto l : {"HostName 192.0.2.10", "Port 22", "User tester", "StrictHostKeyChecking ask",
                   "UserKnownHostsFile \"C:/Users/测试 用户/AppData/it2w/app_known_hosts\"",
                   "GlobalKnownHostsFile \"C:/ProgramData/it2w/empty_known_hosts\"", "VerifyHostKeyDNS no",
                   "UpdateHostKeys no", "RequestTTY force", "ConnectTimeout 10", "BatchMode no", "ForwardAgent no",
                   "ForwardX11 no", "PermitLocalCommand no", "ClearAllForwardings yes",
                   "PreferredAuthentications publickey", "PasswordAuthentication no",
                   "KbdInteractiveAuthentication no", "IdentityFile none", "IdentitiesOnly no"})
        CHECK(has(s, l));
    for (auto bad : {"ProxyCommand", "ProxyJump", "\n  LocalCommand", "RemoteCommand", "ControlMaster", "\r", "\xef\xbb\xbf"})
        CHECK(s.find(bad) == std::string::npos);

    auto t = base(); t.auth = SshAuthMode::KeyFile; t.keyFile = "D:\\keys\\id_ed25519";
    r = buildSshSnapshot(t);
    CHECK(r.text && has(*r.text, "IdentityFile \"D:/keys/id_ed25519\"") && has(*r.text, "IdentitiesOnly yes") &&
          has(*r.text, "IdentityAgent none"));
    t.keyFile = "";
    CHECK(buildSshSnapshot(t).error && buildSshSnapshot(t).error->field == "keyFile");

    t = base(); t.auth = SshAuthMode::Interactive;
    r = buildSshSnapshot(t);
    CHECK(r.text && has(*r.text, "PubkeyAuthentication no") &&
          has(*r.text, "PreferredAuthentications keyboard-interactive,password") &&
          r.text->find("IdentityFile") == std::string::npos);

    t = base(); t.hostName = "2001:db8::10";
    CHECK(buildSshSnapshot(t).text && has(*buildSshSnapshot(t).text, "HostName 2001:db8::10"));
    // 拒绝项
    struct { const char *path; } bads[] = {{"C:/a\"b/key"}, {"C:/%d/key"}, {"~/.ssh/key"}, {"C:/a\nb"}, {"relative/key"}, {""}};
    for (auto &b : bads) { t = base(); t.appKnownHostsFile = b.path; CHECK(buildSshSnapshot(t).error); }
    t = base(); t.hostName = "evil.invalid\n  ProxyCommand calc"; CHECK(buildSshSnapshot(t).error);
    t = base(); t.hostName = "-oProxyCommand=x"; CHECK(buildSshSnapshot(t).error);
    t = base(); t.user = "a b"; CHECK(buildSshSnapshot(t).error);
    t = base(); t.alias = "*"; CHECK(buildSshSnapshot(t).error);
    t = base(); t.port = 0; CHECK(buildSshSnapshot(t).error);
    CHECK(quoteConfigPath("\\\\server\\share\\k").value() == "\"//server/share/k\"");
}

static void test_cmdline() {
    std::vector<std::string> args = {"C:/Windows/System32/OpenSSH/ssh.exe", "-F", "C:/Temp/it2w cfg/snap-1.conf",
                                     "a\"b", "tr\\ail\\", "x\\\\\"y", "", "\t", "中文 参数"};
    auto cmd = buildWindowsCommandLine(args);
    auto back = parseWindowsCommandLine(cmd);
    CHECK(back == args);
    CHECK(quoteWindowsArg("plain") == "plain");
    CHECK(quoteWindowsArg("a b\\") == "\"a b\\\\\"");
    auto ssh = sshArguments("C:/ssh.exe", "C:/s.conf", "it2w-target");
    CHECK(ssh.size() == 4 && ssh[1] == "-F");
}

static void test_env() {
    EnvList in = {{"PATH", "x"}, {"ssh_askpass", "calc.exe"}, {"SSH_ASKPASS_REQUIRE", "force"}, {"Home", "h"}, {"A=B", "1"}};
    auto out = filterChildEnvironment(in);
    CHECK(out.size() == 2 && out[0].first == "Home" && out[1].first == "PATH");
}

static void test_input() {
    InputScheduler s;
    // I07：中文跨 4 KiB 边界不切开
    std::string zh; while (zh.size() < 9000) zh += "中";
    CHECK(s.enqueueUser(zh) == EnqueueResult::Ok);
    std::string got; int units = 0;
    while (true) {
        auto v = s.peekWrite(); if (v.empty()) break;
        CHECK(v.size() <= kTextUnitMax && v.size() % 3 == 0);
        got += v; ++units; s.consumed(v.size(), 1);
    }
    CHECK(got == zh && units == 3 && s.userBytes() == 0);

    // 应答优先于未开始的普通输入；已开始的单元必须写完
    s.enqueueUser("hello");
    auto v = s.peekWrite(); s.consumed(2, 2);
    s.enqueueReply("\x1b[0n");
    v = s.peekWrite(); CHECK(v == "llo"); s.consumed(3, 3);
    s.enqueueUser("next");
    v = s.peekWrite(); CHECK(v == "\x1b[0n"); s.consumed(v.size(), 4);
    v = s.peekWrite(); CHECK(v == "next"); s.consumed(v.size(), 5);

    // I01/I02：粘贴事务不可交错
    std::string body(kPasteMax, 'p');
    CHECK(s.beginPaste(kPasteMax) == EnqueueResult::Ok);
    v = s.peekWrite(); CHECK(v == "\x1b[200~"); s.consumed(v.size(), 6);
    s.enqueueReply("R");
    CHECK(s.peekWrite().empty());  // 正文未到，应答也不能插入
    CHECK(!s.stalled(100000));      // 等待正文不算停滞
    s.appendPaste(std::string_view(body).substr(0, 1000));
    v = s.peekWrite(); CHECK(v.size() == 1000); s.consumed(v.size(), 7);
    s.appendPaste(std::string_view(body).substr(1000));
    std::string tx; while ((v = s.peekWrite()) != "R") { tx += v; s.consumed(v.size(), 8); }
    CHECK(tx == body.substr(1000) + "\x1b[201~");
    s.consumed(1, 9);
    // I03：超 32 KiB 拒绝
    CHECK(s.beginPaste(kPasteMax + 1) == EnqueueResult::TooLarge);
    // I08：取消仍补结束标记
    s.beginPaste(100); s.appendPaste("abc"); s.cancelPasteBody();
    tx.clear(); while (!(v = s.peekWrite()).empty()) { tx += v; s.consumed(v.size(), 10); }
    CHECK(tx == "\x1b[200~abc\x1b[201~");

    // I04/I05：容量与停滞
    InputScheduler q;
    std::string big(kUserQueueCap, 'x');
    CHECK(q.enqueueUser(big) == EnqueueResult::Ok);
    CHECK(q.enqueueUser("y") == EnqueueResult::QueueFull);
    q.notePending(1000);
    CHECK(!q.stalled(5999) && q.stalled(6000));
    q.peekWrite(); q.consumed(10, 6000); CHECK(!q.stalled(10999) && q.stalled(11000));
    std::string rep(1024, 'r'); int ok = 0;
    while (q.enqueueReply(rep) == EnqueueResult::Ok) ++ok;
    CHECK(ok == 64);
    q.close(); CHECK(q.enqueueUser("z") == EnqueueResult::InvalidState);
}

static void test_output() {
    OutputQueue q; std::string chunk(64 << 10, 'o');
    int pushes = 0; OutputQueue::PushResult r;
    while ((r = q.push(chunk)) == OutputQueue::PushResult::Ok) ++pushes;
    CHECK(r == OutputQueue::PushResult::PauseReading && q.bytes() == kOutputPause && q.readingPaused());
    while (!q.shouldResume()) CHECK(q.pop());
    CHECK(q.bytes() <= kOutputResume); q.resumed();
    CHECK(q.peak() <= kOutputCap && !q.incomplete());
    q.beginClosing();
    uint64_t pushed = 0; for (int i = 0; i < 100; ++i) { q.push(chunk); pushed += chunk.size(); }
    CHECK(q.incomplete() && q.discardedBytes() == pushed - kCloseDrainExtra);
}

int main() {
    test_snapshot(); test_cmdline(); test_env(); test_input(); test_output();
    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
