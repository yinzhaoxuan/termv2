#include "direct_ssh_internal.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <stdexcept>

// mingw-w64 的 ws2tcpip.h 未声明这两个 Win8+ API（libws2_32.a 已导出），在此补声明。
#if defined(__MINGW32__)
extern "C" {
INT WSAAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
INT WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED lpOverlapped);
}
#endif

namespace it2w {
using namespace direct_ssh;
namespace {
struct Runtime {
    int error = 0;
    bool winsock = false;
    Runtime() {
        WSADATA data{};
        error = WSAStartup(MAKEWORD(2, 2), &data);
        if (!error) { winsock = true; error = libssh2_init(0); }
    }
    ~Runtime() {
        if (!error) libssh2_exit();
        if (winsock) WSACleanup();
    }
};
int runtimeError() { static Runtime runtime; return runtime.error; }
DirectFailureKind socketFailure(int code) {
    switch (code) {
    case WSAHOST_NOT_FOUND: return DirectFailureKind::HostNotFound;
    case WSANO_DATA: return DirectFailureKind::NoData;
    case WSAECONNREFUSED: return DirectFailureKind::ConnectionRefused;
    case WSAETIMEDOUT: case WSA_E_CANCELLED: return DirectFailureKind::TimedOut;
    case WSAENETUNREACH: return DirectFailureKind::NetworkUnreachable;
    case WSAEHOSTUNREACH: return DirectFailureKind::HostUnreachable;
    case WSAECONNRESET: case WSAECONNABORTED: return DirectFailureKind::SocketReset;
    default: return DirectFailureKind::Other;
    }
}
void closeHandle(HANDLE &h) { if (h) { CloseHandle(h); h = nullptr; } }
// After shutdown, do not let session_free use a stale/reused Winsock descriptor.
ssize_t disconnectedSend(libssh2_socket_t, const void *, size_t, int, void **) { return -ECONNRESET; }
ssize_t disconnectedRecv(libssh2_socket_t, void *, size_t, int, void **) { return -ECONNRESET; }
} // namespace

DirectSshTransport::Impl::Impl(SshCallbacks callbacks, DirectSshTestHooks testHooks)
    : cb(std::move(callbacks)), hooks(std::move(testHooks)) {
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake || !closed) {
        closeHandle(wake); closeHandle(closed);
        throw std::runtime_error("DirectSshTransport: CreateEvent failed");
    }
}
DirectSshTransport::Impl::~Impl() {
    requestClose(CloseReason::User);
    waitClosed(INFINITE);
    if (canceller) WaitForSingleObject(canceller, INFINITE);
    closeHandle(canceller); closeHandle(owner); closeHandle(wake); closeHandle(closed);
}
DirectSshTransport::DirectSshTransport(SshCallbacks cb, DirectSshTestHooks hooks)
    : impl_(std::make_unique<Impl>(std::move(cb), std::move(hooks))) {}
DirectSshTransport::~DirectSshTransport() = default;

std::optional<SnapshotError> DirectSshTransport::start(SessionId sid, AttemptId aid, const SshLaunchOptions &o) {
    // No DNS, socket, libssh2, agent or file access before these synchronous checks.
    if (auto e = validateDirectAuthMode(o.target.auth)) return e;
    if (auto e = validateSshTarget(o.target)) return e;
    auto &p = *impl_;
    Lock g(p.lock);
    if (p.state != TransportState::Idle || p.closeRequested)
        return SnapshotError{"state", "直连 SSH transport already started/closed"};
    p.sid = sid; p.aid = aid; p.target = o.target;
    // Retain a resize received before start as well as those received during it.
    if (!p.sizes.desired.generation) p.sizes = DirectSizes({o.initialSize.cols, o.initialSize.rows});
    p.state = TransportState::Starting;
    p.owner = CreateThread(nullptr, 0, Impl::ownerMain, &p, 0, &p.ownerId);
    if (!p.owner) {
        p.state = TransportState::Idle;
        return SnapshotError{"thread", "直连 SSH: CreateThread failed " + std::to_string(GetLastError())};
    }
    return std::nullopt;
}

