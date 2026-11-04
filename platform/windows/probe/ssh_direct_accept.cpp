// SPDX-License-Identifier: GPL-3.0-or-later
// S5 Windows acceptance carrier: actual product transport, one shell/channel
// for the combined six (close-auth needs a separate interrupted attempt).
// Env/factory selection, ssh.exe/snapshot checks and compat modes comparison
// belong to the S6 shell hand tests, not to this executable.
#include "direct_ssh_internal.h" // read-only TestPeer for the pause assertion
#include "ssh_a_stalling_agent.h"
#include "ssh_direct_accept_logic.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

namespace it2w {
// Observation only. All transport operations and ALL received bytes use the
// public API. A quiet socket alone cannot prove OutputQueue actually paused.
struct DirectSshTransportTestPeer {
    static std::pair<bool, size_t> outputStatus(const DirectSshTransport &t) {
        direct_ssh::Lock guard(t.impl_->lock);
        return {t.impl_->output.readingPaused(), t.impl_->output.bytes()};
    }
};
} // namespace it2w

namespace {
using namespace it2w;
using namespace ssh_a_probe;
using namespace ssh_direct_accept;
constexpr uint64_t kCloseBudgetMs = 3000;
constexpr size_t kCaptureLimit = 1024 * 1024;
uint64_t tick() { return GetTickCount64(); }
std::atomic<bool> interrupted{false};
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { interrupted = true; return TRUE; }
    return FALSE;
}
// Harness watchdog only: report failure instead of hanging in a destructor if
// the product/relay misses its join deadline. No product behavior is injected.
[[noreturn]] void watchdogFailure(const char *why) {
    std::printf("FAIL ssh_direct_accept watchdog: %s\n", why);
    std::fflush(stdout); std::fflush(stderr);
    ExitProcess(1);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
    ~Handle() { if (valid()) CloseHandle(value); }
};
class RelayThread {
    std::function<void()> body_;
    Handle handle_;
    static DWORD WINAPI entry(void *p) {
        try { static_cast<RelayThread *>(p)->body_(); }
        catch (...) { watchdogFailure("uncaught relay exception"); }
        return 0;
    }
public:
    explicit RelayThread(std::function<void()> body) : body_(std::move(body)),
        handle_(CreateThread(nullptr, 0, entry, this, 0, nullptr)) {
        if (!handle_.valid()) throw Failure("CreateThread(relay) failed");
    }
    bool done(DWORD ms = 0) const { return WaitForSingleObject(handle_.value, ms) == WAIT_OBJECT_0; }
    bool cancelSync() const { return CancelSynchronousIo(handle_.value) != FALSE; }
    ~RelayThread() { if (!done()) watchdogFailure("relay outlived its owner"); }
};
struct RelayControl { std::atomic<uint64_t> stalledSignAt{0}; };
using AcceptStallingAgent = StallingAgent<RelayControl, Handle, RelayThread, watchdogFailure>;

