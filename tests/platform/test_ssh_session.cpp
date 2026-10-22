// T07 Linux 侧：ssh -G 解析/快照生效值校验、退出码与错误映射；可选的本机 sshd 实连（127.0.0.1:22422/22423）。
// 实连只用 ssh/ssh-keyscan，不读取或输出任何密码/口令文件内容。
#include "ssh_session.h"
#include "ssh_snapshot.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
using namespace it2w;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static int run(const std::string &cmd, std::string *out) {
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[4096]; size_t n; out->clear();
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out->append(buf, n);
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static std::string tmpdir;
static SshTarget target(uint16_t port, const std::string &user, SshAuthMode m, const std::string &key = {}) {
    SshTarget t; t.hostName = "127.0.0.1"; t.port = port; t.user = user; t.auth = m; t.keyFile = key;
    t.connectTimeoutSec = 5;
    t.appKnownHostsFile = "/" + tmpdir + "/known_hosts";       // quoteConfigPath 要求 X:/ 或 //，POSIX 下用 // 前缀
    t.emptyKnownHostsFile = "/" + tmpdir + "/empty_known_hosts";
    return t;
}
static std::string writeSnap(const SshTarget &t, const char *name) {
    auto r = buildSshSnapshot(t);
    if (!r.text) { std::printf("snapshot error %s %s\n", r.error->field.c_str(), r.error->reason.c_str()); return {}; }
    std::string p = tmpdir + "/" + name;
    std::ofstream(p) << *r.text;
    return p;
}

static void unit() {
    auto m = parseSshG("user it2wtest\nhostname 127.0.0.1\nidentityfile none\nIdentityFile ~/.ssh/id_rsa\r\nport 22\n");
    CHECK(sshGFirst(m, "User") == "it2wtest" && m["identityfile"].size() == 2 && sshGFirst(m, "port") == "22");
    CHECK(sshGFirst(m, "missing").empty());

    CHECK(mapSshExit(0, "").error == SshError::None);
    CHECK(mapSshExit(3, "Permission denied").error == SshError::RemoteExit);
    struct { const char *tail; SshError e; } cases[] = {
        {"it2wtest@127.0.0.1: Permission denied (publickey).\r\n", SshError::AuthFailed},
        {"\x1b[31m@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@\x1b[0m\r\nHost key verification failed.\r\n", SshError::HostKeyChanged},
        {"Host key verification failed.\r\n", SshError::HostKeyUnknown},
        {"ssh: connect to host 127.0.0.1 port 1: Connection refused\r\n", SshError::ConnectionRefused},
        {"ssh: connect to host 10.255.255.1 port 22: Connection timed out\r\n", SshError::Timeout},
        {"ssh: Could not resolve hostname nope.invalid: Name or service not known\r\n", SshError::HostNotFound},
        {"C:/x.conf: line 3: Bad configuration option: bogusoption42\r\nC:/x.conf: terminating, 1 bad configuration options\r\n", SshError::ClientIncompatible},
        {"Can't open user config file C:/x.conf: No such file or directory\r\n", SshError::ConfigError},
        {"Connection closed by 127.0.0.1 port 22\r\n", SshError::ConnectionClosed},
        {"kex_exchange_identification: read: Connection reset by peer\r\n", SshError::ConnectionClosed},
        {"\x1b]0;title\x07garbage\r\n", SshError::Unknown},
    };
    for (auto &c : cases) {
        auto r = mapSshExit(255, c.tail);
        if (r.error != c.e) std::printf("  map mismatch: %s -> %s\n", c.tail, sshErrorName(r.error));
        CHECK(r.error == c.e);
    }
    auto r = mapSshExit(255, "x\r\nit2wtest@127.0.0.1: Permission denied (publickey).\r\n");
    CHECK(r.detail == "it2wtest@127.0.0.1: Permission denied (publickey).");
    OutputTail tail(16);
    tail.append("0123456789"); tail.append("abcdefghij");
    CHECK(tail.text() == "456789abcdefghij");
    CHECK(stripTerminalEscapes("\x1b[1;31mA\x1b]2;t\x1b\\B\r") == "AB\n");
}

// ssh -G：三种模式的生效值（SSH-CFG-01 A1–A4 子集，OpenSSH 版本以本机为准）
static void effective() {
    std::string out;
    for (auto m : {SshAuthMode::Agent, SshAuthMode::KeyFile, SshAuthMode::Interactive}) {
        auto t = target(22422, "it2wtest", m, "/" + tmpdir + "/dummy_key");
        auto p = writeSnap(t, "g.conf");
        int rc = run("HOME=" + tmpdir + " ssh -G -F " + p + " it2w-target", &out);
        CHECK(rc == 0);
        auto cfg = parseSshG(out);
        auto bad = checkEffectiveConfig(cfg, m == SshAuthMode::Interactive);
        for (auto &b : bad) std::printf("  -G mode %d: %s\n", int(m), b.c_str());
        CHECK(bad.empty());
        CHECK(sshGFirst(cfg, "hostname") == "127.0.0.1" && sshGFirst(cfg, "port") == "22422");
        CHECK(sshGFirst(cfg, "requesttty") == "force");
    }
}

static bool portOpen(int port) {
    std::string o;
    return run("timeout 2 bash -c 'echo > /dev/tcp/127.0.0.1/" + std::to_string(port) + "'", &o) == 0;
}
static SshExitInfo connect(const SshTarget &t, const char *name, const std::string &remote = "exit 7") {
    auto p = writeSnap(t, name);
    std::string out;
    // ConPTY 下 stderr 与 stdout 合流；这里同样 2>&1 后按尾部映射。stdin 置空：未知主机时 ask 无法确认
    int rc = run("HOME=" + tmpdir + " timeout 20 ssh -F " + p + " it2w-target " + remote + " </dev/null", &out);
    OutputTail tail; tail.append(out);
    return mapSshExit(uint32_t(rc), tail.text());
}

// OpenSSH 10 的 sshd 对失败来源有 PerSourcePenalties：连续认证失败后会直接重置连接。
// 遇到 ConnectionClosed 时退避重试，避免把服务器惩罚误判为映射错误。
static SshExitInfo connectRetry(const SshTarget &t, const char *name, const std::string &remote = "exit 7") {
    SshExitInfo e;
    for (int i = 0; i < 4; ++i) {
        e = connect(t, name, remote);
        if (e.error != SshError::ConnectionClosed) break;
        std::printf("  (sshd penalty, retry in 20s)\n");
        sleep(20);
    }
    return e;
}

// 密码登录（22423/it2wpw）与带口令密钥（id_pass）：秘密只经 SSH_ASKPASS 脚本从文件读出交给 ssh，
// 测试进程本身不读取、不打印。生产代码中 SSH_ASKPASS 会被 filterChildEnvironment 移除，此处仅测试 harness 使用。
static SshExitInfo connectAskpass(const SshTarget &t, const char *name, const std::string &secretFile) {
    auto p = writeSnap(t, name);
    std::string ap = tmpdir + "/askpass.sh", out;
    std::ofstream(ap) << "#!/bin/sh\nexec cat " << secretFile << "\n";
    run("chmod 700 " + ap, &out);
    int rc = run("HOME=" + tmpdir + " SSH_ASKPASS=" + ap + " SSH_ASKPASS_REQUIRE=force DISPLAY=:0 timeout 20 setsid -w ssh -F " +
                 p + " it2w-target exit 7 </dev/null", &out);
    OutputTail tail; tail.append(out);
    return mapSshExit(uint32_t(rc), tail.text());
}
// 凭据目录：环境变量 IT2W_SSH_LAB 指定，默认 /workspace/qa/ssh-lab（凭据不入库、不打印）
static std::string labPath(const char *f) {
    const char *d = std::getenv("IT2W_SSH_LAB");
    std::string dir = (d && *d) ? d : "/workspace/qa/ssh-lab";
    if (dir.back() != '/') dir += '/';
    return dir + f;
}
static void live_secret() {
    const std::string pws = labPath("it2wpw.password"), pks = labPath("id_pass"), pps = labPath("id_pass.passphrase");
    const char *pw = pws.c_str(), *pk = pks.c_str(), *pp = pps.c_str();
    if (portOpen(22423) && access(pw, R_OK) == 0) {
        std::string scan; run("ssh-keyscan -p 22423 127.0.0.1 2>/dev/null", &scan);
        std::ofstream(tmpdir + "/known_hosts", std::ios::app) << scan;
        auto e = connectAskpass(target(22423, "it2wpw", SshAuthMode::Interactive), "p.conf", pw);
        std::printf("  password exit 7 -> %s (%u)\n", sshErrorName(e.error), e.exitCode);
        CHECK(e.error == SshError::RemoteExit && e.exitCode == 7);
    } else std::printf("SKIP live password: 22423 不可达或 %s 对当前用户不可读\n", pw);
    if (access(pk, R_OK) == 0 && access(pp, R_OK) == 0) {
        auto e = connectAskpass(target(22422, "it2wtest", SshAuthMode::KeyFile, std::string("/") + pk), "q.conf", pp);
        std::printf("  passphrase-key exit 7 -> %s (%u)\n", sshErrorName(e.error), e.exitCode);
        CHECK(e.error == SshError::RemoteExit && e.exitCode == 7);
    } else std::printf("SKIP live passphrase-key: %s 或口令文件对当前用户不可读\n", pk);
}

static void live() {
    if (!portOpen(22422)) { std::printf("SKIP live: 127.0.0.1:22422 不可达\n"); return; }
    std::string out, scan;
    auto stranger = "/" + tmpdir + "/stranger";
    // 先在无认证失败时取主机密钥
    for (int i = 0; i < 4 && scan.find("ssh-") == std::string::npos && scan.find("ecdsa") == std::string::npos; ++i) {
        if (i) sleep(20);
        run("ssh-keyscan -p 22422 127.0.0.1 2>/dev/null", &scan);
    }
    CHECK(!scan.empty());
    // 1) 未知主机：known_hosts 为空 → HostKeyUnknown（认证前失败，不触发惩罚）
    std::ofstream(tmpdir + "/known_hosts").close();
    auto e = connectRetry(target(22422, "it2wtest", SshAuthMode::KeyFile, stranger), "a.conf");
    std::printf("  unknown-host -> %s (%u)\n", sshErrorName(e.error), e.exitCode);
    CHECK(e.error == SshError::HostKeyUnknown);
    // 2) 主机密钥被替换 → HostKeyChanged
    {
        std::string fake;
        run("cat " + tmpdir + "/stranger.pub", &fake);
        std::istringstream is(fake); std::string type, b64; is >> type >> b64;
        std::ofstream(tmpdir + "/known_hosts") << "[127.0.0.1]:22422 " << type << " " << b64 << "\n";
        e = connectRetry(target(22422, "it2wtest", SshAuthMode::KeyFile, stranger), "c.conf");
        std::printf("  changed-key -> %s (%u)\n", sshErrorName(e.error), e.exitCode);
        CHECK(e.error == SshError::HostKeyChanged);
    }
    std::ofstream(tmpdir + "/known_hosts") << scan;
    // 3) 端口无人监听 → ConnectionRefused
    e = connect(target(1, "it2wtest", SshAuthMode::KeyFile, stranger), "d.conf");
    std::printf("  refused -> %s\n", sshErrorName(e.error));
    CHECK(e.error == SshError::ConnectionRefused);
    // 4) 授权密钥实连：远端 exit 7 → RemoteExit；凭据目录见 labPath
    const std::string keys = labPath("id_test"); const char *key = keys.c_str();
    if (access(key, R_OK) == 0) {
        e = connectRetry(target(22422, "it2wtest", SshAuthMode::KeyFile, std::string("/") + key), "f.conf");
        std::printf("  authorized-key exit 7 -> %s (%u)\n", sshErrorName(e.error), e.exitCode);
        CHECK(e.error == SshError::RemoteExit && e.exitCode == 7);
        e = connectRetry(target(22422, "it2wtest", SshAuthMode::KeyFile, std::string("/") + key), "g2.conf", "true");
        CHECK(e.error == SshError::None);
    } else {
        std::printf("SKIP live authorized-key: %s 对当前用户不可读\n", key);
    }
    // 5) 已知主机 + 非授权密钥 → AuthFailed
    e = connectRetry(target(22422, "it2wtest", SshAuthMode::KeyFile, stranger), "b.conf");
    std::printf("  wrong-key -> %s (%u) %s\n", sshErrorName(e.error), e.exitCode, e.detail.c_str());
    CHECK(e.error == SshError::AuthFailed);
    // 6) 交互模式 + 只允许公钥的服务器 → AuthFailed（不读密码文件，stdin 为空）
    e = connectRetry(target(22422, "it2wtest", SshAuthMode::Interactive), "e.conf");
    std::printf("  interactive-vs-pubkey-only -> %s\n", sshErrorName(e.error));
    CHECK(e.error == SshError::AuthFailed);
    live_secret();
}

int main(int argc, char **argv) {
    char tpl[] = "/tmp/it2w-ssh-XXXXXX";
    tmpdir = mkdtemp(tpl);
    std::ofstream(tmpdir + "/empty_known_hosts").close();
    std::ofstream(tmpdir + "/known_hosts").close();
    std::string o;
    run("ssh-keygen -q -t ed25519 -N '' -f " + tmpdir + "/stranger", &o);
    std::string mode = argc > 1 ? argv[1] : "unit";
    if (mode == "unit") unit();
    else if (mode == "effective") effective();
    else if (mode == "live") live();
    run("rm -rf " + tmpdir, &o);
    if (!failures) std::printf("ssh_session %s OK\n", mode.c_str());
    return failures ? 1 : 0;
}