EnqueueResult DirectSshTransport::enqueueUserInput(std::string_view bytes) {
    auto &p = *impl_; Lock g(p.lock);
    if (p.state != TransportState::Running || p.closeRequested) return EnqueueResult::InvalidState;
    // Do not enqueue another user unit between begin/append of a paste. Besides
    // preserving its transaction, this keeps the reserved paste within the cap.
    if (p.input.pasteOpen()) return EnqueueResult::PasteInProgress;
    auto rc = p.input.enqueueUser(bytes);
    if (rc == EnqueueResult::Ok) SetEvent(p.wake);
    return rc;
}
EnqueueResult DirectSshTransport::enqueueReply(std::string_view bytes) {
    auto &p = *impl_;
    EnqueueResult rc;
    {
        Lock g(p.lock);
        if (p.state != TransportState::Running || p.closeRequested) return EnqueueResult::InvalidState;
        // Empty reply units would have no consumable bytes in InputScheduler.
        rc = bytes.empty() ? EnqueueResult::Ok : p.input.enqueueReply(bytes);
        if (rc == EnqueueResult::Ok) SetEvent(p.wake);
        if (rc == EnqueueResult::QueueFull) p.requestedFault = TransportFault::ReplyBackpressure;
    }
    if (rc == EnqueueResult::QueueFull) p.requestClose(CloseReason::Fault);
    return rc;
}
EnqueueResult DirectSshTransport::beginPaste(std::size_t n, bool bracketed) {
    auto &p = *impl_; Lock g(p.lock);
    if (p.state != TransportState::Running || p.closeRequested) return EnqueueResult::InvalidState;
    auto rc = !n && !bracketed ? EnqueueResult::Ok : p.input.beginPaste(n, bracketed);
    if (rc == EnqueueResult::Ok) SetEvent(p.wake);
    return rc;
}
EnqueueResult DirectSshTransport::appendPaste(std::string_view body) {
    auto &p = *impl_; Lock g(p.lock);
    if (p.state != TransportState::Running || p.closeRequested) return EnqueueResult::InvalidState;
    auto rc = p.input.appendPaste(body);
    if (rc == EnqueueResult::Ok) SetEvent(p.wake);
    return rc;
}
void DirectSshTransport::resize(GridSize grid, uint64_t gen) {
    auto &p = *impl_; Lock g(p.lock);
    p.sizes.resize({grid.cols, grid.rows}, gen);
    SetEvent(p.wake);
}
void DirectSshTransport::close(CloseReason reason) { impl_->requestClose(reason); }
TransportState DirectSshTransport::state() const { return impl_->state; }
uint64_t DirectSshTransport::discardedBytes() const {
    Lock g(impl_->lock); return impl_->output.discardedBytes();
}
std::optional<OutputChunk> DirectSshTransport::tryPopOutput() {
    auto &p = *impl_; Lock g(p.lock);
    auto bytes = p.output.pop();
    if (p.output.shouldResume()) { p.output.resumed(); SetEvent(p.wake); }
    if (!bytes) return std::nullopt;
    return OutputChunk{std::move(*bytes), p.output.incomplete()};
}
bool DirectSshTransport::waitClosed(DWORD ms) { return impl_->waitClosed(ms); }
bool DirectSshTransport::Impl::waitClosed(DWORD ms) const {
    HANDLE thread;
    {
        Lock g(lock);
        thread = owner;
        if (thread && GetCurrentThreadId() == ownerId) return false; // never self-join
    }
    return WaitForSingleObject(thread ? thread : closed, ms) == WAIT_OBJECT_0;
}
void DirectSshTransport::Impl::requestClose(CloseReason reason) {
    Lock g(lock);
    if (closeRequested || state == TransportState::Closed || state == TransportState::Failed) return;
    closeAt = now(); closeReason = reason;
    closeRequested = true;
    sizes.closing = true; input.close(); output.beginClosing();
    SetEvent(wake);
    if (!owner) { state = TransportState::Closed; SetEvent(closed); return; }
    // owner and its HANDLE are already published under the same lock as start.
    canceller = CreateThread(nullptr, 0, cancelMain, this, 0, nullptr);
}
DWORD WINAPI DirectSshTransport::Impl::cancelMain(void *p) {
    auto &self = *static_cast<Impl *>(p);
    HANDLE thread;
    uint64_t began;
    { Lock g(self.lock); thread = self.owner; began = self.closeAt; }
    while (now() - began < kCloseMs && WaitForSingleObject(self.closed, 0) == WAIT_TIMEOUT) {
        if (self.inAgentIo && CancelSynchronousIo(thread)) ++self.cancellations;
        if (WaitForSingleObject(self.closed, 5) != WAIT_TIMEOUT) break;
    }
    return 0;
}
DWORD WINAPI DirectSshTransport::Impl::ownerMain(void *p) {
    auto &self = *static_cast<Impl *>(p);
    { Lock g(self.lock); } // wait for start to publish owner/ownerId
    self.run();
    SetEvent(self.closed);
    return 0;
}
void DirectSshTransport::Impl::diag(const char *name, uint64_t ms, uint64_t bytes, std::string detail,
                                    std::optional<int64_t> errorCode) const {
    if (cb.onDiagnostic)
        cb.onDiagnostic(sid, aid, DiagRecord{name, ms, bytes, std::move(detail), std::move(errorCode)});
}
void DirectSshTransport::Impl::setState(TransportState next, std::optional<TransportFault> fault) {
    state = next;
    if (cb.onStateChanged) cb.onStateChanged(sid, aid, next, fault);
}
void DirectSshTransport::Impl::setPhase(DirectStage next, const char *name) {
    auto t = now();
    diag("ssh.direct.phase", phaseAt ? t - phaseAt : 0, 0, name);
    phase = next; phaseAt = t;
}
[[noreturn]] void DirectSshTransport::Impl::fail(DirectFailureKind kind, int code, std::string detail,
                                               std::optional<TransportFault> fault,
                                               std::optional<PresentedHostKey> hostKey) const {
    if (code != 0) diag("ssh.direct.fail", 0, 0, detail, static_cast<int64_t>(code));
    auto exit = mapDirectFailure({phase, kind, code, std::move(hostKey)});
    if (!detail.empty()) exit.detail += "; " + detail;
    throw Failure{std::move(exit), fault};
}
void DirectSshTransport::Impl::checkpoint(uint64_t until) const {
    if (authTimedOut) {
        auto exit = mapDirectFailure({DirectStage::Auth, DirectFailureKind::DeadlineExpired});
        throw Failure{std::move(exit), std::nullopt};
    }
    if (closeRequested) {
        std::optional<TransportFault> fault;
        { Lock g(lock); fault = requestedFault; }
        if (fault) fail(DirectFailureKind::Other, 0, "reply backpressure", fault);
        throw Cancelled{};
    }
    if (until && now() >= until) fail(DirectFailureKind::DeadlineExpired);
}
void DirectSshTransport::Impl::check(int rc, std::string detail) const {
    if (!rc) return;
    char *message = nullptr;
    libssh2_session_last_error(session, &message, nullptr, 0);
    const std::string library = message ? message : "";
    if (!library.empty()) detail += "; " + library;
    auto kind = DirectFailureKind::Other;
    switch (rc) {
    case LIBSSH2_ERROR_KEX_FAILURE: case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:
    case LIBSSH2_ERROR_ALGO_UNSUPPORTED: case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED:
        kind = DirectFailureKind::KexFailed; break;
    case LIBSSH2_ERROR_TIMEOUT: case LIBSSH2_ERROR_SOCKET_TIMEOUT:
        kind = DirectFailureKind::TimedOut; break;
    case LIBSSH2_ERROR_SOCKET_SEND: case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT: case LIBSSH2_ERROR_BAD_SOCKET:
    case LIBSSH2_ERROR_BANNER_RECV: case LIBSSH2_ERROR_BANNER_SEND:
        kind = library.find("agent") != std::string::npos ?
            DirectFailureKind::AgentUnavailable : DirectFailureKind::SocketClosed; break;
    case LIBSSH2_ERROR_AGENT_PROTOCOL: kind = DirectFailureKind::AgentUnavailable; break;
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED: case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:
        kind = DirectFailureKind::NoAcceptedIdentity; break;
    case LIBSSH2_ERROR_CHANNEL_FAILURE: case LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED:
    case LIBSSH2_ERROR_REQUEST_DENIED: kind = DirectFailureKind::RequestDenied; break;
    }
    fail(kind, rc, std::move(detail));
}