std::string utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw Failure("invalid Unicode argument");
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, result.data(), n, nullptr, nullptr);
    result.pop_back(); return result;
}
struct Options {
    std::string host, user, knownHosts, test = "all";
    int port = 22, timeoutMs = 15000;
    Grid grid;
    std::vector<Grid> startingSizes;
};
void usage() {
    std::puts("ssh_direct_accept --host H --user U --known-hosts PATH [--port 22]\n"
        "  [--cols 80 --rows 24] [--starting-size COLSxROWS ...] [--timeout-ms 15000]\n"
        "  [--test all|auth|bytes|stty|resize|close-auth|close-flood|hostkey-type|\n"
        "          hostkey-unknown|hostkey-changed|exit-255|dns-cancel|modes|gate]\n"
        "Agent only (SSH_AUTH_SOCK named pipe or Windows OpenSSH default); no identity option.\n"
        "all: auth/stty/modes/bytes/resize/close-flood share one session/channel;\n"
        "     close-auth uses a separate attempt through the stalling agent.\n"
        "The shared relay enumerates the default Windows OpenSSH agent, as in the probe.\n"
        "Starting resizes run in the Starting callback, before network I/O. Defaults:\n"
        "101x31, 123x37, initialSize; explicit --starting-size replaces this sequence.\n"
        "A stale generation with a different size is always injected afterwards.\n"
        "hostkey-type: supply a target ed25519-only or ECDSA-only known_hosts file;\n"
        "rerun with both files. With ed25519-only + an ECDSA-only server, require\n"
        "HostKeyUnknown and both types in detail. This tool cannot reconfigure sshd.\n"
        "hostkey-unknown/changed: supply the corresponding prebuilt known_hosts fixture.\n"
        "dns-cancel: --host must name a lab DNS lookup that stays pending >100 ms.\n"
        "gate: local Interactive/KeyFile rejection only; environment/factory, ssh.exe,\n"
        "snapshot/process and compat modes comparison are S6 shell hand tests.\n"
        "known_hosts is required, read-only and SHA256 checked after network tests.\n"
        "Output API returns whole chunks; random consumer reads slice actual popped chunks.");
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
            if (v != "agent") throw Failure("only --auth agent is supported");
        } else throw Failure("unknown option: " + a);
    }
    if (o.host.empty() || o.user.empty() || o.knownHosts.empty())
        throw Failure("--host, --user and --known-hosts are required (known_hosts is read-only)");
    if (o.host.front() == '-' || o.host.find_first_of(" \t\r\n[]") != std::string::npos)
        throw Failure("--host must be a host name or bare numeric address, without brackets");
    if (o.user.find_first_of("\r\n") != std::string::npos) throw Failure("invalid --user");
    const std::vector<std::string> tests{"all", "auth", "bytes", "stty", "resize", "close-auth", "close-flood",
        "hostkey-type", "hostkey-unknown", "hostkey-changed", "exit-255", "dns-cancel", "modes", "gate"};
    if (std::find(tests.begin(), tests.end(), o.test) == tests.end()) throw Failure("unknown --test: " + o.test);
    o.grid = clamp(o.grid);
    // Product validation requires absolute paths. Resolve relative CLI paths,
    // as the probe permits them, without creating or opening the file for write.
    o.knownHosts = utf8(std::filesystem::absolute(std::filesystem::path(widen(o.knownHosts))).c_str());
    return o;
}
SshLaunchOptions launch(const Options &o) {
    SshLaunchOptions result;
    result.target.hostName = o.host;
    result.target.user = o.user;
    result.target.port = static_cast<uint16_t>(o.port);
    result.target.appKnownHostsFile = o.knownHosts;
    // Required by common validation, but direct SSH never opens this compat file.
    result.target.emptyKnownHostsFile = o.knownHosts;
    result.target.connectTimeoutSec = static_cast<uint32_t>((o.timeoutMs + 999) / 1000);
    result.initialSize = {static_cast<uint16_t>(o.grid.cols), static_cast<uint16_t>(o.grid.rows)};
    return result;
}
std::string readKnownHosts(const Options &o) {
    std::ifstream file(std::filesystem::path(widen(o.knownHosts)), std::ios::binary);
    if (!file) throw Failure("cannot open known_hosts: " + o.knownHosts);
    std::string text;
    char block[4096];
    while (file.read(block, sizeof block) || file.gcount()) {
        text.append(block, static_cast<size_t>(file.gcount()));
        if (text.size() > 16u * 1024 * 1024) throw Failure("known_hosts exceeds 16 MiB");
    }
    if (!file.eof()) throw Failure("cannot read known_hosts");
    return text;
}
std::string sha256(std::string_view text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (!EVP_Digest(text.data(), text.size(), digest, &length, EVP_sha256(), nullptr))
        throw Failure("known_hosts SHA256 failed");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}
