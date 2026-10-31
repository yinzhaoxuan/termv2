// Windows A acceptance probe. One worker exclusively owns each libssh2 session.
// No ConPTY/ssh.exe, product transport, terminal parser, or UI integration.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <libssh2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include "ssh_a_probe_logic.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

using namespace ssh_a_probe;
namespace {
constexpr uint64_t kCloseBudgetMs = 3000;
constexpr uint64_t kProtocolCloseMs = 2700; // leave room for socket/free/join
constexpr size_t kMaxCapture = 1024 * 1024;
constexpr char kAgentPipe[] = R"(\\.\pipe\openssh-ssh-agent)";
uint64_t now() { return GetTickCount64(); }
std::atomic<bool> interrupted{false};
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { interrupted = true; return TRUE; }
    return FALSE;
}
[[noreturn]] void hardFail(const char *why) {
    // A stuck owner must not be detached, freed from another thread, or called PASS.
    std::fprintf(stdout, "FAIL watchdog: %s (process hard stop)\n", why);
    std::fflush(stdout); std::fflush(stderr);
    ExitProcess(1);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() { if (valid()) CloseHandle(value); }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
// Win32 HANDLE, rather than std::thread::native_handle (MinGW-posix uses pthread_t).
class Thread {
    std::function<void()> body_;
    Handle handle_;
    static DWORD WINAPI entry(void *p) {
        auto *self = static_cast<Thread *>(p);
        try { self->body_(); } catch (...) { hardFail("uncaught worker exception"); }
        return 0;
    }
public:
    explicit Thread(std::function<void()> f) : body_(std::move(f)),
        handle_(CreateThread(nullptr, 0, entry, this, 0, nullptr)) {
        if (!handle_.valid()) throw Failure("CreateThread failed");
    }
    bool done(DWORD waitMs = 0) const { return WaitForSingleObject(handle_.value, waitMs) == WAIT_OBJECT_0; }
    bool cancelSync() const { return CancelSynchronousIo(handle_.value) != FALSE; }
    ~Thread() { if (!done()) hardFail("worker outlived its owner"); }
};
std::string utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw Failure("invalid Unicode argument");
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, result.data(), n, nullptr, nullptr);
    result.pop_back(); return result;
}
struct Options {
    std::string host, user, identity, knownHosts, test = "all";
    int port = 22, timeoutMs = 15000;
    Grid grid;
    std::vector<Grid> startingSizes;
};
void usage() {
    std::puts("ssh_a_probe --host H --user U --known-hosts PATH [--port 22]\n"
              "  [--identity PATH] [--cols 80 --rows 24] [--starting-size COLSxROWS ...]\n"
              "  [--timeout-ms 15000] [--test all|auth|bytes|stty|resize|close-auth|close-flood]\n"
              "Default authentication: Windows OpenSSH named-pipe agent. No interactive auth.\n"
              "--identity: unencrypted key only; all / close-auth require the agent.\n"
              "all uses one session/channel for auth, stty, bytes, resize, close-flood;\n"
              "close-auth necessarily uses a separate, interrupted authentication attempt.");
}
Options parse(int argc, wchar_t **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        auto a = utf8(argv[i]);
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        if (i + 1 >= argc) throw Failure("missing value for " + a);
        auto v = utf8(argv[++i]);
        if (a == "--host") o.host = v;
        else if (a == "--user") o.user = v;
        else if (a == "--known-hosts") o.knownHosts = v;
        else if (a == "--identity") o.identity = v;
        else if (a == "--test") o.test = v;
        else if (a == "--port") o.port = number(v, 1, 65535, "--port");
        else if (a == "--cols") o.grid.cols = number(v, 0, 65535, "--cols");
        else if (a == "--rows") o.grid.rows = number(v, 0, 65535, "--rows");
        else if (a == "--timeout-ms") o.timeoutMs = number(v, 1000, 300000, "--timeout-ms");
        else if (a == "--starting-size") {
            auto x = v.find('x');
            if (x == std::string::npos) throw Failure("--starting-size requires COLSxROWS");
            o.startingSizes.push_back(clamp({number(v.substr(0, x), 0, 65535, "cols"),
                                            number(v.substr(x + 1), 0, 65535, "rows")}));
        } else if (a == "--auth") {
            if (v != "agent") throw Failure("only --auth agent is supported; interactive/password/encrypted keys are unsupported; use --identity for an unencrypted key");
        } else throw Failure("unknown option: " + a);
    }
    if (o.host.empty() || o.user.empty() || o.knownHosts.empty())
        throw Failure("--host, --user and --known-hosts are required (known_hosts is read-only)");
    if (o.host.find_first_of(" \t\r\n[]") != std::string::npos || o.host.front() == '-')
        throw Failure("--host must be a host name or bare numeric address, without brackets");
    if (o.user.find_first_of("\r\n") != std::string::npos) throw Failure("invalid --user");
    const std::vector<std::string> tests{"all", "auth", "bytes", "stty", "resize", "close-auth", "close-flood"};
    if (std::find(tests.begin(), tests.end(), o.test) == tests.end()) throw Failure("unknown --test: " + o.test);
    if (!o.identity.empty() && (o.test == "all" || o.test == "close-auth"))
        throw Failure("all / close-auth require Windows agent coverage; use --identity with individual tests");
    o.grid = clamp(o.grid);
    return o;
}
std::string readFile(const std::string &name, size_t limit) {
    std::ifstream f(std::filesystem::path(std::u8string(name.begin(), name.end())), std::ios::binary);
    if (!f) throw Failure("cannot open file: " + name);
    std::string content;
    char block[4096];
    while (f.read(block, sizeof block) || f.gcount()) {
        content.append(block, static_cast<size_t>(f.gcount()));
        if (content.size() > limit) throw Failure("file too large: " + name);
    }
    if (!f.eof()) throw Failure("cannot read file: " + name);
    return content;
}
uint32_t u32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void validateKey(const std::string &key) {
    auto reject = [] { throw Failure("encrypted private keys are unsupported; load into Windows agent or use compatibility mode"); };
    if (key.find("ENCRYPTED") != std::string::npos) reject();
    constexpr std::string_view begin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    if (auto a = key.find(begin); a != std::string::npos) {
        auto b = key.find("-----END OPENSSH PRIVATE KEY-----", a + begin.size());
        if (b == std::string::npos) throw Failure("invalid OpenSSH private key envelope");
        std::string b64;
        for (char c : std::string_view(key).substr(a + begin.size(), b - a - begin.size()))
            if (c != '\r' && c != '\n' && c != ' ' && c != '\t') b64 += c;
        std::vector<unsigned char> decoded(b64.size() + 1);
        int count = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char *>(b64.data()), static_cast<int>(b64.size()));
        constexpr char magic[] = "openssh-key-v1"; // includes NUL
        if (count < 0 || static_cast<size_t>(count) < sizeof magic + 4 ||
            std::memcmp(decoded.data(), magic, sizeof magic) != 0) throw Failure("invalid OpenSSH private key");
        auto p = decoded.data() + sizeof magic;
        auto n = u32(p); p += 4;
        bool unencrypted = n == 4 && static_cast<size_t>(count) >= sizeof magic + 8 && std::memcmp(p, "none", 4) == 0;
        OPENSSL_cleanse(decoded.data(), decoded.size());
        if (!unencrypted) reject();
    }
}
enum class Phase { Starting, Resolve, Connect, Handshake, HostKey, Auth, Channel, Pty, Shell, Running, Closing, Closed };
const char *phaseName(Phase p) {
    switch (p) {
    case Phase::Starting: return "Starting"; case Phase::Resolve: return "DNS";
    case Phase::Connect: return "Connect"; case Phase::Handshake: return "Handshake";
    case Phase::HostKey: return "HostKey"; case Phase::Auth: return "Authentication";
    case Phase::Channel: return "Channel"; case Phase::Pty: return "PTY";
    case Phase::Shell: return "Shell"; case Phase::Running: return "Running";
    case Phase::Closing: return "Closing"; case Phase::Closed: return "Closed";
    }
    return "?";
}
struct Control {
    std::atomic<uint64_t> cancelAt{0}, phaseAt{0}, stalledSignAt{0};
    std::atomic<Phase> phase{Phase::Starting};
    std::atomic<unsigned> cancellations{0};
    void set(Phase p) {
        phaseAt = now(); phase = p;
        std::fprintf(stderr, "[ssh-a] state=%s\n", phaseName(p));
    }
    void cancel() { uint64_t zero = 0; cancelAt.compare_exchange_strong(zero, now()); }
};
struct Result { bool pass = false, recorded = false; std::string reason; };
using Results = std::map<std::string, Result>;
void record(Results &r, const std::string &test, bool pass, std::string reason) {
    r[test] = {pass, true, std::move(reason)};
}