WSANETWORKEVENTS DirectSshTransport::Impl::waitSocket(uint64_t until, bool closing,
                                                    bool connecting, int directions) {
    if (!closing) checkpoint(until);
    auto began = now();
    if (until && began >= until) return {};
    const auto stop = until ? std::min(until, began + 50) : began + 50;
    if (directions < 0) directions = session ? libssh2_session_block_directions(session) : 0;
    if (!directions) directions = LIBSSH2_SESSION_BLOCK_INBOUND | LIBSSH2_SESSION_BLOCK_OUTBOUND;
    for (;;) {
        auto t = now();
        if (t >= stop) return {};
        WSAEVENT events[] = {socketEvent, wake};
        DWORD rc = WSAWaitForMultipleEvents(2, events, FALSE, static_cast<DWORD>(stop - t), FALSE);
        if (!closing) checkpoint(until);
        if (rc == WSA_WAIT_TIMEOUT || rc == WSA_WAIT_EVENT_0 + 1) return {};
        if (rc == WSA_WAIT_FAILED) {
            if (closing) return {};
            int code = WSAGetLastError(); fail(socketFailure(code), code, "socket wait");
        }
        WSANETWORKEVENTS network{};
        if (WSAEnumNetworkEvents(socket, socketEvent, &network) == SOCKET_ERROR) {
            if (closing) return {};
            int code = WSAGetLastError(); fail(socketFailure(code), code, "socket events");
        }
        if (connecting) {
            if (network.lNetworkEvents & (FD_CONNECT | FD_CLOSE)) return network;
        } else if ((network.lNetworkEvents & FD_CLOSE) ||
                   ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) && (network.lNetworkEvents & FD_READ)) ||
                   ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) && (network.lNetworkEvents & FD_WRITE))) {
            return network;
        }
        // Irrelevant edges are acknowledged, without restarting the 50ms bound.
    }
}

