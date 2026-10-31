// Windows SSH session interface shared by SSH and Hyper-V transports.
#pragma once
#ifndef _WIN32
#error "ssh_session_transport 仅用于 Windows"
#endif
// Transport types remain in the Windows ConPTY header for v0.
#include "conpty_transport.h"
#include "ssh_session.h"
#include "ssh_snapshot.h"
#include <memory>

namespace it2w {

struct SshLaunchOptions {
    SshTarget target;
    std::wstring sshExe;            // 空：%SystemRoot%\System32\OpenSSH\ssh.exe
    std::wstring snapshotDir;       // 应用私有目录（已存在）；快照文件名含 session/attempt
    GridSize initialSize;
    EnvList environment;            // 空：继承当前进程环境后过滤
};

struct SshCallbacks {
    // 原始输出 tap（读线程），仅用于诊断；不得派发消息/调用 UI。
    std::function<void(SessionId, AttemptId, std::string_view)> onOutputTap;
    std::function<void(SessionId, AttemptId, TransportState, std::optional<TransportFault>)> onStateChanged;
    std::function<void(SessionId, AttemptId)> onOutputAvailable;
    // ssh 退出：exit.error 为映射结果（None/RemoteExit 为正常结束）
    std::function<void(SessionId, AttemptId, const SshExitInfo &)> onFinished;
    std::function<void(SessionId, AttemptId, const DiagRecord &)> onDiagnostic;
};

enum class SshTransportMode { ConPty, Direct };

class SshSessionTransport {
public:
    virtual ~SshSessionTransport() = default;
    virtual std::optional<SnapshotError> start(SessionId, AttemptId, const SshLaunchOptions &) = 0;
    virtual EnqueueResult enqueueUserInput(std::string_view) = 0;
    virtual EnqueueResult enqueueReply(std::string_view) = 0;
    virtual EnqueueResult beginPaste(std::size_t n, bool bracketed) = 0;
    virtual EnqueueResult appendPaste(std::string_view) = 0;
    virtual void resize(GridSize, uint64_t generation) = 0;
    virtual void close(CloseReason) = 0;
    virtual std::optional<OutputChunk> tryPopOutput() = 0;
    virtual uint64_t discardedBytes() const = 0;
    virtual TransportState state() const = 0;
    virtual bool waitClosed(DWORD ms) = 0;
    virtual uint32_t activeProcessCount() = 0;
    virtual bool supportsOsc52Read() const = 0;
    virtual SshTransportMode mode() const = 0;
};

using SshTransportFactory = std::function<std::unique_ptr<SshSessionTransport>(SshCallbacks)>;

}  // namespace it2w
