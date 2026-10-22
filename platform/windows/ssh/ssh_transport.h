// T07 SshTransport：在 ConPTY 中启动 `ssh.exe -F <快照> <alias>`（不经 shell 拼接）。
// 快照每个 attempt 独立一份，写到应用私有目录，attempt 结束由 ConPtyTransport 删除。
// 未实机验证：Windows 自带 OpenSSH、ConPTY 下的提示交互、快照路径编码均待 DESKTOP-HOODQAJ。
#pragma once
#ifndef _WIN32
#error "ssh_transport 仅用于 Windows"
#endif
#include "conpty_transport.h"
#include "ssh_session.h"
#include "ssh_snapshot.h"
#include <memory>
#include <mutex>

namespace it2w {

struct SshLaunchOptions {
    SshTarget target;
    std::wstring sshExe;            // 空：%SystemRoot%\System32\OpenSSH\ssh.exe
    std::wstring snapshotDir;       // 应用私有目录（已存在）；快照文件名含 session/attempt
    GridSize initialSize;
    EnvList environment;            // 空：继承当前进程环境后过滤
};

struct SshCallbacks {
    std::function<void(SessionId, AttemptId, TransportState, std::optional<TransportFault>)> onStateChanged;
    std::function<void(SessionId, AttemptId)> onOutputAvailable;
    // ssh 退出：exit.error 为映射结果（None/RemoteExit 为正常结束）
    std::function<void(SessionId, AttemptId, const SshExitInfo &)> onFinished;
    std::function<void(SessionId, AttemptId, const DiagRecord &)> onDiagnostic;
};

class SshTransport {
public:
    explicit SshTransport(SshCallbacks cb);
    ~SshTransport();
    SshTransport(const SshTransport &) = delete;
    SshTransport &operator=(const SshTransport &) = delete;

    // 快照非法 → SnapshotError（不启动）；ssh.exe 缺失/启动失败 → onFinished(SshNotFound)
    std::optional<SnapshotError> start(SessionId, AttemptId, const SshLaunchOptions &);

    EnqueueResult enqueueUserInput(std::string_view b) { return pty_->enqueueUserInput(b); }
    EnqueueResult enqueueReply(std::string_view b) { return pty_->enqueueReply(b); }
    void resize(GridSize s, uint64_t gen) { pty_->resize(s, gen); }
    void close(CloseReason r) { pty_->close(r); }
    // 取输出：同时喂入 OutputTail 用于退出时错误识别
    std::optional<OutputChunk> tryPopOutput();
    TransportState state() const { return pty_->state(); }
    bool waitClosed(DWORD ms) { return pty_->waitClosed(ms); }

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