void DirectSshTransport::Impl::connectSocket(uint64_t until) {
    setPhase(DirectStage::Dns, "dns");
    struct Lookup {
        OVERLAPPED overlapped{};
        PADDRINFOEXW addresses = nullptr;
        HANDLE cancel = nullptr;
        Lookup() { overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
        ~Lookup() {
            if (addresses) FreeAddrInfoExW(addresses);
            if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
        }
    } lookup;
    if (!lookup.overlapped.hEvent) fail(DirectFailureKind::Other, static_cast<int>(GetLastError()));
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    auto host = widen(target.hostName), port = std::to_wstring(target.port);
    checkpoint(until);
    const auto dnsAt = now();
    if (dnsAt >= until) fail(DirectFailureKind::DeadlineExpired);
    const auto remaining = until - dnsAt;
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(remaining / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((remaining % 1000) * 1000);
    int rc = GetAddrInfoExW(host.c_str(), port.c_str(), NS_DNS, nullptr, &hints,
        &lookup.addresses, &timeout, &lookup.overlapped, nullptr, &lookup.cancel);
    if (rc == WSA_IO_PENDING) {
        for (;;) {
            if (WaitForSingleObject(lookup.overlapped.hEvent, 0) == WAIT_OBJECT_0) break;
            if (closeRequested || now() >= until) {
                GetAddrInfoExCancel(&lookup.cancel);
                // Windows immediately completes the caller's event on cancel,
                // even if legacy providers continue internally. Keep OVERLAPPED
                // and ppResult alive until that completion, never detach them.
                WaitForSingleObject(lookup.overlapped.hEvent, INFINITE);
                break;
            }
            HANDLE events[] = {lookup.overlapped.hEvent, wake};
            DWORD wait = WaitForMultipleObjects(2, events, FALSE,
                static_cast<DWORD>(std::min<uint64_t>(50, until - now())));
            if (wait == WAIT_FAILED) {
                const auto error = GetLastError();
                GetAddrInfoExCancel(&lookup.cancel);
                WaitForSingleObject(lookup.overlapped.hEvent, INFINITE);
                fail(DirectFailureKind::Other, static_cast<int>(error), "DNS wait");
            }
        }
        rc = GetAddrInfoExOverlappedResult(&lookup.overlapped);
    }
    checkpoint(until); // cancellation takes precedence over WSA_E_CANCELLED
    if (rc) fail(socketFailure(rc), rc);
    setPhase(DirectStage::Connect, "connect");
    socketEvent = WSACreateEvent();
    if (socketEvent == WSA_INVALID_EVENT) fail(DirectFailureKind::Other, WSAGetLastError());
    int error = WSAEHOSTUNREACH;
    for (auto *a = lookup.addresses; a; a = a->ai_next) {
        checkpoint(until);
        socket = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket == INVALID_SOCKET) { error = WSAGetLastError(); continue; }
        WSAResetEvent(socketEvent);
        // WSAEventSelect sets FIONBIO. Keep this subscription stable so FD_WRITE
        // is not continuously re-armed on a socket which is already writable.
        if (WSAEventSelect(socket, socketEvent, FD_READ | FD_WRITE | FD_CLOSE | FD_CONNECT))
            fail(DirectFailureKind::Other, WSAGetLastError(), "WSAEventSelect");
        rc = ::connect(socket, a->ai_addr, static_cast<int>(a->ai_addrlen));
        error = rc == 0 ? 0 : WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
            for (;;) {
                auto events = waitSocket(until, false, true);
                if (events.lNetworkEvents & FD_CONNECT) {
                    error = events.iErrorCode[FD_CONNECT_BIT]; break;
                }
                if (events.lNetworkEvents & FD_CLOSE) {
                    error = events.iErrorCode[FD_CLOSE_BIT];
                    if (!error) error = WSAECONNRESET;
                    break;
                }
            }
        }
        if (!error) return;
        closesocket(socket); socket = INVALID_SOCKET;
    }
    fail(socketFailure(error), error);
}

