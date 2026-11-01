// Shared close-auth fault injector. Handle/Thread and join-failure policy stay
// with each harness; the probe retains its hardFail-on-join-failure semantics.
#pragma once
#include <windows.h>
#include "ssh_a_probe_logic.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

namespace ssh_a_probe {
inline constexpr char kAgentPipe[] = R"(\\.\pipe\openssh-ssh-agent)";
inline constexpr size_t kAgentPacketLimit = 1024 * 1024;
inline uint32_t u32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
// Fault-injection relay for close-auth only. Enumerates the REAL OpenSSH agent's
// keys, then withholds the reply to the first SIGN_REQUEST accepted by the sshd.
// The worker is inside unmodified libssh2_agent_userauth -> synchronous ReadFile.
// The relay stays open until the worker has unwound, so a broken pipe cannot
// masquerade as successful CancelSynchronousIo. It never sees private keys.
template<class Control, class Handle, class Thread, void (*JoinFailure)(const char *)>
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
        if (!n || n > kAgentPacketLimit) throw Failure("invalid/oversized agent message");
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
            control_.stalledSignAt = GetTickCount64();
            while (!stop_) Sleep(5); // intentionally no signing reply
        } catch (const std::exception &e) { error_ = e.what(); }
        // pipe_ is deliberately kept open until the session worker is done.
    }
public:
    std::string path;
    explicit StallingAgent(Control &c) : control_(c) {
        path = R"(\\.\pipe\termv2-ssh-a-probe-)" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        pipe_.value = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, nullptr);
        if (!pipe_.valid()) throw Failure("CreateNamedPipe(close-auth) failed");
        thread_ = std::make_unique<Thread>([this] { run(); });
    }
    ~StallingAgent() {
        stop_ = true;
        const auto end = GetTickCount64() + 200;
        while (!thread_->done(5) && GetTickCount64() < end) thread_->cancelSync();
        if (!thread_->done()) JoinFailure("agent relay did not join");
        if (!error_.empty()) std::fprintf(stderr, "[ssh-a] close-auth relay: %s\n", error_.c_str());
    }
};

} // namespace ssh_a_probe