// Fault-injection relay for close-auth only. Enumerates the REAL OpenSSH agent's
// keys, then withholds the reply to the first SIGN_REQUEST accepted by the sshd.
// The worker is inside unmodified libssh2_agent_userauth -> synchronous ReadFile.
// The relay stays open until the worker has unwound, so a broken pipe cannot
// masquerade as successful CancelSynchronousIo. It never sees private keys.
class StallingAgent {
    Control &control_;
    Handle pipe_;
    std::atomic<bool> stop_{false};
    std::unique_ptr<Thread> thread_;
    std::string error_;
    static void transfer(HANDLE h, void *buffer, size_t n, bool write) {
        auto *p = static_cast<unsigned char *>(buffer);
        while (n) {
            DWORD done = 0;
            BOOL ok = write ? WriteFile(h, p, static_cast<DWORD>(n), &done, nullptr)
                            : ReadFile(h, p, static_cast<DWORD>(n), &done, nullptr);
            if (!ok || !done) throw Failure("agent relay pipe I/O failed");
            n -= done; p += done;
        }
    }
    static std::vector<unsigned char> packet(HANDLE h) {
        unsigned char prefix[4]; transfer(h, prefix, 4, false);
        uint32_t n = u32(prefix);
        if (!n || n > kMaxCapture) throw Failure("invalid/oversized agent message");
        std::vector<unsigned char> p(n + 4);
        std::memcpy(p.data(), prefix, 4); transfer(h, p.data() + 4, n, false);
        return p;
    }
    void run() {
        try {
            if (!ConnectNamedPipe(pipe_.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
                throw Failure("agent relay connect failed");
            Handle real(CreateFileA(kAgentPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            if (!real.valid()) throw Failure("cannot open real Windows OpenSSH agent for close-auth");
            auto request = packet(pipe_.value);
            if (request[4] != 11) throw Failure("expected agent REQUEST_IDENTITIES");
            transfer(real.value, request.data(), request.size(), true);
            auto identities = packet(real.value);
            if (identities[4] != 12) throw Failure("agent did not return IDENTITIES_ANSWER");
            transfer(pipe_.value, identities.data(), identities.size(), true);
            auto sign = packet(pipe_.value);
            if (sign[4] != 13) throw Failure("expected agent SIGN_REQUEST");
            control_.stalledSignAt = now();
            while (!stop_) Sleep(5); // intentionally no signing reply
        } catch (const std::exception &e) { error_ = e.what(); }
        // pipe_ is deliberately kept open until the session worker is done.
    }
public:
    std::string path;
    explicit StallingAgent(Control &c) : control_(c) {
        path = R"(\\.\pipe\termv2-ssh-a-probe-)" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(now());
        pipe_.value = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, nullptr);
        if (!pipe_.valid()) throw Failure("CreateNamedPipe(close-auth) failed");
        thread_ = std::make_unique<Thread>([this] { run(); });
    }
    ~StallingAgent() {
        stop_ = true;
        const auto end = now() + 200;
        while (!thread_->done(5) && now() < end) thread_->cancelSync();
        if (!thread_->done()) hardFail("agent relay did not join");
        if (!error_.empty()) std::fprintf(stderr, "[ssh-a] close-auth relay: %s\n", error_.c_str());
    }
};

class Session {
    const Options &options_;
    Control &control_;
    SOCKET socket_ = INVALID_SOCKET;
    LIBSSH2_SESSION *session_ = nullptr;
    LIBSSH2_CHANNEL *channel_ = nullptr;
    LIBSSH2_AGENT *agent_ = nullptr;
    Sizes sizes_;
    unsigned serial_ = 0;
    bool closed_ = false, handshaken_ = false;
    std::string key_;
    uint64_t deadline() const { return now() + options_.timeoutMs; }
    void checkpoint() const { if (control_.cancelAt) throw Cancelled{}; }
    void waitSocket(uint64_t until, bool closing = false, bool connecting = false) {
        if (!closing) checkpoint();
        if (now() >= until) throw Failure("timeout in " + std::string(phaseName(control_.phase)));
        int directions = connecting ? LIBSSH2_SESSION_BLOCK_OUTBOUND :
                         session_ ? libssh2_session_block_directions(session_) : 0;
        fd_set rd, wr, ex; FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) || !directions) FD_SET(socket_, &rd);
        if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(socket_, &wr);
        FD_SET(socket_, &ex);
        auto ms = std::min<uint64_t>(20, until - now());
        timeval tv{};
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>(ms * 1000);
        int n = select(0, &rd, &wr, &ex, &tv);
        if (n == SOCKET_ERROR) throw Failure("socket select error=" + std::to_string(WSAGetLastError()));
    }
    template<class F> int retry(F f, uint64_t until) {
        for (;;) {
            checkpoint();
            int rc = f();
            checkpoint();
            if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
            waitSocket(until);
        }
    }
    void check(int rc, const char *what) {
        if (rc == 0) return;
        char *message = nullptr;
        if (session_) libssh2_session_last_error(session_, &message, nullptr, 0);
        throw Failure(std::string(what) + ": libssh2=" + std::to_string(rc) + (message ? " " + std::string(message) : ""));
    }
    void connectSocket() {
        control_.set(Phase::Resolve);
        addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        addrinfo *raw = nullptr;
        auto port = std::to_string(options_.port);
        int rc = getaddrinfo(options_.host.c_str(), port.c_str(), &hints, &raw);
        if (rc) throw Failure("DNS error=" + std::to_string(rc));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
        checkpoint();
        control_.set(Phase::Connect);
        const auto until = deadline();
        for (auto *a = raw; a; a = a->ai_next) {
            socket_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (socket_ == INVALID_SOCKET) continue;
            u_long nonblocking = 1;
            if (ioctlsocket(socket_, FIONBIO, &nonblocking)) throw Failure("cannot set nonblocking socket");
            rc = connect(socket_, a->ai_addr, static_cast<int>(a->ai_addrlen));
            int error = rc == 0 ? 0 : WSAGetLastError();
            if (rc && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS)) {
                for (;;) {
                    waitSocket(until, false, true);
                    fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex); FD_SET(socket_, &wr); FD_SET(socket_, &ex);
                    timeval tv{};
                    rc = select(0, nullptr, &wr, &ex, &tv);
                    if (rc == SOCKET_ERROR) throw Failure("connect select failed");
                    if (!rc) continue;
                    int length = sizeof error;
                    if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length))
                        error = WSAGetLastError();
                    break;
                }
            }
            if (!error) return;
            closesocket(socket_); socket_ = INVALID_SOCKET;
        }
        throw Failure("TCP connect failed");
    }
    void verifyHost() {
        control_.set(Phase::HostKey);
        size_t keyLength = 0; int type = 0;
        const char *key = libssh2_session_hostkey(session_, &keyLength, &type);
        if (!key) throw Failure("server supplied no host key");
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int digestLength = 0;
        if (!EVP_Digest(key, keyLength, digest, &digestLength, EVP_sha256(), nullptr)) throw Failure("SHA256 failed");
        unsigned char encoded[4 * EVP_MAX_MD_SIZE + 1]{};
        EVP_EncodeBlock(encoded, digest, static_cast<int>(digestLength));
        std::string fingerprint(reinterpret_cast<char *>(encoded));
        while (!fingerprint.empty() && fingerprint.back() == '=') fingerprint.pop_back();
        fingerprint = "SHA256:" + fingerprint;
        std::fprintf(stderr, "[ssh-a] host-key %s\n", fingerprint.c_str());
        int algorithm;
        switch (type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: algorithm = LIBSSH2_KNOWNHOST_KEY_SSHRSA; break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: algorithm = LIBSSH2_KNOWNHOST_KEY_ED25519; break;
        default: throw Failure("unsupported host key algorithm; " + fingerprint);
        }
        auto *raw = libssh2_knownhost_init(session_);
        if (!raw) throw Failure("knownhost_init failed");
        std::unique_ptr<LIBSSH2_KNOWNHOSTS, decltype(&libssh2_knownhost_free)> hosts(raw, libssh2_knownhost_free);
        std::istringstream input(readFile(options_.knownHosts, 16 * 1024 * 1024));
        std::string line;
        while (std::getline(input, line)) {
            auto p = line.find_first_not_of(" \t\r");
            if (p == std::string::npos || line[p] == '#') continue;
            if (line[p] == '@') throw Failure("known_hosts markers/certificates unsupported; " + fingerprint);
            check(libssh2_knownhost_readline(raw, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH), "known_hosts parse");
        }
        // Exact host:port identity; never alias or default-port fallback at a non-22 port.
        std::string lookup = options_.port == 22 ? options_.host :
                             "[" + options_.host + "]:" + std::to_string(options_.port);
        int rc = libssh2_knownhost_check(raw, lookup.c_str(), key, keyLength,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | algorithm, nullptr);
        if (rc != LIBSSH2_KNOWNHOST_CHECK_MATCH)
            throw Failure(std::string(rc == LIBSSH2_KNOWNHOST_CHECK_MISMATCH ? "HostKeyChanged" :
                                      rc == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND ? "HostKeyUnknown" : "HostKeyCheckFailed") +
                          " for " + lookup + "; " + fingerprint);
    }
    void authenticate(const std::string &agentPath) {
        control_.set(Phase::Auth);
        const auto until = deadline();
        if (!options_.identity.empty()) {
            check(retry([&] { return libssh2_userauth_publickey_frommemory(session_, options_.user.c_str(),
                options_.user.size(), nullptr, 0, key_.data(), key_.size(), ""); }, until),
                "key authentication (encrypted/unsupported keys and interactive authentication are not supported)");
        } else {
            agent_ = libssh2_agent_init(session_);
            if (!agent_) throw Failure("agent_init failed");
            // Explicit path prevents Pageant/environment fallback and tests the native OpenSSH pipe.
            libssh2_agent_set_identity_path(agent_, agentPath.c_str());
            check(retry([&] { return libssh2_agent_connect(agent_); }, until), "Windows OpenSSH agent connect");
            check(retry([&] { return libssh2_agent_list_identities(agent_); }, until), "agent list identities");
            libssh2_agent_publickey *identity = nullptr, *previous = nullptr;
            bool success = false;
            for (unsigned tried = 0; tried < 64; ++tried) {
                int rc = libssh2_agent_get_identity(agent_, &identity, previous);
                if (rc == 1) break;
                check(rc, "agent get identity");
                rc = retry([&] { return libssh2_agent_userauth(agent_, options_.user.c_str(), identity); }, until);
                if (!rc) { success = true; break; }
                if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED && rc != LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
                    check(rc, "agent userauth");
                previous = identity;
            }
            if (!success) throw Failure("agent authentication failed: no accepted identity (password/keyboard-interactive unsupported)");
        }
        authenticated = libssh2_userauth_authenticated(session_) != 0;
        if (!authenticated) throw Failure("authentication returned without success");
    }
    void sendSize(SizeRequest request, bool initial) {
        const auto until = deadline(); // keep immutable arguments across every EAGAIN
        if (initial) {
            // Explicit ECHO=0 and OPOST=0. Keep canonical input for the login shell.
            // Raw mode is entered by the first framed command, before the byte test.
            const char modes[] = {53, 0, 0, 0, 0, 70, 0, 0, 0, 0, 0};
            check(retry([&] { return libssh2_channel_request_pty_ex(channel_, "xterm", 5, modes, sizeof modes,
                request.grid.cols, request.grid.rows, 0, 0); }, until), "pty-req");
        } else {
            // libssh2 1.11.1 channel.c emits window-change with want_reply=0.
            check(retry([&] { return libssh2_channel_request_pty_size_ex(channel_,
                request.grid.cols, request.grid.rows, 0, 0); }, until), "window-change");
        }
        sizes_.sent = request;
        std::fprintf(stderr, "[ssh-a] %s cols=%d rows=%d generation=%llu (local send only)\n",
            initial ? "pty-req" : "window-change", request.grid.cols, request.grid.rows,
            static_cast<unsigned long long>(request.generation));
    }
    void write(const std::string &data) {
        const auto until = deadline(); size_t offset = 0;
        while (offset < data.size()) {
            checkpoint();
            auto n = libssh2_channel_write(channel_, data.data() + offset, data.size() - offset);
            if (n == LIBSSH2_ERROR_EAGAIN) { waitSocket(until); continue; }
            if (n < 0) check(static_cast<int>(n), "channel write");
            if (!n) throw Failure("channel write made no progress");
            offset += static_cast<size_t>(n);
        }
    }
    size_t read(std::string *capture) {
        char buffer[4096]; size_t total = 0;
        // Bounded batches prevent a busy remote starving cancellation and close.
        for (int stream = 0; stream <= 1; ++stream) {
            for (int batch = 0; batch < 8; ++batch) {
                checkpoint();
                auto n = libssh2_channel_read_ex(channel_, stream, buffer, sizeof buffer);
                if (n == LIBSSH2_ERROR_EAGAIN || !n) break;
                if (n < 0) check(static_cast<int>(n), "channel read");
                total += static_cast<size_t>(n);
                if (stream && n) throw Failure("unexpected SSH extended-data from PTY shell");
                if (capture) {
                    capture->append(buffer, static_cast<size_t>(n));
                    if (capture->size() > kMaxCapture) throw Failure("capture exceeded 1 MiB");
                }
            }
        }
        return total;
    }