bool DirectSshTransport::Impl::sendSize(bool initial, uint64_t until) {
    std::optional<DirectSizeRequest> request;
    { Lock g(lock); request = sizes.beginSend(); }
    if (!request) return false;
    assert(request->grid.cols >= 20 && request->grid.rows >= 5);
    const auto modes = directPtyModes();
    int rc = retry([&] {
        return initial ? libssh2_channel_request_pty_ex(channel, kDirectTerm.data(),
            static_cast<unsigned>(kDirectTerm.size()), modes.data(), static_cast<unsigned>(modes.size()),
            request->grid.cols, request->grid.rows, 0, 0) :
            libssh2_channel_request_pty_size_ex(channel, request->grid.cols, request->grid.rows, 0, 0);
    }, until);
    if (rc) {
        check(rc, initial ? "pty-req" : "window-change before shell");
    }
    { Lock g(lock); sizes.completeSend(); }
    diag("ssh.direct.size", 0, request->generation,
        std::string(initial ? "pty-req " : "window-change ") +
        std::to_string(request->grid.cols) + "x" + std::to_string(request->grid.rows));
    return true;
}
bool DirectSshTransport::Impl::resizeRunning() {
    std::optional<DirectSizeRequest> request;
    { Lock g(lock); request = sizes.beginSend(); }
    if (!request) return true;
    if (!resizeUntil) resizeUntil = now() + uint64_t(target.connectTimeoutSec) * 1000;
    checkpoint(resizeUntil);
    int rc = libssh2_channel_request_pty_size_ex(channel, request->grid.cols, request->grid.rows, 0, 0);
    checkpoint(resizeUntil);
    if (rc == LIBSSH2_ERROR_EAGAIN) return false;
    if (rc) fail(DirectFailureKind::RequestDenied, rc, "window-change", TransportFault::PipeBroken);
    { Lock g(lock); sizes.completeSend(); }
    resizeUntil = 0;
    diag("ssh.direct.size", 0, request->generation, "window-change " +
        std::to_string(request->grid.cols) + "x" + std::to_string(request->grid.rows));
    return true;
}
void DirectSshTransport::Impl::open(uint64_t connectUntil) {
    checkpoint(connectUntil);
    int rc = runtimeError();
    if (rc) fail(DirectFailureKind::Other, rc, "Winsock/libssh2 init");
    session = libssh2_session_init();
    if (!session) fail(DirectFailureKind::Other, LIBSSH2_ERROR_ALLOC);
    libssh2_session_set_blocking(session, 0);
    prepareHostKey();
    checkpoint(connectUntil);
    connectSocket(connectUntil);
    setPhase(DirectStage::Handshake, "handshake");
    check(retry([&] { return libssh2_session_handshake(session, socket); }, connectUntil));
    handshaken = true;
    verifyHostKey();
    checkpoint(connectUntil);
    std::string methods;
    for (int method : {LIBSSH2_METHOD_KEX, LIBSSH2_METHOD_HOSTKEY, LIBSSH2_METHOD_CRYPT_CS, LIBSSH2_METHOD_CRYPT_SC}) {
        const char *name = libssh2_session_methods(session, method);
        if (name) { if (!methods.empty()) methods += ','; methods += name; }
    }
    diag("ssh.direct.methods", 0, 0, methods);
    authenticate();
    const auto until = now() + uint64_t(target.connectTimeoutSec) * 1000;
    setPhase(DirectStage::Channel, "channel");
    while (!channel) {
        checkpoint(until);
        channel = libssh2_channel_open_ex(session, "session", 7, 1024 * 1024, 32768, nullptr, 0);
        checkpoint(until);
        if (!channel) {
            int error = libssh2_session_last_errno(session);
            if (error != LIBSSH2_ERROR_EAGAIN) check(error ? error : LIBSSH2_ERROR_CHANNEL_FAILURE);
            waitSocket(until);
        }
    }
    setPhase(DirectStage::Pty, "pty");
    sendSize(true, until);
    while (sendSize(false, until)) {} // catch resizes received during pty-req
    setPhase(DirectStage::Shell, "shell");
    check(retry([&] { return libssh2_channel_shell(channel); }, until));
    checkpoint(until);
    ran = true;
    setPhase(DirectStage::Running, "running");
    setState(TransportState::Running);
}

