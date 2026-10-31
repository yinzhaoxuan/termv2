#include "hyperv_transport.h"
#include "hyperv_query.h"

namespace it2w {

HyperVTransport::HyperVTransport(HyperVCallbacks cb, SshTransportFactory factory) : cb_(std::move(cb)) {
    cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ssh_ = factory(static_cast<SshCallbacks>(cb_));
}

HyperVTransport::~HyperVTransport() {
    closing_ = true;
    if (cancel_) SetEvent(cancel_);
    if (th_.joinable()) th_.join();
    ssh_.reset();
    if (cancel_) CloseHandle(cancel_);
}

void HyperVTransport::start(SessionId s, AttemptId a, HyperVLaunch l) {
    { std::lock_guard g(mu_); size_ = {l.ssh.initialSize, 0}; }
    state_ = TransportState::Starting;
    if (cb_.onStateChanged) cb_.onStateChanged(s, a, TransportState::Starting, std::nullopt);
    th_ = std::thread([this, s, a, l = std::move(l)]() mutable { worker(s, a, std::move(l)); });
}

void HyperVTransport::worker(SessionId s, AttemptId a, HyperVLaunch l) {
    HyperVResolver r(l.policy);
    HvDriver d;
    const std::string id = l.policy.vmId;
    const DWORD qms = l.policy.queryTimeoutMs;
    d.query = [&] { return queryHyperVVm(id, qms, cancel_); };
    d.start = [&] { return startHyperVVm(id, qms * 6, cancel_); };   // Start-VM 本身可能耗时，单独放宽
    d.sleep = [&](uint32_t ms) { return WaitForSingleObject(cancel_, ms) == WAIT_TIMEOUT; };
    d.now = [] { return uint64_t(GetTickCount64()); };
    d.cancelled = [&] { return closing_.load(); };
    d.onStep = [&](const HvAction &x) {
        if (cb_.onDiagnostic && x.kind == HvAction::Wait) cb_.onDiagnostic(s, a, DiagRecord{"hyperv.wait", 0, 0, x.text});
        if (cb_.onDiagnostic && x.kind == HvAction::Start) cb_.onDiagnostic(s, a, DiagRecord{"hyperv.start", 0, 0, {}});
    };
    HvAction res = runHyperVResolution(r, d);
    if (closing_) { state_ = TransportState::Closed; if (cb_.onStateChanged) cb_.onStateChanged(s, a, TransportState::Closed, std::nullopt); return; }
    if (cb_.onDiagnostic && res.fallback) cb_.onDiagnostic(s, a, DiagRecord{"hyperv.fallback", 0, 0, res.text});
    if (cb_.onResolved) cb_.onResolved(s, a, res);
    if (res.kind != HvAction::Connect) {
        state_ = TransportState::Failed;
        if (cb_.onStateChanged) cb_.onStateChanged(s, a, TransportState::Failed, TransportFault::StartFailed);
        return;
    }
    l.ssh.target.hostName = res.host;
    uint64_t launchGeneration;
    {
        std::lock_guard g(mu_);
        l.ssh.initialSize = size_.grid;
        launchGeneration = size_.generation;
    }
    if (auto e = ssh_->start(s, a, l.ssh)) {
        state_ = TransportState::Failed;
        if (cb_.onResolved) { HvAction f; f.kind = HvAction::Fail; f.text = "SSH 配置快照无效：" + e->field + " " + e->reason; cb_.onResolved(s, a, f); }
        if (cb_.onStateChanged) cb_.onStateChanged(s, a, TransportState::Failed, TransportFault::StartFailed);
        return;
    }
    sshStarted_ = true;
    {
        // Replay a resize received during start(). Serialize with live resize()
        // so a newer ConPTY size cannot be overwritten by an older replay.
        std::lock_guard g(mu_);
        if (size_.generation > launchGeneration) ssh_->resize(size_.grid, size_.generation);
    }
    if (closing_) ssh_->close(CloseReason::User);   // 解析完成与 close 交错
}

EnqueueResult HyperVTransport::enqueueUserInput(std::string_view b) { return sshStarted_ ? ssh_->enqueueUserInput(b) : EnqueueResult::Ok; }
EnqueueResult HyperVTransport::enqueueReply(std::string_view b) { return sshStarted_ ? ssh_->enqueueReply(b) : EnqueueResult::Ok; }
EnqueueResult HyperVTransport::beginPaste(std::size_t n, bool br) { return sshStarted_ ? ssh_->beginPaste(n, br) : EnqueueResult::Ok; }
EnqueueResult HyperVTransport::appendPaste(std::string_view b) { return sshStarted_ ? ssh_->appendPaste(b) : EnqueueResult::Ok; }
void HyperVTransport::resize(GridSize g, uint64_t gen) {
    std::lock_guard l(mu_);
    size_ = {g, gen};
    if (sshStarted_) ssh_->resize(g, gen);
}
void HyperVTransport::close(CloseReason r) {
    closing_ = true;
    if (cancel_) SetEvent(cancel_);
    if (th_.joinable()) th_.join();   // 解析线程最多再等一次查询超时（≤5 s，或 Start-VM 30 s）
    if (sshStarted_) ssh_->close(r);
}
std::optional<OutputChunk> HyperVTransport::tryPopOutput() { return sshStarted_ ? ssh_->tryPopOutput() : std::nullopt; }
TransportState HyperVTransport::state() const { return sshStarted_ ? ssh_->state() : state_.load(); }
bool HyperVTransport::waitClosed(DWORD ms) {
    if (sshStarted_) return ssh_->waitClosed(ms);
    TransportState st = state_.load();
    return st == TransportState::Closed || st == TransportState::Failed || st == TransportState::Idle;
}
uint32_t HyperVTransport::activeProcessCount() { return sshStarted_ ? ssh_->activeProcessCount() : 0; }

}  // namespace it2w
