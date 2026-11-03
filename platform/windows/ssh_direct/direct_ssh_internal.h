// Implementation shared by the transport, host-key and agent adapters only.
#pragma once
// Winsock must precede the Windows transport interface (which includes windows.h).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include "direct_ssh_transport.h"
#include "ssh_direct_logic.h"

namespace it2w {
namespace direct_ssh {
inline constexpr uint64_t kCloseMs = 3000, kProtocolCloseMs = 2700;
inline uint64_t now() { return GetTickCount64(); }
struct Lock {
    SRWLOCK &lock;
    explicit Lock(SRWLOCK &l) : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~Lock() { ReleaseSRWLockExclusive(&lock); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};
struct Cancelled {};
struct Failure { SshExitInfo exit; std::optional<TransportFault> fault; };
} // namespace direct_ssh

struct DirectSshTransport::Impl {
    explicit Impl(SshCallbacks, DirectSshTestHooks);
    ~Impl();
    SshCallbacks cb;
    const DirectSshTestHooks hooks;
    SessionId sid{0};
    AttemptId aid{0};
    SshTarget target;
    std::atomic<TransportState> state{TransportState::Idle};
    // lock protects publication/lifetime of thread handles, closeAt, queues,
    // sizes, and authDeadline. No wait, protocol call or callback under lock.
    mutable SRWLOCK lock = SRWLOCK_INIT;
    InputScheduler input;
    OutputQueue output;
    DirectSizes sizes;
    HANDLE wake = nullptr, closed = nullptr, owner = nullptr, canceller = nullptr;
    DWORD ownerId = 0;
    std::atomic<bool> closeRequested{false}, inAgentIo{false}, authTimedOut{false};
    std::atomic<uint64_t> cancellations{0};
    uint64_t closeAt = 0, authDeadline = 0;
    CloseReason closeReason = CloseReason::User;
    std::optional<TransportFault> requestedFault;
    // Everything below is used exclusively by the owner, apart from the timer
    // handle's destruction after joining the owner.
    PTP_TIMER authTimer = nullptr;
    SOCKET socket = INVALID_SOCKET;
    WSAEVENT socketEvent = WSA_INVALID_EVENT;
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_CHANNEL *channel = nullptr;
    LIBSSH2_AGENT *agent = nullptr;
    LIBSSH2_KNOWNHOSTS *knownHosts = nullptr;
    std::vector<std::string> knownTypes;
    std::string hostLower, writePending;
    OutputTail tail;
    std::optional<SshExitInfo> remoteExit;
    DirectStage phase = DirectStage::Dns;
    uint64_t phaseAt = 0, resizeUntil = 0;
    bool handshaken = false, agentConnected = false, authInterrupted = false, authInProgress = false;
    bool ran = false;

    static DWORD WINAPI ownerMain(void *);
    static DWORD WINAPI cancelMain(void *);
    static void CALLBACK authExpired(PTP_CALLBACK_INSTANCE, void *, PTP_TIMER);
    void requestClose(CloseReason);
    bool waitClosed(DWORD) const;
    void run();
    void open(uint64_t connectUntil);
    void connectSocket(uint64_t until);
    void prepareHostKey();
    void verifyHostKey();
    void authenticate();
    void releaseAgent();
    void running();
    bool sendSize(bool initial, uint64_t until);
    bool resizeRunning(); // one attempt per pump turn; false means EAGAIN
    bool readOutput(bool closing = false);
    SshExitInfo channelExit();
    void cleanup();
    void setState(TransportState, std::optional<TransportFault> = std::nullopt);
    void setPhase(DirectStage, const char *);
    void diag(const char *, uint64_t ms = 0, uint64_t bytes = 0, std::string detail = {},
              std::optional<int64_t> errorCode = std::nullopt) const;
    void checkpoint(uint64_t until = 0) const;
    [[noreturn]] void fail(DirectFailureKind, int code = 0, std::string detail = {},
                           std::optional<TransportFault> = std::nullopt) const;
    void check(int rc, std::string detail = {}) const;
    WSANETWORKEVENTS waitSocket(uint64_t until, bool closing = false,
                                bool connecting = false, int directions = -1);
    template<class F> int retry(F call, uint64_t until) {
        for (;;) {
            checkpoint(until);
            int rc = call();
            checkpoint(until);
            if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
            waitSocket(until);
        }
    }
};

} // namespace it2w