std::optional<DirectSha1> hmacSha1(std::string_view salt, std::string_view host) {
    DirectSha1 digest{};
    unsigned length = 0;
    if (!HMAC(EVP_sha1(), salt.data(), static_cast<int>(salt.size()),
        reinterpret_cast<const unsigned char *>(host.data()), host.size(), digest.data(), &length) ||
        length != digest.size()) return std::nullopt;
    return digest;
}
struct Result { bool pass = false, recorded = false; std::string detail; };
using Results = std::map<std::string, Result>;
void pass(Results &r, const std::string &name, std::string detail) { r[name] = {true, true, std::move(detail)}; }
void fail(Results &r, const std::string &name, std::string detail) { r[name] = {false, true, std::move(detail)}; }
void skip(const char *name, const char *why) { std::printf("SKIP %s: %s\n", name, why); }

struct Observation {
    bool running = false, terminalOrder = true, dnsSeen = false;
    unsigned finishedCount = 0, stateCount = 0, outputSignals = 0;
    std::optional<SshExitInfo> exit;
    std::vector<DiagRecord> diagnostics;
    std::string phase;
};
class Attempt {
    const Options &options_;
    mutable std::mutex mutex_;
    Observation observation_;
    Handle changed_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::vector<Grid> starting_;
    OutputSlices slices_;
    unsigned serial_ = 0;
    std::mt19937 random_{0x535348};
public:
    std::unique_ptr<DirectSshTransport> transport;
    uint64_t generation = 0;
    explicit Attempt(const Options &o, DirectSshTestHooks hooks = {}, bool merge = false) : options_(o) {
        if (!changed_.valid()) throw Failure("CreateEvent(harness) failed");
        if (merge) {
            starting_ = o.startingSizes.empty() ? std::vector<Grid>{{101, 31}, {123, 37}, o.grid} : o.startingSizes;
            generation = starting_.size();
        }
        SshCallbacks callbacks;
        callbacks.onStateChanged = [this](SessionId, AttemptId, TransportState state, std::optional<TransportFault>) {
            if (state == TransportState::Starting && !starting_.empty()) {
                uint64_t gen = 0;
                for (auto grid : starting_) transport->resize(apiGrid(grid), ++gen);
                // Distinct stale size: accepting a regressed generation must
                // change the first stty result and fail the assertion.
                const auto final = starting_.back();
                transport->resize(apiGrid({final.cols == 137 ? 138 : 137, final.rows == 43 ? 44 : 43}), gen - 1);
            }
            {
                std::lock_guard guard(mutex_);
                ++observation_.stateCount;
                if (state == TransportState::Running) observation_.running = true;
                if (state == TransportState::Closed || state == TransportState::Failed)
                    observation_.terminalOrder = observation_.finishedCount == 1;
            }
            SetEvent(changed_.value);
        };
        callbacks.onFinished = [this](SessionId, AttemptId, const SshExitInfo &exit) {
            { std::lock_guard guard(mutex_); ++observation_.finishedCount; observation_.exit = exit; }
            SetEvent(changed_.value);
        };
        callbacks.onOutputAvailable = [this](SessionId, AttemptId) {
            { std::lock_guard guard(mutex_); ++observation_.outputSignals; }
            SetEvent(changed_.value);
        };
        callbacks.onDiagnostic = [this](SessionId, AttemptId, const DiagRecord &record) {
            {
                std::lock_guard guard(mutex_);
                observation_.diagnostics.push_back(record);
                if (record.stage == "ssh.direct.phase") {
                    observation_.phase = record.detail;
                    if (record.detail == "dns") observation_.dnsSeen = true;
                }
            }
            std::fprintf(stderr, "[ssh-direct] %s ms=%llu bytes=%llu %s\n", record.stage.c_str(),
                static_cast<unsigned long long>(record.elapsedMs), static_cast<unsigned long long>(record.bytes), record.detail.c_str());
            SetEvent(changed_.value);
        };
        transport = std::make_unique<DirectSshTransport>(std::move(callbacks), std::move(hooks));
    }
    ~Attempt() {
        transport->close(CloseReason::User);
        if (!transport->waitClosed(3000)) watchdogFailure("owner did not join during teardown (3000 ms)");
    }
    static GridSize apiGrid(Grid grid) { return {static_cast<uint16_t>(grid.cols), static_cast<uint16_t>(grid.rows)}; }
    Observation observe() const { std::lock_guard guard(mutex_); return observation_; }
    std::optional<DiagRecord> diagnostic(const char *stage) const {
        auto seen = observe();
        for (auto i = seen.diagnostics.rbegin(); i != seen.diagnostics.rend(); ++i)
            if (i->stage == stage) return *i;
        return std::nullopt;
    }
    void pause() const { WaitForSingleObject(changed_.value, 5); }
    template<class Predicate> void until(Predicate predicate, const char *why) const {
        const auto deadline = tick() + options_.timeoutMs;
        while (!predicate()) {
            if (interrupted) throw Failure("interrupted by user");
            if (tick() >= deadline) throw Failure(why);
            pause();
        }
    }
    void start() {
        static std::atomic<uint64_t> serial{0};
        if (auto error = transport->start({1}, {++serial}, launch(options_)))
            throw Failure("start SnapshotError[" + error->field + "]: " + error->reason);
    }
    void running() const {
        until([&] { auto s = observe(); return s.running || s.exit.has_value(); }, "Running timeout");
        auto seen = observe();
        if (!seen.running || seen.finishedCount || transport->state() != TransportState::Running)
            throw Failure("did not remain Running: " + (seen.exit ? std::string(sshErrorName(seen.exit->error)) + ": " + seen.exit->detail : "state changed"));
    }
    void finished() const {
        until([&] { return observe().exit.has_value(); }, "onFinished timeout");
        if (!transport->waitClosed(3000)) watchdogFailure("finished owner did not join (3000 ms)");
        const auto seen = observe();
        if (seen.finishedCount != 1 || !seen.terminalOrder) throw Failure("terminal callback order/count incorrect");
    }
    void send(const std::string &bytes) {
        if (transport->enqueueUserInput(bytes) != EnqueueResult::Ok) throw Failure("enqueueUserInput rejected command");
    }
    std::string pop(bool randomSizes = false) {
        if (slices_.empty()) {
            auto chunk = transport->tryPopOutput();
            if (!chunk) return {};
            if (chunk->incompleteAfterThis) throw Failure("OutputQueue discarded bytes");
            slices_.put(std::move(chunk->bytes));
        }
        return slices_.pop(randomSizes ? 1 + random_() % 97 : std::numeric_limits<size_t>::max());
    }
    std::string command(const std::string &body, bool randomSizes = false) {
        const auto id = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++serial_);
        Frame frame{"\036SSH_DIRECT_BEGIN_" + id + "\037", "\036SSH_DIRECT_END_" + id + "\037"};
        send("printf '" + octal(frame.begin) + "'; { " + body + "; }; __ssh_direct_status=$?; printf '" +
            octal(frame.end) + "%s\\n' \"$__ssh_direct_status\"\n");
        std::string capture;
        until([&] {
            capture += pop(randomSizes);
            if (capture.size() > kCaptureLimit) throw Failure("command capture exceeded 1 MiB");
            if (frame.complete(capture)) return true;
            if (observe().exit) throw Failure("onFinished before command frame completed");
            return false;
        }, "command response timeout");
        return framePayload(frame, std::move(capture));
    }
    Grid expectedInitial() const { return starting_.empty() ? options_.grid : starting_.back(); }
    void checkInitialSize() {
        if (!sizeMatches(command("stty size"), expectedInitial())) throw Failure("first stty size != last Starting/initial size");
        const auto seen = observe();
        unsigned ptys = 0;
        const auto expected = expectedInitial();
        for (const auto &d : seen.diagnostics) if (d.stage == "ssh.direct.size" && d.detail.starts_with("pty-req ")) {
            ++ptys;
            if (d.bytes != generation || d.detail != "pty-req " + std::to_string(expected.cols) + "x" + std::to_string(expected.rows))
                throw Failure("pty-req used an intermediate size/generation (Starting merge/regression)");
        }
        if (ptys != 1) throw Failure("expected exactly one pty-req diagnostic");
    }
    uint64_t closeUser() {
        const auto began = tick();
        transport->close(CloseReason::User);
        if (!transport->waitClosed(3000)) watchdogFailure("waitClosed(3000) failed");
        const auto elapsed = tick() - began;
        const auto seen = observe();
        if (elapsed > kCloseBudgetMs || transport->state() != TransportState::Closed ||
            seen.finishedCount != 1 || !seen.terminalOrder)
            throw Failure("close did not reach Closed with ordered callbacks within 3000 ms");
        return elapsed;
    }
};

