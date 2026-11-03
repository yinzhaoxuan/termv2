#include "ssh_direct_logic.h"
#include <cstdio>
#include <limits>

using namespace it2w;
static int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #__VA_ARGS__); ++failures; \
} } while (0)

static SshTarget target() {
    SshTarget t;
    t.hostName = "example.com";
    t.user = "alice";
    t.appKnownHostsFile = "C:\\cfg\\known_hosts";
    t.emptyKnownHostsFile = "C:\\cfg\\empty";
    t.keyFile = "D:\\keys\\id_ed25519";
    return t;
}

static void test_validation_and_snapshots() {
    // Frozen byte contract, including UTF-8, LF, whitespace and option order.
    const std::string prefix =
        "# iTerm2-Windows 生成的 SSH 配置快照；每个连接尝试独立一份，结束后删除\n"
        "Host it2w-target\n"
        "  HostName example.com\n"
        "  Port 22\n"
        "  User alice\n"
        "  StrictHostKeyChecking ask\n"
        "  UserKnownHostsFile \"C:/cfg/known_hosts\"\n"
        "  GlobalKnownHostsFile \"C:/cfg/empty\"\n"
        "  VerifyHostKeyDNS no\n"
        "  UpdateHostKeys no\n"
        "  RequestTTY force\n"
        "  ConnectTimeout 10\n"
        "  BatchMode no\n"
        "  ForwardAgent no\n"
        "  ForwardX11 no\n"
        "  PermitLocalCommand no\n"
        "  ClearAllForwardings yes\n";
    const std::string publickey =
        "  PreferredAuthentications publickey\n"
        "  PubkeyAuthentication yes\n"
        "  PasswordAuthentication no\n"
        "  KbdInteractiveAuthentication no\n";
    const std::string suffix =
        "  CertificateFile none\n"
        "  ControlMaster no\n"
        "  ControlPath none\n";
    struct { SshAuthMode mode; std::string body; } modes[] = {
        {SshAuthMode::Agent, publickey + "  IdentityFile none\n  IdentitiesOnly no\n"},
        {SshAuthMode::KeyFile, publickey +
            "  IdentityFile \"D:/keys/id_ed25519\"\n  IdentitiesOnly yes\n  IdentityAgent none\n"},
        {SshAuthMode::Interactive,
            "  PreferredAuthentications keyboard-interactive,password\n"
            "  PubkeyAuthentication no\n"
            "  PasswordAuthentication yes\n"
            "  KbdInteractiveAuthentication yes\n"
            "  IdentityFile none\n  IdentitiesOnly yes\n  IdentityAgent none\n"}
    };
    for (auto &c : modes) {
        auto t = target(); t.auth = c.mode;
        CHECK(!validateSshTarget(t));
        auto result = buildSshSnapshot(t);
        CHECK(!result.error && result.text == prefix + c.body + suffix);
    }
    auto invalid = [](const SshTarget &t, const char *field, const char *reason) {
        auto error = validateSshTarget(t);
        auto snapshot = buildSshSnapshot(t);
        CHECK(error && error->field == field && error->reason == reason);
        CHECK(!snapshot.text && snapshot.error && snapshot.error->field == field &&
              snapshot.error->reason == reason);
    };
    auto t = target(); t.alias = "*";
    invalid(t, "alias", "只允许字母数字 . _ -");
    t = target(); t.hostName = "-bad"; invalid(t, "hostName", "非法主机名");
    t = target(); t.port = 0; invalid(t, "port", "端口为 0");
    t = target(); t.user = "bad user"; invalid(t, "user", "非法用户名");
    for (uint32_t timeout : {0u, 601u}) {
        t = target(); t.connectTimeoutSec = timeout;
        invalid(t, "connectTimeoutSec", "超出 1..600");
    }
    t = target(); t.appKnownHostsFile = "relative";
    invalid(t, "appKnownHostsFile", "不是绝对路径");
    t = target(); t.emptyKnownHostsFile = "C:/%h";
    invalid(t, "emptyKnownHostsFile", "含 %（ssh token 展开）");
    t = target(); t.auth = SshAuthMode::KeyFile; t.keyFile.clear();
    invalid(t, "keyFile", "空路径");
    // The shared validator keeps first-error precedence, and ignores keyFile
    // outside KeyFile mode, just as snapshot generation did before extraction.
    t.appKnownHostsFile.clear(); invalid(t, "appKnownHostsFile", "空路径");
    t.alias = "*"; invalid(t, "alias", "只允许字母数字 . _ -");
    for (auto mode : {SshAuthMode::Agent, SshAuthMode::Interactive}) {
        t = target(); t.auth = mode; t.keyFile = "relative\ninvalid";
        CHECK(!validateSshTarget(t) && buildSshSnapshot(t).text);
    }
    t = target();
    t.hostName = "192.0.2.10";
    t.port = 2222;
    t.hostKeyAlias = "hv-0f3a1b2c-0000-4000-8000-00000000abcd";
    auto aliased = buildSshSnapshot(t);
    CHECK(aliased.text);
    CHECK(aliased.text->find(
        "  HostName 192.0.2.10\n"
        "  HostKeyAlias hv-0f3a1b2c-0000-4000-8000-00000000abcd\n"
        "  Port 2222\n") != std::string::npos);
    CHECK(aliased.text->find(
        "  VerifyHostKeyDNS no\n  CheckHostIP no\n  UpdateHostKeys no\n") != std::string::npos);
    t.hostKeyAlias = "bad alias";
    invalid(t, "hostKeyAlias", "只允许字母数字 . _ -");
}