public:
    bool authenticated = false, hardSocket = false;
    uint64_t closeMs = 0, floodBytes = 0;
    explicit Session(const Options &o, Control &c) : options_(o), control_(c), sizes_(o.grid) {}
    ~Session() {
        if (!closed_) hardFail("session destroyed before bounded close");
        if (!key_.empty()) OPENSSL_cleanse(key_.data(), key_.size());
    }
    void start(const std::string &agentPath = kAgentPipe, bool authOnly = false) {
        if (!options_.identity.empty()) { key_ = readFile(options_.identity, 4 * 1024 * 1024); validateKey(key_); }
        // Deterministic Starting input, in arrival order; final desired goes into pty-req.
        uint64_t generation = 1;
        for (auto g : options_.startingSizes) sizes_.resize(g, ++generation);
        connectSocket();
        session_ = libssh2_session_init();
        if (!session_) throw Failure("session_init failed");
        libssh2_session_set_blocking(session_, 0);
        control_.set(Phase::Handshake);
        check(retry([&] { return libssh2_session_handshake(session_, socket_); }, deadline()), "SSH handshake");
        handshaken_ = true;
        verifyHost();
        authenticate(agentPath);
        if (authOnly) return;
        control_.set(Phase::Channel);
        const auto until = deadline();
        while (!channel_) {
            checkpoint();
            channel_ = libssh2_channel_open_ex(session_, "session", 7, 1024 * 1024, 32768, nullptr, 0);
            if (!channel_) {
                check(libssh2_session_last_errno(session_) == LIBSSH2_ERROR_EAGAIN ? 0 : libssh2_session_last_errno(session_), "open channel");
                waitSocket(until);
            }
        }
        control_.set(Phase::Pty);
        sendSize(sizes_.desired, true);
        while (sizes_.sent.generation < sizes_.desired.generation) sendSize(sizes_.desired, false);
        control_.set(Phase::Shell);
        check(retry([&] { return libssh2_channel_shell(channel_); }, deadline()), "shell request");
        control_.set(Phase::Running);
    }
    Grid initialSize() const { return sizes_.sent.grid; }
    void resize(Grid g) {
        sizes_.resize(g, sizes_.desired.generation + 1);
        sendSize(sizes_.desired, false);
    }
    std::string command(const std::string &body) {
        auto id = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++serial_);
        Frame f{"\036SSH_A_BEGIN_" + id + "\037", "\036SSH_A_END_" + id + "\037"};
        // POSIX shell, on the same channel. No terminal parser consumes ESC/DCS/OSC.
        std::string script = "printf '" + octal(f.begin) + "'; { " + body + "; }; "
            "__ssh_a_status=$?; printf '" + octal(f.end) + "%s\\n' \"$__ssh_a_status\"\n";
        write(script);
        std::string capture;
        const auto until = deadline();
        while (!f.complete(capture)) {
            if (!read(&capture)) waitSocket(until);
            if (now() >= until) throw Failure("command response timeout");
            if (libssh2_channel_eof(channel_) && !f.complete(capture)) {
                int status = libssh2_channel_get_exit_status(channel_);
                throw Failure("remote EOF; raw exit-status=" + std::to_string(status));
            }
        }
        return f.payload(capture);
    }
    void flood() {
        // Establish a clean raw shell first, then exec a foreground infinite producer.
        command("stty raw -echo -opost && printf ready");
        write("exec yes SSH_A_FLOOD\n");
        const auto until = deadline();
        while (floodBytes < 1024 * 1024) {
            floodBytes += read(nullptr);
            if (now() >= until || libssh2_channel_eof(channel_)) throw Failure("flood did not reach 1 MiB");
            waitSocket(until);
        }
        // Stop application reads while the peer continues producing (bounded SSH
        // window); this covers close with outstanding output/backpressure.
        const auto pauseUntil = now() + 150;
        while (now() < pauseUntil) { checkpoint(); Sleep(5); }
    }
    void close() {
        if (closed_) return;
        const auto started = control_.cancelAt.load() ? control_.cancelAt.load() : now();
        uint64_t zero = 0;
        control_.cancelAt.compare_exchange_strong(zero, started);
        sizes_.closing = true;
        control_.set(Phase::Closing);
        const auto until = started + kProtocolCloseMs;
        bool protocolOk = true;
        auto bounded = [&](auto call) {
            while (now() < until) {
                int rc = call();
                if (rc == 0) return true;
                if (rc != LIBSSH2_ERROR_EAGAIN) return false;
                try { waitSocket(until, true); } catch (...) { return false; }
            }
            return false;
        };
        if (agent_) { libssh2_agent_disconnect(agent_); libssh2_agent_free(agent_); agent_ = nullptr; }
        if (channel_) {
            protocolOk = bounded([&] { return libssh2_channel_send_eof(channel_); });
            if (protocolOk) protocolOk = bounded([&] { return libssh2_channel_close(channel_); });
            std::fprintf(stderr, "[ssh-a] raw remote exit-status=%d (library value; may be absent on forced close; 255 is not mapped)\n",
                         libssh2_channel_get_exit_status(channel_));
        }
        if (session_ && handshaken_ && protocolOk)
            protocolOk = bounded([&] { return libssh2_session_disconnect_ex(session_, SSH_DISCONNECT_BY_APPLICATION, "ssh_a_probe close", ""); });
        hardSocket = !protocolOk || now() >= until;
        if (socket_ != INVALID_SOCKET) { shutdown(socket_, SD_BOTH); closesocket(socket_); socket_ = INVALID_SOCKET; }
        // Socket is gone, blocking mode remains OFF; only the owning worker frees.
        if (session_) {
            int rc;
            do {
                rc = libssh2_session_free(session_);
                if (rc == LIBSSH2_ERROR_EAGAIN) Sleep(1);
            } while (rc == LIBSSH2_ERROR_EAGAIN && now() < started + 2900);
            if (rc != 0) hardFail("session_free failed after socket close");
            session_ = nullptr; channel_ = nullptr;
        }
        closeMs = now() - started;
        closed_ = true;
        control_.set(Phase::Closed);
    }
};