void runLive(const Options &o, Results &results) {
    std::string active = results.contains("auth") ? "auth" : o.test;
    try {
        Attempt attempt(o, {}, true);
        attempt.start(); attempt.running();
        auto agent = attempt.diagnostic("ssh.direct.agent");
        if (!agent || agent->detail.empty() || !agent->detail.starts_with(R"(\\.\pipe\)"))
            throw Failure("missing named-pipe agent diagnostic");
        if (results.contains("auth")) {
            pass(results, "auth", "Running; onFinished absent; agent=" + agent->detail);
            std::puts("[ssh-direct] auth: rsa-sha2 signature algorithm is not exposed by product diagnostics; rerun with only an RSA agent identity for S6 coverage");
        }
        if (o.test == "auth") { attempt.closeUser(); return; }
        active = "stty";
        attempt.checkInitialSize(); // FIRST remote command, before modes/raw/Running resizes
        if (results.contains("stty")) {
            const auto initial = attempt.expectedInitial();
            pass(results, "stty", "first size=" + std::to_string(initial.rows) + " " + std::to_string(initial.cols) +
                "; Starting resizes=" + std::to_string(attempt.generation) + "; pty-req uses last generation; stale generation ignored");
        }
        if (results.contains("modes")) {
            active = "modes";
            const auto modes = attempt.command("stty -a && printf '\\nTERM=%s\\n' \"$TERM\"");
            if (!modesMatch(modes)) throw Failure("expected erase ^?, positive iutf8/echo, TERM=xterm-256color; received: " + modes);
            pass(results, "modes", "product defaults: erase ^?, iutf8, echo, TERM=xterm-256color (before raw commands)");
        }
        if (o.test == "exit-255") {
            active = "exit-255";
            attempt.send("exit 255\n"); attempt.finished();
            auto exit = attempt.observe().exit;
            if (!exit || exit->error != SshError::RemoteExit || exit->exitCode != 255 ||
                attempt.transport->state() != TransportState::Closed)
                throw Failure("expected RemoteExit(255) and Closed, received " + (exit ? std::string(sshErrorName(exit->error)) : "no exit"));
            pass(results, "exit-255", "onFinished RemoteExit(255), state Closed");
            return;
        }
        // Only the remote TEST command changes shell modes. Product pty modes
        // remain intact; modes above checks them before this binary-test setup.
        if (results.contains("bytes") || results.contains("close-flood")) {
            active = results.contains("bytes") ? "bytes" : "close-flood";
            attempt.command("stty raw -echo -opost && printf ready");
        }
        if (results.contains("bytes")) {
            active = "bytes";
            for (bool randomSizes : {false, true}) {
                const auto mismatch = compareBytes(attempt.command(kR18Command, randomSizes));
                if (!mismatch.empty()) throw Failure(mismatch + (randomSizes ? " (random consumer reads)" : " (whole API chunks)"));
            }
            if (!attempt.observe().outputSignals) throw Failure("onOutputAvailable was not delivered");
            pass(results, "bytes", "79/79 via tryPopOutput, twice: whole chunks and random 1..97-byte consumer reads (seed=0x535348)");
        }
        if (results.contains("resize")) {
            active = "resize";
            for (int i = 0; i < 33; ++i) {
                if (attempt.transport->state() != TransportState::Running) throw Failure("resize outside Running");
                const Grid grid = i == 32 ? Grid{137, 43} : Grid{80 + (i * 17) % 101, 24 + (i * 7) % 41};
                attempt.transport->resize(Attempt::apiGrid(grid), ++attempt.generation);
            }
            attempt.until([&] {
                auto d = attempt.diagnostic("ssh.direct.size");
                if (attempt.observe().exit) throw Failure("exited during resize");
                return d && d->bytes == attempt.generation && d->detail == "window-change 137x43";
            }, "final resize was not sent");
            if (!sizeMatches(attempt.command("stty size"), {137, 43})) throw Failure("final stty != 43 137 after 33 Running API resizes");
            pass(results, "resize", "33 API resizes while Running; final stty size=43 137; same session/channel");
        }
        if (results.contains("close-flood")) {
            active = "close-flood";
            attempt.send("exec yes SSH_DIRECT_FLOOD\n");
            uint64_t received = 0;
            attempt.until([&] {
                received += attempt.pop().size();
                if (attempt.observe().exit) throw Failure("exited before flood reached 1 MiB");
                return received >= 1024 * 1024;
            }, "flood did not reach 1 MiB");
            // No more pops. Observe the real OutputQueue high-water pause.
            size_t queued = 0;
            attempt.until([&] {
                if (attempt.observe().exit) throw Failure("exited before OutputQueue paused");
                const auto status = DirectSshTransportTestPeer::outputStatus(*attempt.transport);
                queued = status.second;
                return status.first && queued >= kOutputPause;
            }, "OutputQueue never paused after consumer stopped");
            const auto elapsed = attempt.closeUser();
            const auto close = attempt.diagnostic("ssh.direct.close");
            if (!close) throw Failure("missing close diagnostic");
            pass(results, "close-flood", "received=" + std::to_string(received) + "; paused queue=" + std::to_string(queued) +
                "; close+owner join=" + std::to_string(elapsed) + " ms; Closed; " + close->detail);
        } else attempt.closeUser();
    } catch (const std::exception &e) { fail(results, active, e.what()); }
}