static void test_sizes() {
    DirectSizes sizes({0, 0});
    CHECK(sizes.desired == DirectSizeRequest{{20, 5}, 0});
    CHECK(!sizes.inflight && !sizes.sent && sizes.needsSend());
    sizes.resize({200, 100}, 0); // Equal launch generation is ignored.
    CHECK(sizes.desired == DirectSizeRequest{{20, 5}, 0});
    auto launch = sizes.beginSend();
    CHECK(launch == DirectSizeRequest{{20, 5}, 0});
    sizes.resize({100, 30}, 2);
    sizes.resize({120, 40}, 4);
    sizes.resize({90, 20}, 3); // Rollback must never win.
    sizes.resize({90, 20}, 4); // Nor can a duplicate replace the same gen.
    CHECK(sizes.desired == DirectSizeRequest{{120, 40}, 4});
    CHECK(sizes.beginSend() == launch); // EAGAIN retry remains immutable.
    sizes.completeSend();
    CHECK(sizes.sent == launch && !sizes.inflight && sizes.needsSend());
    auto change = sizes.beginSend();
    CHECK(change == DirectSizeRequest{{120, 40}, 4});
    sizes.resize({160, 50}, 5);
    CHECK(sizes.beginSend() == change);
    sizes.completeSend();
    CHECK(sizes.needsSend()); // Must send gen 5 before allowing shell.
    CHECK(sizes.beginSend() == DirectSizeRequest{{160, 50}, 5});
    sizes.completeSend();
    CHECK(!sizes.needsSend() && !sizes.beginSend());
    sizes.completeSend(); // No outstanding request is harmless.
    CHECK(sizes.sent == DirectSizeRequest{{160, 50}, 5});
    sizes.closing = true;
    sizes.resize({300, 100}, 6);
    CHECK(sizes.desired == DirectSizeRequest{{160, 50}, 5});
    CHECK(!sizes.needsSend() && !sizes.beginSend());

    DirectSizes beforePty({80, 24});
    beforePty.resize({132, 43}, 1); // Starting resize before pty-req starts.
    CHECK(beforePty.beginSend() == DirectSizeRequest{{132, 43}, 1});
    beforePty.closing = true;
    CHECK(!beforePty.beginSend());
    beforePty.completeSend();
    CHECK(!beforePty.needsSend());

    // Exercise launch, desired, retry and sent dimensions at/below both floors.
    for (int cols : {std::numeric_limits<int>::min(), -1, 0, 19, 20, 80}) {
        for (int rows : {-1, 0, 4, 5, 24}) {
            DirectSizes s({cols, rows});
            s.resize({cols, rows}, 1);
            for (auto request : {s.desired, *s.beginSend()})
                CHECK(request.grid.cols >= 20 && request.grid.rows >= 5);
            s.completeSend();
            CHECK(s.sent && s.sent->grid.cols >= 20 && s.sent->grid.rows >= 5);
        }
    }
    DirectSizes maxGen;
    maxGen.resize({90, 30}, std::numeric_limits<uint64_t>::max());
    maxGen.resize({100, 40}, 0); // Generation wrap must not become a new size.
    CHECK(maxGen.desired.grid == DirectGrid{90, 30});
}

