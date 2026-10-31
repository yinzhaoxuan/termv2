// Hyper-V startup handoff regression: no PowerShell, VM, or SSH process required.
#include "hyperv_transport.h"
#include "hyperv_query.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <utility>
#include <vector>

using namespace it2w;

namespace it2w {
HvQueryResult runHyperVScript(const std::string &id, bool, DWORD, HANDLE) {
    HvVm vm;
    vm.id = id;
    vm.state = "Running";
    vm.adapters.push_back({"adapter", "Ethernet", {}, {"192.0.2.1"}});
    return {HvQueryError::None, {}, {vm}};
}
}  // namespace it2w

namespace {
struct FakeSsh final : SshSessionTransport {
    GridSize initial;
    std::vector<std::pair<GridSize, uint64_t>> sizes;
    std::function<void()> duringStart;
    std::promise<void> started;
    bool osc52Read = false;
    bool closed = false;

    std::optional<SnapshotError> start(SessionId, AttemptId, const SshLaunchOptions &o) override {
        initial = o.initialSize;
        if (duringStart) duringStart();
        started.set_value();
        return std::nullopt;
    }
    EnqueueResult enqueueUserInput(std::string_view) override { return EnqueueResult::Ok; }
    EnqueueResult enqueueReply(std::string_view) override { return EnqueueResult::Ok; }
    EnqueueResult beginPaste(std::size_t, bool) override { return EnqueueResult::Ok; }
    EnqueueResult appendPaste(std::string_view) override { return EnqueueResult::Ok; }
    void resize(GridSize g, uint64_t gen) override { sizes.emplace_back(g, gen); }
    void close(CloseReason) override { closed = true; }
    std::optional<OutputChunk> tryPopOutput() override { return std::nullopt; }
    uint64_t discardedBytes() const override { return 0; }
    TransportState state() const override { return TransportState::Running; }
    bool waitClosed(DWORD) override { return closed; }
    uint32_t activeProcessCount() override { return 0; }
    bool supportsOsc52Read() const override { return osc52Read; }
    SshTransportMode mode() const override { return osc52Read ? SshTransportMode::Direct : SshTransportMode::ConPty; }
};

void handoff(bool pendingResize, bool liveResize) {
    auto fake = std::make_unique<FakeSsh>();
    auto *ssh = fake.get();
    ssh->osc52Read = pendingResize;
    auto started = ssh->started.get_future();
    HyperVTransport *transport = nullptr;
    HyperVCallbacks cb;
    cb.onResolved = [&](SessionId, AttemptId, const HvAction &res) {
        assert(res.kind == HvAction::Connect);
        transport->resize({100, 30}, 7);  // Resolution-time resize becomes launch size.
    };
    HyperVTransport hv(std::move(cb), [&](SshCallbacks) { return std::move(fake); });
    transport = &hv;
    assert(hv.supportsOsc52Read() == pendingResize);  // Available before SSH starts.
    if (pendingResize) {
        ssh->duringStart = [&] {
            hv.resize({120, 40}, 8);
            hv.resize({137, 43}, 9);  // Previously lost between size copy and sshStarted_.
        };
    }
    HyperVLaunch launch;
    launch.policy.vmId = "01234567-89ab-cdef-0123-456789abcdef";
    launch.ssh.initialSize = {80, 24};
    hv.start(SessionId{1}, AttemptId{1}, std::move(launch));
    assert(started.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    if (liveResize) hv.resize({150, 50}, 10);
    hv.close(CloseReason::User);  // Joins worker before inspecting the fake.
    assert(ssh->initial.cols == 100 && ssh->initial.rows == 30);
    assert(hv.supportsOsc52Read() == pendingResize);
    assert(ssh->closed);
    if (liveResize) {
        assert(!ssh->sizes.empty());
        const auto &[grid, gen] = ssh->sizes.back();
        assert(grid.cols == 150 && grid.rows == 50 && gen == 10);
    } else if (pendingResize) {
        assert(ssh->sizes.size() == 1);
        const auto &[grid, gen] = ssh->sizes.front();
        assert(grid.cols == 137 && grid.rows == 43 && gen == 9);
    } else {
        assert(ssh->sizes.empty());  // No redundant replay when start saw the latest size.
    }
}
}  // namespace

int main() {
    handoff(false, false);
    handoff(true, false);
    handoff(true, true);
    std::puts("HV-02 transport handoff ok");
}