void runCloseAuth(const Options &o, Results &results) {
    try {
        RelayControl control;
        // Declaration order keeps the relay open until the product owner joins,
        // including exception unwinding. A broken pipe cannot fake cancellation.
        auto relay = std::make_unique<AcceptStallingAgent>(control);
        Attempt attempt(o, DirectSshTestHooks{relay->path});
        attempt.start();
        attempt.until([&] {
            if (attempt.observe().exit) throw Failure("finished before held SIGN_REQUEST");
            auto stalled = control.stalledSignAt.load();
            return stalled && tick() - stalled >= 100;
        }, "did not reach held agent SIGN_REQUEST (real agent needs a server-accepted key)");
        const auto began = tick();
        attempt.closeUser();
        auto count = attempt.diagnostic("ssh.direct.cancel_sync");
        auto agent = attempt.diagnostic("ssh.direct.agent");
        auto exit = attempt.observe().exit;
        if (!agent || agent->detail != relay->path) throw Failure("hook pipe absent from diagnostic");
        if (!count || !count->bytes) throw Failure("CancelSynchronousIo successes must be >0");
        if (!exit || exit->error != SshError::None) throw Failure("cancelled auth must finish None, never AuthFailed");
        relay.reset(); // join counts against the same 3 s budget
        const auto elapsed = tick() - began;
        if (elapsed > kCloseBudgetMs) throw Failure("owner+relay join exceeded 3000 ms");
        pass(results, "close-auth", "held SIGN_REQUEST; cancel_sync=" + std::to_string(count->bytes) +
            "; owner+relay join=" + std::to_string(elapsed) + " ms; onFinished None; Closed");
    } catch (const std::exception &e) { fail(results, "close-auth", e.what()); }
}

