// T07 SshTransport：在 ConPTY 中启动 `ssh.exe -F <快照> <alias>`（不经 shell 拼接）。
// 快照每个 attempt 独立一份，写到应用私有目录，attempt 结束由 ConPtyTransport 删除。
// 未实机验证：Windows 自带 OpenSSH、ConPTY 下的提示交互、快照路径编码均待 DESKTOP-HOODQAJ。
#pragma once
#ifndef _WIN32
#error "ssh_transport 仅用于 Windows"
#endif
#include "ssh_session_transport.h"
#include <mutex>

namespace it2w {

class SshTransport final : public SshSessionTransport {
public:
    explicit SshTransport(SshCallbacks cb);
    ~SshTransport() override;
    SshTransport(const SshTransport &) = delete;
    SshTransport &operator=(const SshTransport &) = delete;

    // 快照非法 → SnapshotError（不启动）；ssh.exe 缺失/启动失败 → onFinished(SshNotFound)
    std::optional<SnapshotError> start(SessionId, AttemptId, const SshLaunchOptions &) override;

    EnqueueResult enqueueUserInput(std::string_view b) override { return pty_->enqueueUserInput(b); }
    EnqueueResult enqueueReply(std::string_view b) override { return pty_->enqueueReply(b); }
    void resize(GridSize s, uint64_t gen) override { pty_->resize(s, gen); }
    void close(CloseReason r) override { pty_->close(r); }
    // 取输出：同时喂入 OutputTail 用于退出时错误识别
    std::optional<OutputChunk> tryPopOutput() override;
    uint64_t discardedBytes() const override { return pty_->discardedBytes(); }
    TransportState state() const override { return pty_->state(); }
    bool waitClosed(DWORD ms) override { return pty_->waitClosed(ms); }
    EnqueueResult beginPaste(std::size_t n, bool bracketed) override { return pty_->beginPaste(n, bracketed); }
    EnqueueResult appendPaste(std::string_view b) override { return pty_->appendPaste(b); }
    uint32_t activeProcessCount() override { return pty_->activeProcessCount(); }
    bool supportsOsc52Read() const override { return false; }
    SshTransportMode mode() const override { return SshTransportMode::ConPty; }

    static std::wstring defaultSshExe();

private:
    void handleExit(SessionId, AttemptId, uint32_t code);
    SshCallbacks cb_;
    std::unique_ptr<ConPtyTransport> pty_;
    std::mutex tailMu_;
    OutputTail tail_;
    bool finished_ = false;
};

}  // namespace it2w