// Independently computed HMAC-SHA1 fixtures (salt = bytes 0..19). The injected
// test callback also checks that decoded binary salt and canonical host arrive
// intact, including the leading NUL and non-default-port brackets.
static std::optional<DirectSha1> testHmac(std::string_view salt, std::string_view host) {
    CHECK(salt.size() == 20);
    for (size_t i = 0; i < salt.size(); ++i) CHECK(static_cast<unsigned char>(salt[i]) == i);
    if (host == "example.com") return DirectSha1{
        158, 117, 10, 215, 160, 13, 177, 119, 119, 132, 189, 245, 97, 240, 36, 24, 233, 110, 74, 53};
    if (host == "[example.com]:2222") return DirectSha1{
        90, 7, 49, 248, 89, 190, 46, 102, 150, 192, 46, 235, 67, 205, 30, 33, 253, 174, 29, 237};
    if (host == "[2001:db8::1]:2222") return DirectSha1{
        160, 224, 150, 207, 155, 13, 227, 1, 42, 2, 153, 124, 24, 188, 93, 92, 195, 177, 170, 148};
    return DirectSha1{};
}

static void test_known_hosts() {
    const std::string plain =
        " \t# comment\r\n\n"
        "other.example ssh-rsa AAAA\n"
        "elsewhere,example.com,third ssh-ed25519 AAAA comment\r\n"
        "EXAMPLE.COM ecdsa-sha2-nistp384 AAAA\n"
        "example.com ssh-ed25519 AAAA\n"
        "[example.com]:2222 ssh-rsa AAAA\n"
        "[2001:db8::1]:2222 ecdsa-sha2-nistp521 AAAA\n";
    auto r = knownHostTypesFor(plain, "ExAmPlE.CoM", 22, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-ed25519", "ecdsa-sha2-nistp384"});
    r = knownHostTypesFor(plain, "EXAMPLE.COM", 2222, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-rsa"});
    r = knownHostTypesFor(plain, "2001:DB8::1", 2222, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ecdsa-sha2-nistp521"});
    for (auto port : {23, 2200}) {
        r = knownHostTypesFor(plain, "example.com", uint16_t(port), {});
        CHECK(!r.error && r.types.empty()); // No default-port fallback.
    }
    r = knownHostTypesFor(plain, "missing", 22, {});
    CHECK(!r.error && r.types.empty());
    r = knownHostTypesFor("", "example.com", 22, {});
    CHECK(!r.error && r.types.empty());
    r = knownHostTypesFor("\xc3\x84HOST ssh-ed25519 AAAA", "\xc3\x84host", 22, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-ed25519"});

    const std::string salt = "|1|AAECAwQFBgcICQoLDA0ODxAREhM=|";
    const std::string hashed =
        salt + "nnUK16ANsXd3hL31YfAkGOluSjU= ssh-ed25519 AAAA\n" +
        salt + "Wgcx+Fm+LmaWwC7rQ80eIf2uHe0= ecdsa-sha2-nistp256 AAAA\n" +
        salt + "oOCWz5sN4wEqApl8GLxdXMOxqpQ= ssh-rsa AAAA";
    r = knownHostTypesFor(hashed, "EXAMPLE.COM", 22, testHmac);
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-ed25519"});
    r = knownHostTypesFor(hashed, "EXAMPLE.COM", 2222, testHmac);
    CHECK(!r.error && r.types == std::vector<std::string>{"ecdsa-sha2-nistp256"});
    r = knownHostTypesFor(hashed, "2001:DB8::1", 2222, testHmac);
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-rsa"});
    r = knownHostTypesFor(hashed, "different", 22, testHmac);
    CHECK(!r.error && r.types.empty());
    r = knownHostTypesFor(hashed, "EXAMPLE.COM", 22, {});
    CHECK(r.error && r.types.empty());
    r = knownHostTypesFor(hashed, "EXAMPLE.COM", 22,
        [](auto, auto) -> std::optional<DirectSha1> { return std::nullopt; });
    CHECK(r.error && r.types.empty());

    for (const std::string bad : {
        "@cert-authority other ssh-ed25519 AAAA", "@revoked other ssh-rsa AAAA",
        "@unknown other ssh-rsa AAAA", "example.com", "example.com ssh-rsa",
        "example.com ssh-rsa !!!", "example.com ssh-rsa A===",
        "example.com ssh-rsa AB==", "example.com ssh-rsa A",
        ",example.com ssh-rsa AAAA", "example.com, ssh-rsa AAAA",
        "|2|salt|hash ssh-rsa AAAA", "|1|missing ssh-rsa AAAA",
        "|1|!!!|nnUK16ANsXd3hL31YfAkGOluSjU= ssh-rsa AAAA",
        "|1|AAECAwQFBgcICQoLDA0ODxAREhM=|AAAA ssh-rsa AAAA",
        "|1|AAECAwQFBgcICQoLDA0ODxAREhM=|nnUK16ANsXd3hL31YfAkGOluSjU=|extra ssh-rsa AAAA"
    }) {
        r = knownHostTypesFor("example.com ssh-ed25519 AAAA\n" + bad, "example.com", 22, testHmac);
        CHECK(r.error && r.error->field == "appKnownHostsFile" && r.types.empty());
        CHECK(r.error && r.error->reason.find("line 2") != std::string::npos);
    }
    CHECK(knownHostTypesFor(plain, "", 22, {}).error);
    CHECK(knownHostTypesFor(plain, "example.com", 0, {}).error);

    // Hyper-V: the alias is the identity. The transient address, and a
    // port-qualified copy of the alias, must not win. OpenSSH stores
    // HostKeyAlias verbatim (no put_host_port).
    const char *alias = "HV-0F3A1B2C-0000-4000-8000-00000000ABCD";
    const char *canon = "hv-0f3a1b2c-0000-4000-8000-00000000abcd";
    CHECK(directHostKeyLookup("192.0.2.10", 22, "") == "192.0.2.10");
    CHECK(directHostKeyLookup("192.0.2.10", 2222, "") == "[192.0.2.10]:2222");
    CHECK(directHostKeyLookup("192.0.2.10", 22, alias) == canon);
    CHECK(directHostKeyLookup("192.0.2.10", 2222, alias) == canon);
    const std::string mixed =
        "192.0.2.10 ssh-rsa AAAA\n"
        "[192.0.2.10]:2222 ecdsa-sha2-nistp256 AAAA\n"
        "hv-0f3a1b2c-0000-4000-8000-00000000abcd ssh-ed25519 AAAA\n"
        "[hv-0f3a1b2c-0000-4000-8000-00000000abcd]:2222 ssh-rsa AAAA\n";
    r = knownHostTypesFor(mixed, "192.0.2.10", 2222, {}, alias);
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-ed25519"});
    r = knownHostTypesFor(mixed, "192.0.2.10", 22, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ssh-rsa"});
    r = knownHostTypesFor(mixed, "192.0.2.10", 2222, {});
    CHECK(!r.error && r.types == std::vector<std::string>{"ecdsa-sha2-nistp256"});
    std::string hashedHost;
    auto watch = [&](std::string_view hmacSalt, std::string_view host) -> std::optional<DirectSha1> {
        hashedHost = std::string(host);
        return testHmac(hmacSalt, host);
    };
    r = knownHostTypesFor(salt + "nnUK16ANsXd3hL31YfAkGOluSjU= ssh-ed25519 AAAA\n",
                          "192.0.2.10", 2222, watch, alias);
    CHECK(!r.error && hashedHost == canon && r.types.empty());
}

static void test_preferences_and_auth() {
    const std::string all = "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,"
        "ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";
    CHECK(hostKeyPref({}) == all);
    CHECK(hostKeyPref({"ssh-ed25519"}) == all);
    CHECK(hostKeyPref({"ecdsa-sha2-nistp384"}) ==
        "ecdsa-sha2-nistp384,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256");
    CHECK(hostKeyPref({"ssh-rsa"}) ==
        "rsa-sha2-512,rsa-sha2-256,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521");
    CHECK(hostKeyPref({"ssh-rsa", "ecdsa-sha2-nistp521", "ssh-ed25519", "ssh-rsa"}) ==
        "ssh-ed25519,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384");
    CHECK(hostKeyPref({"rsa-sha2-256"}) ==
        "rsa-sha2-256,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512");
    CHECK(hostKeyPref({"unknown", "ssh-dss", "ssh-ed25519-cert-v01@openssh.com"}) == all);
    CHECK(hostKeyPref({"rsa-sha2-256", "ecdsa-sha2-nistp521", "rsa-sha2-512",
        "ecdsa-sha2-nistp384", "ssh-ed25519", "ecdsa-sha2-nistp256"}) == all);
    // Library capabilities can differ; preserve all supplied fallback methods,
    // deduplicate them and never introduce an algorithm absent from that set.
    CHECK(hostKeyPref({"ssh-rsa"}, {"ssh-dss", "ssh-ed25519", "rsa-sha2-256",
        "rsa-sha2-512", "ssh-rsa", "ssh-dss"}) ==
        "rsa-sha2-512,rsa-sha2-256,ssh-rsa,ssh-ed25519,ssh-dss");
    CHECK(hostKeyPref({"ssh-ed25519", "ecdsa-sha2-nistp256"}, {"rsa-sha2-256"}) == "rsa-sha2-256");
    CHECK(hostKeyPref({"ssh-ed25519"}, {}).empty());

    const std::string defaultPipe = R"(\\.\pipe\openssh-ssh-agent)";
    CHECK(directAgentPipe(R"(\\.\pipe\custom-agent)") == R"(\\.\pipe\custom-agent)");
    for (auto value : {"", "Pageant", "pageant", "/tmp/ssh-agent.sock", "C:/agent",
                       R"(\\.\PIPE\custom)", R"(\\server\pipe\agent)",
                       R"(\\?\pipe\agent)", R"(x\\.\pipe\agent)"}) {
        CHECK(directAgentPipe(value) == defaultPipe);
        CHECK(directAgentPipe(value) != "Pageant");
    }
    CHECK(!validateDirectAuthMode(SshAuthMode::Agent));
    for (auto mode : {SshAuthMode::KeyFile, SshAuthMode::Interactive}) {
        auto e = validateDirectAuthMode(mode);
        CHECK(e && e->field == "auth" && e->reason ==
            "直连 v0 仅支持 agent；请 ssh-add 后使用，或去掉 ITERM2WIN_SSH_DIRECT 使用兼容模式");
    }
}

static void test_presented_host_key() {
    using T = DirectHostKeyType;
    struct { T type; const char *name; } cases[] = {
        {T::Ed25519, "ssh-ed25519"}, {T::Rsa, "ssh-rsa"},
        {T::Ecdsa256, "ecdsa-sha2-nistp256"}, {T::Ecdsa384, "ecdsa-sha2-nistp384"},
        {T::Ecdsa521, "ecdsa-sha2-nistp521"}
    };
    for (const auto &c : cases) {
        CHECK(directHostKeyTypeName(c.type) == c.name);
        CHECK(canOfferTrust(c.type));
        auto key = presentHostKey("[example.com]:2222", c.type, std::string_view("\0\xff\x80", 3), "SHA256:test");
        CHECK(key && key->knownHostsName == "[example.com]:2222" && key->keyType == c.name &&
              key->keyBase64 == "AP+A" && key->fingerprint == "SHA256:test");
    }
    CHECK(directHostKeyTypeName(T::Dss) == "ssh-dss");
    CHECK(!directHostKeyTypeName(T::Unknown));
    for (auto type : {T::Dss, T::Unknown}) {
        CHECK(!canOfferTrust(type));
        CHECK(!presentHostKey("host", type, "raw", "SHA256:test"));
    }
    CHECK(!presentHostKey("host", T::Ed25519, {}, "SHA256:test"));
    const auto key = presentHostKey("hv-guid", T::Ed25519, "raw", "SHA256:test");
    auto exit = mapDirectFailure({DirectStage::HostKey, DirectFailureKind::HostKeyUnknown, 0, key});
    CHECK(exit.error == SshError::HostKeyUnknown && exit.hostKey && exit.hostKey->knownHostsName == "hv-guid");
    exit = mapDirectFailure({DirectStage::HostKey, DirectFailureKind::HostKeyChanged, 0, key});
    CHECK(exit.error == SshError::HostKeyChanged && !exit.hostKey);
}

static void test_host_key_base64() {
    struct { const char *raw, *encoded; } cases[] = {
        {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foobar", "Zm9vYmFy"}
    };
    for (const auto &c : cases) {
        const auto key = presentHostKey("host", DirectHostKeyType::Ed25519, c.raw, "fp");
        CHECK(key && key->keyBase64 == c.encoded);
    }
    CHECK(!presentHostKey("host", DirectHostKeyType::Ed25519, "", "fp"));
    std::string raw;
    for (unsigned char c = 0; c < 32; ++c) raw += char(c);
    const auto key = presentHostKey("host", DirectHostKeyType::Ed25519, raw, "fp");
    CHECK(key && key->keyBase64 == "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
    CHECK(key && key->keyBase64.size() == 44);
    if (!key || key->keyBase64.size() != 44) return;
    // 独立按四字符组解码，校验二进制内容与长度。
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    for (size_t i = 0; i < key->keyBase64.size(); i += 4) {
        const auto group = key->keyBase64.substr(i, 4);
        unsigned value = 0;
        for (char c : group) value = (value << 6) | (c == '=' ? 0u : unsigned(alphabet.find(c)));
        decoded += char((value >> 16) & 0xff);
        if (group[2] != '=') decoded += char((value >> 8) & 0xff);
        if (group[3] != '=') decoded += char(value & 0xff);
    }
    CHECK(decoded.size() == 32 && decoded == raw);
}

static void test_failure_mapping() {
    using Stage = DirectStage;
    using Kind = DirectFailureKind;
    struct { Stage stage; Kind kind; SshError error; const char *detail; } cases[] = {
        {Stage::Dns, Kind::HostNotFound, SshError::HostNotFound, "DNS"},
        {Stage::Dns, Kind::NoData, SshError::HostNotFound, "DNS"},
        {Stage::Connect, Kind::ConnectionRefused, SshError::ConnectionRefused, "refused"},
        {Stage::Connect, Kind::TimedOut, SshError::Timeout, "timed out"},
        {Stage::Handshake, Kind::DeadlineExpired, SshError::Timeout, "handshake"},
        {Stage::Connect, Kind::NetworkUnreachable, SshError::NetworkUnreachable, "unreachable"},
        {Stage::Connect, Kind::HostUnreachable, SshError::NetworkUnreachable, "unreachable"},
        {Stage::Handshake, Kind::KexFailed, SshError::ClientIncompatible, "key exchange"},
        {Stage::Handshake, Kind::NoHostKeyAlgorithm, SshError::ClientIncompatible, "hostkey algorithm"},
        {Stage::Handshake, Kind::NoCipherAlgorithm, SshError::ClientIncompatible, "cipher algorithm"},
        {Stage::Handshake, Kind::SocketClosed, SshError::ConnectionClosed, "handshake"},
        {Stage::Handshake, Kind::SocketReset, SshError::ConnectionClosed, "handshake"},
        {Stage::Running, Kind::SocketClosed, SshError::ConnectionClosed, "running"},
        {Stage::Running, Kind::SocketReset, SshError::ConnectionClosed, "running"},
        {Stage::KnownHosts, Kind::KnownHostsOpenFailed, SshError::ConfigError, "open"},
        {Stage::KnownHosts, Kind::KnownHostsParseFailed, SshError::ConfigError, "parse"},
        {Stage::KnownHosts, Kind::KnownHostsMarker, SshError::ConfigError, "markers"},
        {Stage::HostKey, Kind::HostKeyUnknown, SshError::HostKeyUnknown, "请先用兼容模式"},
        {Stage::HostKey, Kind::HostKeyChanged, SshError::HostKeyChanged, "changed"},
        {Stage::Agent, Kind::AgentUnavailable, SshError::AuthFailed, "pipe unavailable"},
        {Stage::Auth, Kind::NoAcceptedIdentity, SshError::AuthFailed, "no accepted identity"},
        {Stage::Channel, Kind::RequestDenied, SshError::Unknown, "channel: request denied"},
        {Stage::Pty, Kind::RequestDenied, SshError::Unknown, "pty-req: request denied"},
        {Stage::Shell, Kind::RequestDenied, SshError::Unknown, "shell: request denied"},
        {Stage::KnownHosts, Kind::Other, SshError::ConfigError, "known_hosts"},
        {Stage::Auth, Kind::Other, SshError::AuthFailed, "auth"},
        {Stage::Agent, Kind::Other, SshError::AuthFailed, "agent"},
        {Stage::Running, Kind::Other, SshError::Unknown, "running"}
    };
    for (const auto &c : cases) {
        auto e = mapDirectFailure({c.stage, c.kind, -123});
        CHECK(e.error == c.error && e.exitCode == 255 && e.outputTail.empty());
        CHECK(e.detail.find(c.detail) != std::string::npos);
        CHECK(e.detail.find("code -123") != std::string::npos);
    }
    for (Stage stage : {Stage::Dns, Stage::Connect, Stage::Handshake, Stage::HostKey,
                         Stage::Auth, Stage::Channel, Stage::Pty, Stage::Shell}) {
        CHECK(mapDirectFailure({stage, Kind::DeadlineExpired}).error == SshError::Timeout);
    }
}

static void test_channel_exit_and_pty() {
    for (uint32_t code = 0; code <= 255; ++code) {
        auto e = mapChannelExit(code);
        CHECK(e.exitCode == code && e.error == (code ? SshError::RemoteExit : SshError::None));
        CHECK(e.detail.empty() && e.outputTail.empty());
    }
    auto e = mapChannelExit(0, "TERM");
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 143 && e.detail == "signal TERM");
    e = mapChannelExit(255, "INT");
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 130 && e.detail == "signal INT");
    e = mapChannelExit(0, "USR1", 30); // Remote macOS numbering supplied explicitly.
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 158 && e.detail == "signal USR1");
    e = mapChannelExit(0, "UNKNOWN");
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 255 && e.detail == "signal UNKNOWN");
    e = mapChannelExit(0, "TERM", std::numeric_limits<uint32_t>::max());
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 255);
    e = mapChannelExit(0, "TERM", 0);
    CHECK(e.error == SshError::RemoteExit && e.exitCode == 255);
    CHECK(mapChannelExit(0, {}, 15).error == SshError::None);
    const char expected[] = {1, 0, 0, 0, 3, 3, 0, 0, 0, 127, 42, 0, 0, 0, 1, 0};
    CHECK(directPtyModes() == std::string(expected, sizeof expected));
    CHECK(kDirectTerm == "xterm-256color");
    // Decode independently: only VINTR, VERASE, IUTF8, then TTY_OP_END.
    auto modes = directPtyModes();
    CHECK(modes.size() == 16 && modes.back() == 0);
    for (size_t i = 0; i + 1 < modes.size(); i += 5) {
        CHECK(modes[i] != 53 && modes[i] != 70); // Never ECHO=0/OPOST=0.
    }
}

int main() {
    test_validation_and_snapshots();
    test_sizes();
    test_known_hosts();
    test_preferences_and_auth();
    test_failure_mapping();
    test_presented_host_key();
    test_host_key_base64();
    test_channel_exit_and_pty();
    std::printf("ssh_direct_logic: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