void runHostKey(const Options &o, const std::string &knownText, Results &results) {
    try {
        std::vector<std::string> types;
        bool onlyEd = false, onlyEcdsa = false;
        if (o.test == "hostkey-type") {
            auto parsed = knownHostTypesFor(knownText, o.host, static_cast<uint16_t>(o.port), hmacSha1);
            if (parsed.error) throw Failure(parsed.error->reason);
            types = parsed.types;
            onlyEd = types == std::vector<std::string>{"ssh-ed25519"};
            onlyEcdsa = !types.empty() && std::all_of(types.begin(), types.end(), [](const auto &t) { return t.starts_with("ecdsa-sha2-"); });
            if (!onlyEd && !onlyEcdsa) throw Failure("hostkey-type needs ed25519-only or ECDSA-only entries for this target");
        }
        Attempt attempt(o);
        attempt.start();
        attempt.until([&] { auto s = attempt.observe(); return s.running || s.exit.has_value(); }, "hostkey attempt timeout");
        auto seen = attempt.observe();
        auto presented = attempt.diagnostic("ssh.direct.hostkey");
        if (o.test == "hostkey-type" && seen.running) {
            attempt.running();
            if (!presented || !std::any_of(types.begin(), types.end(), [&](const auto &t) { return presented->detail.starts_with(t + " "); }))
                throw Failure("negotiated hostkey type not present in typed known_hosts");
            attempt.closeUser();
            pass(results, o.test, "typed negotiation accepted " + presented->detail);
            skip("hostkey-type.server-only-ecdsa", "rerun with ed25519-only known_hosts and an externally configured ECDSA-only sshd; this tool cannot reconfigure the server");
            return;
        }
        if (seen.running) throw Failure("unexpected Running for a rejecting known_hosts fixture");
        attempt.finished();
        auto exit = attempt.observe().exit;
        if (attempt.transport->state() != TransportState::Failed || !exit) throw Failure("hostkey rejection did not reach Failed");
        if (o.test == "hostkey-type") {
            if (!onlyEd || !presented || !presented->detail.starts_with("ecdsa-sha2-") || exit->error != SshError::HostKeyUnknown ||
                exit->detail.find("ssh-ed25519") == std::string::npos || exit->detail.find("ecdsa-sha2-") == std::string::npos)
                throw Failure("expected typed negotiation success or cross-type HostKeyUnknown with both types: " + exit->detail);
            pass(results, o.test, "ed25519-only known_hosts + presented ECDSA -> HostKeyUnknown; both types in detail: " + exit->detail);
        } else {
            const auto expected = o.test == "hostkey-unknown" ? SshError::HostKeyUnknown : SshError::HostKeyChanged;
            if (exit->error != expected) throw Failure("expected " + std::string(sshErrorName(expected)) + ", got " + sshErrorName(exit->error) + ": " + exit->detail);
            pass(results, o.test, std::string(sshErrorName(exit->error)) + "; " + exit->detail);
        }
    } catch (const std::exception &e) { fail(results, o.test, e.what()); }
}

