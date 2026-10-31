// Portable direct-SSH policy only. No transport, OS or crypto-library dependency.
#pragma once
#include "ssh_session.h"
#include "ssh_snapshot.h"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace it2w {

struct DirectGrid {
    int cols = 80, rows = 24;
    bool operator==(const DirectGrid &) const = default;
};
struct DirectSizeRequest {
    DirectGrid grid;
    uint64_t generation = 0;
    bool operator==(const DirectSizeRequest &) const = default;
};
// Caller provides synchronization. An absent sent value means pty-req has not
// completed (generation 0 is a real launch size, never an "unsent" sentinel).
struct DirectSizes {
    DirectSizeRequest desired;
    std::optional<DirectSizeRequest> inflight, sent;
    bool closing = false;
    explicit DirectSizes(DirectGrid launch = {});
    void resize(DirectGrid grid, uint64_t generation);
    bool needsSend() const; // Drain before shell, and again after each completion.
    std::optional<DirectSizeRequest> beginSend(); // Same arguments on every retry.
    void completeSend(); // Call only on successful pty-req/window-change.
};

using DirectSha1 = std::array<uint8_t, 20>;
// salt is decoded binary (may contain NUL); host is the canonical lookup text.
// nullopt reports a crypto failure, distinct from a nonmatching host hash.
using DirectHmacSha1 = std::function<std::optional<DirectSha1>(
    std::string_view salt, std::string_view host)>;
struct KnownHostTypesResult {
    std::vector<std::string> types; // Unique key types, in file order.
    std::optional<SnapshotError> error;
};
KnownHostTypesResult knownHostTypesFor(std::string_view text, std::string_view host,
                                      uint16_t port, const DirectHmacSha1 &hmac);
// Defaults to raw Ed25519/ECDSA/RSA-SHA2. ssh-rsa records select both RSA SHA-2
// signatures. The overload accepts an adapter's available algorithms so all
// remaining supported methods are retained, without adding unavailable ones.
std::string hostKeyPref(const std::vector<std::string> &types);
std::string hostKeyPref(const std::vector<std::string> &types,
                        const std::vector<std::string> &supported);

// Exact, case-sensitive named-pipe prefix; never selects a Pageant backend.
std::string directAgentPipe(std::string_view sshAuthSock);
std::optional<SnapshotError> validateDirectAuthMode(SshAuthMode auth);

enum class DirectStage {
    Dns, Connect, Handshake, KnownHosts, HostKey, Agent, Auth,
    Channel, Pty, Shell, Running
};
// The Windows adapter must normalize WSA/libssh2 errors to these portable kinds.
enum class DirectFailureKind {
    HostNotFound, NoData, ConnectionRefused, DeadlineExpired, TimedOut,
    NetworkUnreachable, HostUnreachable, KexFailed, NoHostKeyAlgorithm,
    NoCipherAlgorithm, SocketClosed, SocketReset, KnownHostsOpenFailed,
    KnownHostsParseFailed, KnownHostsMarker, HostKeyUnknown, HostKeyChanged,
    AgentUnavailable, NoAcceptedIdentity, RequestDenied, Other
};
struct DirectFailure {
    DirectStage stage;
    DirectFailureKind kind;
    int code = 0; // Native diagnostic code only, never a remote exit status.
};
SshExitInfo mapDirectFailure(DirectFailure failure);
// Signal takes precedence over status. Common signal names have conventional
// numbers; supply a remote signo explicitly for platform-specific signals.
SshExitInfo mapChannelExit(uint32_t status, std::string_view signal = {},
                           std::optional<uint32_t> signo = std::nullopt);

inline constexpr std::string_view kDirectTerm = "xterm-256color";
std::string directPtyModes(); // Binary RFC 4254/8160 opcode + uint32 pairs.

} // namespace it2w