bool DirectSshTransport::Impl::readOutput(bool closing) {
    char buffer[16 * 1024];
    bool progress = false;
    for (int stream = 0; stream <= 1; ++stream) {
        for (int batch = 0; batch < 8; ++batch) {
            if (!closing) {
                checkpoint();
                Lock g(lock);
                if (output.readingPaused()) return progress;
            }
            auto n = libssh2_channel_read_ex(channel, stream, buffer, sizeof buffer);
            if (!n || n == LIBSSH2_ERROR_EAGAIN) break;
            if (n < 0) { if (closing) return progress; check(static_cast<int>(n), "channel read"); }
            progress = true;
            const std::string_view bytes(buffer, static_cast<size_t>(n));
            tail.append(bytes);
            if (cb.onOutputTap) cb.onOutputTap(sid, aid, bytes);
            OutputQueue::PushResult rc;
            { Lock g(lock); rc = output.push(std::string(bytes)); }
            if (cb.onOutputAvailable) cb.onOutputAvailable(sid, aid);
            if (rc == OutputQueue::PushResult::Overflow)
                fail(DirectFailureKind::Other, 0, "output queue limit", TransportFault::OutputLimit);
            if (closing && now() - closeAt >= kProtocolCloseMs) return progress;
        }
    }
    return progress;
}
void DirectSshTransport::Impl::running() {
    for (;;) {
        checkpoint();
        bool stalled;
        {
            Lock g(lock);
            if (input.hasPending()) input.notePending(now());
            stalled = input.stalled(now());
        }
        if (stalled) fail(DirectFailureKind::Other, 0, "input stalled", TransportFault::InputStalled);
        // A frozen write buffer survives EAGAIN and paste appends. Do not start
        // another outbound control request in the middle of a pending write.
        if (writePending.empty() && !resizeRunning()) {
            int directions = libssh2_session_block_directions(session);
            bool progress = readOutput();
            directions |= libssh2_session_block_directions(session);
            if (libssh2_channel_eof(channel)) { requestClose(CloseReason::ProcessExited); return; }
            if (!progress) waitSocket(0, false, false, directions);
            continue; // immutable resize arguments; control gets another turn first
        }
        checkpoint();
        if (writePending.empty()) {
            Lock g(lock);
            auto bytes = input.peekWrite();
            writePending.assign(bytes.substr(0, 16 * 1024));
        }
        bool progress = false;
        int directions = 0;
        if (!writePending.empty()) {
            auto n = libssh2_channel_write(channel, writePending.data(), writePending.size());
            if (n == LIBSSH2_ERROR_EAGAIN) directions |= libssh2_session_block_directions(session);
            else if (n < 0) check(static_cast<int>(n), "channel write");
            else if (n > 0) {
                Lock g(lock);
                input.consumed(static_cast<size_t>(n), now());
                writePending.clear(); progress = true;
            }
        }
        progress = readOutput() || progress;
        directions |= libssh2_session_block_directions(session);
        if (libssh2_channel_eof(channel)) { requestClose(CloseReason::ProcessExited); return; }
        // Successful bounded reads loop again before waiting: libssh2 may have
        // decrypted bytes cached even when Winsock no longer signals FD_READ.
        if (!progress) waitSocket(0, false, false, directions);
    }
}
SshExitInfo DirectSshTransport::Impl::channelExit() {
    char *signal = nullptr;
    size_t length = 0;
    libssh2_channel_get_exit_signal(channel, &signal, &length, nullptr, nullptr, nullptr, nullptr);
    auto result = mapChannelExit(static_cast<uint32_t>(libssh2_channel_get_exit_status(channel)),
                                signal ? std::string_view(signal, length) : std::string_view{});
    if (signal) libssh2_free(session, signal);
    return result;
}