void runDnsCancel(const Options &o, Results &results) {
    try {
        Attempt attempt(o);
        attempt.start();
        attempt.until([&] { auto s = attempt.observe(); return s.dnsSeen || s.exit; }, "DNS phase not observed");
        auto entered = attempt.observe();
        if (!entered.dnsSeen) throw Failure("attempt failed before DNS: " + (entered.exit ? entered.exit->detail : "no DNS diagnostic"));
        const auto began = tick();
        while (tick() - began < 100 && attempt.observe().phase == "dns" && !attempt.observe().exit) {
            if (interrupted) throw Failure("interrupted by user");
            Sleep(5);
        }
        auto seen = attempt.observe();
        if (seen.phase != "dns" || seen.exit) {
            // No claim of DNS cancellation coverage when the lab name resolves
            // quickly. Teardown still runs, and a teardown failure remains FAIL.
            attempt.transport->close(CloseReason::User);
            if (!attempt.transport->waitClosed(3000)) watchdogFailure("DNS fixture teardown did not join");
            skip("dns-cancel", "--host did not stay in DNS for 100 ms; supply a deliberately slow lab DNS name");
            results.erase("dns-cancel");
            return;
        }
        const auto elapsed = attempt.closeUser();
        const auto exit = attempt.observe().exit;
        if (!exit || exit->error != SshError::None) throw Failure("slow DNS cancellation did not finish None");
        pass(results, "dns-cancel", "closed while DNS remained pending; close+owner join=" + std::to_string(elapsed) + " ms; Closed");
    } catch (const std::exception &e) { fail(results, "dns-cancel", e.what()); }
}

