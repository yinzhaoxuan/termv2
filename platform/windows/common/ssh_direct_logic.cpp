#include "ssh_direct_logic.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace it2w {
namespace {
DirectGrid clamp(DirectGrid grid) {
    return {std::max(20, grid.cols), std::max(5, grid.rows)};
}
std::string asciiLower(std::string_view text) {
    std::string result(text);
    for (char &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
std::string_view token(std::string_view &line) {
    while (!line.empty() && space(line.front())) line.remove_prefix(1);
    size_t n = 0;
    while (n < line.size() && !space(line[n])) ++n;
    auto result = line.substr(0, n);
    line.remove_prefix(n);
    return result;
}
std::optional<std::string> base64(std::string_view encoded) {
    if (encoded.empty()) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    const size_t padding = encoded.find('=');
    auto body = encoded.substr(0, padding);
    if (body.size() % 4 == 1) return std::nullopt;
    if (padding != std::string_view::npos) {
        const auto tail = encoded.substr(padding);
        if (encoded.size() % 4 || (tail != "=" && tail != "==") ||
            (tail.size() == 1 ? body.size() % 4 != 3 : body.size() % 4 != 2))
            return std::nullopt;
    }
    std::string result;
    unsigned bits = 0, count = 0;
    for (char c : body) {
        int value = digit(c);
        if (value < 0) return std::nullopt;
        bits = (bits << 6) | unsigned(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            result += char((bits >> count) & 0xff);
        }
    }
    if (count && (bits & ((1u << count) - 1))) return std::nullopt;
    if (result.empty()) return std::nullopt;
    return result;
}
const char *stageName(DirectStage stage) {
    switch (stage) {
    case DirectStage::Dns: return "DNS";
    case DirectStage::Connect: return "connect";
    case DirectStage::Handshake: return "handshake";
    case DirectStage::KnownHosts: return "known_hosts";
    case DirectStage::HostKey: return "host key";
    case DirectStage::Agent: return "agent";
    case DirectStage::Auth: return "auth";
    case DirectStage::Channel: return "channel";
    case DirectStage::Pty: return "pty-req";
    case DirectStage::Shell: return "shell";
    case DirectStage::Running: return "running";
    }
    return "unknown stage";
}
} // namespace

DirectSizes::DirectSizes(DirectGrid launch) : desired{clamp(launch), 0} {}
void DirectSizes::resize(DirectGrid grid, uint64_t generation) {
    if (!closing && generation > desired.generation) desired = {clamp(grid), generation};
}
bool DirectSizes::needsSend() const {
    return !closing && (!sent || desired.generation > sent->generation);
}
std::optional<DirectSizeRequest> DirectSizes::beginSend() {
    if (closing) return std::nullopt;
    if (!inflight && needsSend()) inflight = desired;
    return inflight;
}
void DirectSizes::completeSend() {
    if (inflight) { sent = inflight; inflight.reset(); }
}

KnownHostTypesResult knownHostTypesFor(std::string_view text, std::string_view host,
                                      uint16_t port, const DirectHmacSha1 &hmac) {
    KnownHostTypesResult result;
    size_t lineNumber = 0;
    auto fail = [&](std::string reason) {
        // Do not expose partial results after any parse/crypto failure.
        return KnownHostTypesResult{{}, SnapshotError{"appKnownHostsFile",
            "known_hosts line " + std::to_string(lineNumber) + ": " + reason}};
    };
    if (host.empty() || !port) return fail("invalid host/port");
    const std::string lowerHost = asciiLower(host);
    const std::string lookup = port == 22 ? lowerHost :
        "[" + lowerHost + "]:" + std::to_string(port);
    while (!text.empty()) {
        size_t end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++lineNumber;
        auto hosts = token(line);
        if (hosts.empty() || hosts.front() == '#') continue;
        // Reject markers anywhere in the file, just like the probe.
        if (hosts.front() == '@') return fail("markers/certificates unsupported");
        auto type = token(line);
        auto key = token(line);
        if (type.empty() || !base64(key)) return fail("invalid host/key record");
        bool matches = false;
        if (hosts.front() == '|') {
            if (!hosts.starts_with("|1|")) return fail("unsupported host hash");
            auto parts = hosts.substr(3);
            auto separator = parts.find('|');
            if (separator == std::string_view::npos) return fail("invalid host hash");
            auto salt = base64(parts.substr(0, separator));
            auto expected = base64(parts.substr(separator + 1));
            if (!salt || !expected || expected->size() != DirectSha1{}.size())
                return fail("invalid host hash");
            if (!hmac) return fail("HMAC-SHA1 callback unavailable");
            auto actual = hmac(*salt, lookup);
            if (!actual) return fail("HMAC-SHA1 failed");
            matches = std::equal(actual->begin(), actual->end(), expected->begin(),
                [](uint8_t a, char b) { return a == static_cast<unsigned char>(b); });
        } else {
            while (!hosts.empty()) {
                auto comma = hosts.find(',');
                auto name = hosts.substr(0, comma);
                if (name.empty()) return fail("empty host in list");
                if (asciiLower(name) == lookup) matches = true;
                if (comma == std::string_view::npos) break;
                hosts.remove_prefix(comma + 1);
                if (hosts.empty()) return fail("empty host in list");
            }
        }
        if (matches && std::find(result.types.begin(), result.types.end(), type) == result.types.end())
            result.types.emplace_back(type);
    }
    return result;
}

std::string hostKeyPref(const std::vector<std::string> &types) {
    return hostKeyPref(types, {
        "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256"
    });
}
std::string hostKeyPref(const std::vector<std::string> &types,
                        const std::vector<std::string> &supported) {
    constexpr std::string_view ranked[] = {
        "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256"
    };
    std::vector<std::string_view> ordered;
    for (auto algorithm : ranked)
        if (std::find(supported.begin(), supported.end(), algorithm) != supported.end())
            ordered.push_back(algorithm);
    for (const auto &algorithm : supported)
        if (std::find(ordered.begin(), ordered.end(), algorithm) == ordered.end())
            ordered.push_back(algorithm);
    auto known = [&](std::string_view algorithm) {
        if (algorithm.starts_with("rsa-sha2-") &&
            std::find(types.begin(), types.end(), "ssh-rsa") != types.end()) return true;
        return std::find(types.begin(), types.end(), algorithm) != types.end();
    };
    std::string pref;
    for (bool wantKnown : {true, false}) {
        for (auto algorithm : ordered) if (known(algorithm) == wantKnown) {
            if (!pref.empty()) pref += ',';
            pref += algorithm;
        }
    }
    return pref;
}

std::string directAgentPipe(std::string_view sshAuthSock) {
    return std::string(sshAuthSock.starts_with(R"(\\.\pipe\)") ?
        sshAuthSock : R"(\\.\pipe\openssh-ssh-agent)");
}
std::optional<SnapshotError> validateDirectAuthMode(SshAuthMode auth) {
    if (auth == SshAuthMode::Agent) return std::nullopt;
    return SnapshotError{"auth", "直连 v0 仅支持 agent；请 ssh-add 后使用，或去掉 ITERM2WIN_SSH_DIRECT 使用兼容模式"};
}

SshExitInfo mapDirectFailure(DirectFailure failure) {
    SshExitInfo result;
    result.exitCode = 255;
    const char *reason = "operation failed";
    switch (failure.kind) {
    case DirectFailureKind::HostNotFound:
    case DirectFailureKind::NoData:
        result.error = SshError::HostNotFound; reason = "host not found"; break;
    case DirectFailureKind::ConnectionRefused:
        result.error = SshError::ConnectionRefused; reason = "connection refused"; break;
    case DirectFailureKind::DeadlineExpired:
    case DirectFailureKind::TimedOut:
        result.error = SshError::Timeout; reason = "timed out"; break;
    case DirectFailureKind::NetworkUnreachable:
    case DirectFailureKind::HostUnreachable:
        result.error = SshError::NetworkUnreachable; reason = "network/host unreachable"; break;
    case DirectFailureKind::KexFailed:
        result.error = SshError::ClientIncompatible; reason = "key exchange failed"; break;
    case DirectFailureKind::NoHostKeyAlgorithm:
        result.error = SshError::ClientIncompatible; reason = "no common hostkey algorithm"; break;
    case DirectFailureKind::NoCipherAlgorithm:
        result.error = SshError::ClientIncompatible; reason = "no common cipher algorithm"; break;
    case DirectFailureKind::SocketClosed:
    case DirectFailureKind::SocketReset:
        result.error = SshError::ConnectionClosed; reason = "socket closed/reset"; break;
    case DirectFailureKind::KnownHostsOpenFailed:
        result.error = SshError::ConfigError; reason = "cannot open known_hosts"; break;
    case DirectFailureKind::KnownHostsParseFailed:
        result.error = SshError::ConfigError; reason = "cannot parse known_hosts"; break;
    case DirectFailureKind::KnownHostsMarker:
        result.error = SshError::ConfigError; reason = "known_hosts markers/certificates unsupported"; break;
    case DirectFailureKind::HostKeyUnknown:
        result.error = SshError::HostKeyUnknown;
        reason = "请先用兼容模式连接一次以确认指纹"; break;
    case DirectFailureKind::HostKeyChanged:
        result.error = SshError::HostKeyChanged; reason = "host key changed"; break;
    case DirectFailureKind::AgentUnavailable:
        result.error = SshError::AuthFailed; reason = "agent pipe unavailable"; break;
    case DirectFailureKind::NoAcceptedIdentity:
        result.error = SshError::AuthFailed; reason = "agent has no accepted identity"; break;
    case DirectFailureKind::RequestDenied:
        result.error = SshError::Unknown; reason = "request denied"; break;
    case DirectFailureKind::Other:
        result.error = failure.stage == DirectStage::KnownHosts ? SshError::ConfigError :
            (failure.stage == DirectStage::Auth || failure.stage == DirectStage::Agent) ?
                SshError::AuthFailed : SshError::Unknown;
        break;
    }
    result.detail = std::string(stageName(failure.stage)) + ": " + reason;
    if (failure.code) result.detail += " (code " + std::to_string(failure.code) + ")";
    return result;
}

SshExitInfo mapChannelExit(uint32_t status, std::string_view signal,
                           std::optional<uint32_t> signo) {
    SshExitInfo result;
    result.exitCode = status;
    result.error = status == 0 ? SshError::None : SshError::RemoteExit;
    if (!signal.empty()) {
        // These conventional numbers agree on Linux/macOS. USR1/USR2 and other
        // platform-specific numbers must come from the caller, never <signal.h>.
        constexpr std::pair<std::string_view, uint32_t> common[] = {
            {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"ILL", 4}, {"ABRT", 6},
            {"FPE", 8}, {"KILL", 9}, {"SEGV", 11}, {"PIPE", 13},
            {"ALRM", 14}, {"TERM", 15}
        };
        if (!signo) for (auto entry : common) if (entry.first == signal) signo = entry.second;
        result.error = SshError::RemoteExit;
        result.exitCode = signo && *signo > 0 && *signo <= std::numeric_limits<uint32_t>::max() - 128 ?
            128 + *signo : 255;
        result.detail = "signal " + std::string(signal);
    }
    return result;
}

std::string directPtyModes() {
    // RFC 4254 §8 / RFC 8160: uint32 values use network byte order.
    constexpr char modes[] = {
        1, 0, 0, 0, 3,    // VINTR = ^C
        3, 0, 0, 0, 127,  // VERASE = ^?
        42, 0, 0, 0, 1,   // IUTF8
        0                 // TTY_OP_END; leave ECHO/OPOST at server defaults.
    };
    return {modes, sizeof modes};
}

} // namespace it2w