void DirectSshTransport::Impl::cleanup() {
    releaseAgent();
    const auto until = closeAt + kProtocolCloseMs;
    bool protocol = handshaken && !authInterrupted && !authInProgress;
    auto bounded = [&](auto call) {
        while (now() < until) {
            int rc = call();
            if (!rc) return true;
            if (rc != LIBSSH2_ERROR_EAGAIN) return false;
            // Ignore output backpressure during close. Each drain is bounded in
            // bytes and time, and OutputQueue bounds/discards the closing tail.
            if (channel) readOutput(true);
            waitSocket(until, true);
        }
        return false;
    };
    if (channel && protocol) {
        protocol = bounded([&] { return libssh2_channel_send_eof(channel); });
        if (protocol) protocol = bounded([&] { return libssh2_channel_close(channel); });
    }
    // EOF may precede exit-status/signal. Capture after channel close has
    // processed the peer's close and final requests, before session_free.
    if (channel && closeReason == CloseReason::ProcessExited) remoteExit = channelExit();
    if (session && protocol)
        protocol = bounded([&] { return libssh2_session_disconnect_ex(session,
            SSH_DISCONNECT_BY_APPLICATION, "Direct SSH close", ""); });
    if (session) {
        libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_SEND,
            reinterpret_cast<libssh2_cb_generic *>(disconnectedSend));
        libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_RECV,
            reinterpret_cast<libssh2_cb_generic *>(disconnectedRecv));
    }
    if (socket != INVALID_SOCKET) { shutdown(socket, SD_BOTH); closesocket(socket); socket = INVALID_SOCKET; }
    if (knownHosts) { libssh2_knownhost_free(knownHosts); knownHosts = nullptr; }
    if (session) {
        // With nonblocking mode and terminal send/recv callbacks, EAGAIN cannot
        // be caused by Winsock, nor can the library access a recycled socket.
        int rc;
        do {
            rc = libssh2_session_free(session);
            if (rc == LIBSSH2_ERROR_EAGAIN) WaitForSingleObject(wake, 1);
        } while (rc == LIBSSH2_ERROR_EAGAIN && now() < closeAt + 2900);
        if (rc) diag("ssh.direct.free", now() - closeAt, 0, "session_free error=" + std::to_string(rc), rc);
        session = nullptr; channel = nullptr;
    }
    if (socketEvent != WSA_INVALID_EVENT) { WSACloseEvent(socketEvent); socketEvent = WSA_INVALID_EVENT; }
    diag("ssh.direct.cancel_sync", 0, cancellations.load());
    diag("ssh.direct.close", now() - closeAt, 0,
         protocol && now() < until ? "protocol close" : "hard socket close");
}
void DirectSshTransport::Impl::run() {
    SshExitInfo exit{SshError::None, 0, {}, {}};
    std::optional<TransportFault> fault;
    try {
        setState(TransportState::Starting);
        open(now() + uint64_t(target.connectTimeoutSec) * 1000);
        running();
    } catch (const Cancelled &) {
        exit = {SshError::None, 0, ran ? "" : "已取消", {}};
    } catch (const Failure &f) {
        exit = f.exit;
        fault = f.fault;
    } catch (const std::exception &e) {
        exit = mapDirectFailure({phase, DirectFailureKind::Other});
        exit.detail += "; " + std::string(e.what());
    } catch (...) {
        exit = mapDirectFailure({phase, DirectFailureKind::Other});
    }
    requestClose(exit.error == SshError::None ? CloseReason::User : CloseReason::Fault);
    if (!ran && closeReason == CloseReason::User && !authTimedOut) {
        exit = {SshError::None, 0, "已取消", {}};
        fault.reset();
    }
    setState(TransportState::Closing);
    cleanup();
    if (remoteExit && (exit.error == SshError::None)) exit = std::move(*remoteExit);
    exit.outputTail = utf8Tail(tail.text(), kSshOutputTailBytes);
    const bool failed = fault.has_value() || (exit.error != SshError::None && exit.error != SshError::RemoteExit);
    if (failed && !fault) fault = ran ? TransportFault::PipeBroken : TransportFault::StartFailed;
    if (failed) exit.detail = "（直连）" + exit.detail;
    if (cb.onFinished) cb.onFinished(sid, aid, exit);
    setState(failed ? TransportState::Failed : TransportState::Closed, fault);
}

} // namespace it2w