void runGate(const Options &o, Results &results) {
    try {
        unsigned callbacks = 0;
        SshCallbacks cb;
        cb.onStateChanged = [&](SessionId, AttemptId, TransportState, std::optional<TransportFault>) { ++callbacks; };
        cb.onFinished = [&](SessionId, AttemptId, const SshExitInfo &) { ++callbacks; };
        cb.onDiagnostic = [&](SessionId, AttemptId, const DiagRecord &) { ++callbacks; };
        cb.onOutputAvailable = [&](SessionId, AttemptId) { ++callbacks; };
        DirectSshTransport transport(std::move(cb)); // default production hooks
        auto options = launch(o);
        // Invalid target/paths ensure auth validation wins before any file or
        // DNS access. KeyFile is intentionally missing a keyFile as well.
        options.target.hostName.clear(); options.target.appKnownHostsFile.clear();
        for (auto auth : {SshAuthMode::Interactive, SshAuthMode::KeyFile}) {
            options.target.auth = auth;
            auto error = transport.start({1}, {1}, options);
            if (!error || error->field != "auth" || transport.state() != TransportState::Idle || callbacks || transport.waitClosed(0))
                throw Failure("Interactive/KeyFile must synchronously return auth SnapshotError without an owner/callbacks");
        }
        transport.close(CloseReason::User);
        if (!transport.waitClosed(0) || callbacks) throw Failure("local gate teardown failed");
        pass(results, "gate", "Interactive + KeyFile -> auth SnapshotError, Idle, zero callbacks; no network owner");
        skip("gate.shell", "environment/factory, ssh.exe processes, snapshot files and build-without-direct behavior require S6 shell hand tests");
    } catch (const std::exception &e) { fail(results, "gate", e.what()); }
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        const auto options = parse(argc, argv);
        std::printf("[ssh-direct] product API acceptance; test=%s; known_hosts=%s\n", options.test.c_str(), options.knownHosts.c_str());
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        Results results;
        if (options.test == "all") {
            for (const auto *name : {"auth", "bytes", "stty", "resize", "close-auth", "close-flood", "modes"}) results[name] = {};
        } else results[options.test] = {};
        if (options.test == "gate") runGate(options, results);
        else {
            const auto knownText = readKnownHosts(options);
            const auto before = sha256(knownText);
            if (options.test.starts_with("hostkey-")) runHostKey(options, knownText, results);
            else if (options.test == "dns-cancel") runDnsCancel(options, results);
            else {
                if (options.test != "close-auth") runLive(options, results);
                if (results.contains("close-auth")) runCloseAuth(options, results);
            }
            try {
                const auto after = sha256(readKnownHosts(options));
                if (before != after) throw Failure("known_hosts SHA256 changed");
                pass(results, "known-hosts-readonly", "SHA256 unchanged " + after);
            } catch (const std::exception &e) { fail(results, "known-hosts-readonly", e.what()); }
        }
        bool ok = !interrupted;
        for (auto &[name, result] : results) {
            if (!result.recorded) result.detail = "not run: prerequisite failed";
            std::printf("%s %s: %s\n", result.pass ? "PASS" : "FAIL", name.c_str(), result.detail.c_str());
            ok = ok && result.pass;
        }
        // A skipped DNS fixture is never mislabeled a passing DNS assertion.
        const bool skipped = options.test == "dns-cancel" && !results.contains("dns-cancel");
        std::printf("%s ssh_direct_accept (%s)\n", !ok ? "FAIL" : skipped ? "SKIP" : "PASS", options.test.c_str());
        SetConsoleCtrlHandler(consoleHandler, FALSE);
        return ok ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("FAIL ssh_direct_accept: %s\n", e.what());
        return 1;
    }
}