// The main thread can cancel synchronous named-pipe I/O, but never calls any
// libssh2/session API or frees worker-owned state. The process watchdog makes a
// missed deadline an observable FAIL, even when the dependency itself is stuck.
uint64_t supervised(const Options &o, Control &c, bool authCancellation, const std::function<void()> &body) {
    const auto began = now();
    Thread worker(body);
    for (;;) {
        if (worker.done(5)) break;
        if (interrupted) c.cancel();
        auto signAt = c.stalledSignAt.load();
        if (authCancellation && signAt && now() >= signAt + 100) c.cancel();
        if (c.phase == Phase::Auth && now() >= c.phaseAt + o.timeoutMs) c.cancel();
        if (c.cancelAt) {
            if (worker.cancelSync()) ++c.cancellations;
            if (now() - c.cancelAt >= kCloseBudgetMs && !worker.done())
                hardFail("close budget exceeded 3000 ms; worker did not join");
        }
        if (now() - began > uint64_t(o.timeoutMs) * 12 + kCloseBudgetMs)
            hardFail("scenario deadline exceeded (including DNS); no detached worker");
    }
    return c.cancelAt ? now() - c.cancelAt.load() : 0;
}
void runMain(const Options &o, Results &results) {
    Control c;
    std::string active = "auth";
    auto joinedMs = supervised(o, c, false, [&] {
        Session s(o, c);
        try {
            s.start(kAgentPipe, o.test == "auth");
            if (results.contains("auth")) record(results, "auth", true,
                o.identity.empty() ? "Windows OpenSSH agent authenticated; one session" : "explicit unencrypted key authenticated (agent gate not exercised)");
            if (o.test != "auth") {
                // FIRST stty size: no resize is sent before this check, even in --test resize.
                active = "stty";
                auto text = s.command("stty size");
                const auto initial = s.initialSize();
                if (!sizeMatches(text, initial)) throw Failure("first stty size mismatch, expected rows=" +
                    std::to_string(initial.rows) + " cols=" + std::to_string(initial.cols));
                if (results.contains("stty")) record(results, "stty", true, "first stty size=" +
                    std::to_string(initial.rows) + " " + std::to_string(initial.cols));
                // Clear prompts, echo, newline mapping before the exact binary frame.
                s.command("stty raw -echo -opost && printf ready");
                if (results.contains("bytes")) {
                    active = "bytes";
                    auto bytes = s.command(kR18Command);
                    auto mismatch = compareBytes(bytes);
                    if (!mismatch.empty()) throw Failure(mismatch);
                    record(results, "bytes", true, "79/79 bytes identical, including unterminated DCS, Sixel and OSC 52 query/write");
                }
                if (results.contains("resize")) {
                    active = "resize";
                    // Multiple actual window-change packets, not just one coalesced request.
                    for (int i = 0; i < 32; ++i) s.resize({80 + (i * 17) % 101, 24 + (i * 7) % 41});
                    const Grid final{137, 43}; s.resize(final);
                    auto actual = s.command("stty size");
                    if (!sizeMatches(actual, final)) throw Failure("final stty size mismatch after 33 window-change requests; expected 43 137");
                    record(results, "resize", true, "33 window-change requests; final stty size=43 137, same session/channel");
                }
                if (results.contains("close-flood")) {
                    active = "close-flood";
                    s.flood();
                    c.cancel();
                    s.close();
                    record(results, "close-flood", s.closeMs <= kCloseBudgetMs,
                        "received=" + std::to_string(s.floodBytes) + " bytes, close=" + std::to_string(s.closeMs) +
                        " ms, socket-hard-cut=" + (s.hardSocket ? "yes" : "no"));
                }
            }
        } catch (const Cancelled &) {
            if (results.contains(active)) record(results, active, false, "cancelled/operation timeout");
            else record(results, o.test, false, "prerequisite " + active + " cancelled");
        } catch (const std::exception &e) {
            if (results.contains(active)) record(results, active, false, e.what());
            else record(results, o.test, false, "prerequisite " + active + ": " + e.what());
        }
        s.close();
        if (s.closeMs > kCloseBudgetMs) record(results, "teardown", false, "close exceeded 3000 ms");
    });
    if (results.contains("close-flood") && results.at("close-flood").recorded) {
        auto &result = results.at("close-flood");
        result.pass = result.pass && joinedMs <= kCloseBudgetMs;
        result.reason += ", owner joined=" + std::to_string(joinedMs) + " ms";
    } else if (joinedMs > kCloseBudgetMs) {
        record(results, "teardown", false, "owner join exceeded 3000 ms");
    }
}
void runCloseAuth(const Options &o, Results &results) {
    Control c;
    uint64_t closeMs = 0;
    bool cancelled = false, authenticated = false, hardSocket = false;
    std::string failure;
    supervised(o, c, true, [&] {
        // relay outlives Session::close and stays connected during cancellation.
        std::unique_ptr<StallingAgent> relay;
        Session s(o, c);
        try {
            relay = std::make_unique<StallingAgent>(c);
            s.start(relay->path, true);
            failure = "authentication unexpectedly completed without a held SIGN_REQUEST";
        } catch (const Cancelled &) { cancelled = true; }
          catch (const std::exception &e) { failure = e.what(); }
        authenticated = s.authenticated;
        s.close(); closeMs = s.closeMs; hardSocket = s.hardSocket;
        // Clean up the relay before publishing completion; its join counts too.
        relay.reset();
    });
    if (c.cancelAt) closeMs = now() - c.cancelAt;
    if (failure.empty()) {
        if (interrupted) failure = "interrupted by user";
        else if (!c.stalledSignAt) failure = "did not reach agent SIGN_REQUEST; signing-wait cancellation was not covered";
        else if (!cancelled) failure = "owner did not observe cancellation";
        else if (authenticated) failure = "authentication completed before cancellation";
        else if (!c.cancellations) failure = "CancelSynchronousIo did not hit pending synchronous I/O";
        else if (closeMs > kCloseBudgetMs) failure = "owner/relay join exceeded 3000 ms";
    }
    bool pass = failure.empty() && cancelled && !authenticated && c.stalledSignAt &&
                c.cancellations > 0 && closeMs <= kCloseBudgetMs && !interrupted;
    record(results, "close-auth", pass, failure.empty() ?
        "held agent SIGN_REQUEST; CancelSynchronousIo successes=" + std::to_string(c.cancellations.load()) +
        ", owner+relay joined in " + std::to_string(closeMs) + " ms, socket-hard-cut=" + (hardSocket ? "yes" : "no") :
        failure + "; close=" + std::to_string(closeMs) + " ms, CancelSynchronousIo successes=" + std::to_string(c.cancellations.load()));
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        const auto o = parse(argc, argv);
        std::printf("[ssh-a] libssh2=%s, OpenSSL=%s; tests=%s\n", libssh2_version(0), OPENSSL_VERSION_TEXT, o.test.c_str());
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw Failure("WSAStartup failed");
        if (libssh2_init(0)) { WSACleanup(); throw Failure("libssh2_init failed"); }
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        Results results;
        const std::vector<std::string> order{"auth", "bytes", "stty", "resize", "close-auth", "close-flood"};
        for (const auto &name : order) if (o.test == "all" || o.test == name) results[name] = {};
        if (o.test != "close-auth") runMain(o, results);
        if (results.contains("close-auth")) runCloseAuth(o, results);
        bool pass = !interrupted;
        for (auto &[name, result] : results) {
            if (!result.recorded) result.reason = "not run: prerequisite failed";
            std::printf("%s %s: %s\n", result.pass ? "PASS" : "FAIL", name.c_str(), result.reason.c_str());
            pass = pass && result.pass;
        }
        std::printf("%s ssh_a_probe (%s)\n", pass ? "PASS" : "FAIL", o.test.c_str());
        SetConsoleCtrlHandler(consoleHandler, FALSE);
        libssh2_exit(); WSACleanup();
        return pass ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("FAIL ssh_a_probe: %s\n", e.what());
        return 1;
    }
}
