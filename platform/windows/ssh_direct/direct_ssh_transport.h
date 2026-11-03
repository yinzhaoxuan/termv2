// Windows in-process SSH. One attempt per object; all protocol I/O is owner-only.
#pragma once
#include "ssh_session_transport.h"

namespace it2w {

// Test harness only. Empty agentPipe preserves SSH_AUTH_SOCK/default selection.
struct DirectSshTestHooks { std::string agentPipe; };

class DirectSshTransport final : public SshSessionTransport {
public:
    explicit DirectSshTransport(SshCallbacks, DirectSshTestHooks hooks = {});
    ~DirectSshTransport() override;
    DirectSshTransport(const DirectSshTransport &) = delete;
    DirectSshTransport &operator=(const DirectSshTransport &) = delete;

    std::optional<SnapshotError> start(SessionId, AttemptId, const SshLaunchOptions &) override;
    EnqueueResult enqueueUserInput(std::string_view) override;
    EnqueueResult enqueueReply(std::string_view) override;
    EnqueueResult beginPaste(std::size_t, bool bracketed = true) override;
    EnqueueResult appendPaste(std::string_view) override;
    void resize(GridSize, uint64_t generation) override;
    void close(CloseReason) override;
    std::optional<OutputChunk> tryPopOutput() override;
    uint64_t discardedBytes() const override;
    TransportState state() const override;
    // Also waits for the owner HANDLE, so true means the thread has exited.
    // Callbacks must return promptly and must not destroy this object.
    bool waitClosed(DWORD ms) override;
    uint32_t activeProcessCount() override { return 0; }
    // REVIEW 增补二 D-1: false until a per-read confirmation UI exists.
    // Direct is byte-transparent, so true would answer clipboard reads whenever
    // osc52.allowRead is set. Same default as ConPTY; Hyper-V forwards this.
    bool supportsOsc52Read() const override { return false; }
    SshTransportMode mode() const override { return SshTransportMode::Direct; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct DirectSshTransportTestPeer;
};

} // namespace it2w
