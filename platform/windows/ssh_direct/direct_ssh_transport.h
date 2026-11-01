// Windows in-process SSH. One attempt per object; all protocol I/O is owner-only.
#pragma once
#include "ssh_session_transport.h"

namespace it2w {

class DirectSshTransport final : public SshSessionTransport {
public:
    explicit DirectSshTransport(SshCallbacks);
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
    bool supportsOsc52Read() const override { return true; }
    SshTransportMode mode() const override { return SshTransportMode::Direct; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct DirectSshTransportTestPeer;
};

} // namespace it2w
