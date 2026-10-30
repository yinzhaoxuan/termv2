// T09 HyperVTransport：先在后台线程按 VM ID 解析地址（HyperVResolver + 固定 PowerShell 查询），
// 得到地址后用 SshTransport 连接。对外方法与 SshTransport 相同，便于后端 Transport 抽象复用。
// 解析期间：状态 Starting；输入/回复/粘贴丢弃（返回 Ok）；resize 记住最后尺寸供 ssh 启动；close 取消解析。
// 未实机验证：须在启用 Hyper-V 的 Win11 上覆盖 VM-01。
#pragma once
#ifndef _WIN32
#error "hyperv_transport 仅用于 Windows"
#endif
#include "ssh_transport.h"
#include "hyperv_resolve.h"
#include <atomic>
#include <thread>

namespace it2w {

struct HyperVLaunch {
    HvPolicy policy;          // vmId 必须已规范化
    SshLaunchOptions ssh;     // target.hostName 留空，由解析结果填入
};

struct HyperVCallbacks : SshCallbacks {
    // 解析结束：action.kind 为 Connect（随后启动 ssh）或 Fail（窗格应进入 Failed，text 为原因）
    std::function<void(SessionId, AttemptId, const HvAction &)> onResolved;
};

class HyperVTransport {
public:
    explicit HyperVTransport(HyperVCallbacks cb);
    ~HyperVTransport();
    HyperVTransport(const HyperVTransport &) = delete;
    HyperVTransport &operator=(const HyperVTransport &) = delete;

    void start(SessionId, AttemptId, HyperVLaunch);
    EnqueueResult enqueueUserInput(std::string_view b);
    EnqueueResult enqueueReply(std::string_view b);
    EnqueueResult beginPaste(std::size_t n, bool bracketed);
    EnqueueResult appendPaste(std::string_view b);
    void resize(GridSize s, uint64_t gen);
    void close(CloseReason r);
    std::optional<OutputChunk> tryPopOutput();
    TransportState state() const;
    uint64_t discardedBytes() const { return sshStarted_ ? ssh_->discardedBytes() : 0; }
    bool waitClosed(DWORD ms);
    uint32_t activeProcessCount();

private:
    void worker(SessionId, AttemptId, HyperVLaunch);
    HyperVCallbacks cb_;
    std::unique_ptr<SshTransport> ssh_;
    std::atomic<bool> sshStarted_{false};   // 置位后 ssh_ 只读，可无锁转发
    std::atomic<bool> closing_{false};
    std::atomic<TransportState> state_{TransportState::Idle};
    std::mutex mu_;
    GridSize size_{};
    HANDLE cancel_ = nullptr;
    std::thread th_;
};

}  // namespace it2w
