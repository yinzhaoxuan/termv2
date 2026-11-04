// SPDX-License-Identifier: GPL-3.0-or-later
// Windows-only deterministic unit tests; no DNS, socket, sshd or agent required.
#include "direct_ssh_internal.h"
#include <cassert>
#include <thread>
#include <vector>

namespace it2w {
// Exercise the actual public queue API without creating a network owner. This
// peer is private to this test executable; it injects no production behavior.
struct DirectSshTransportTestPeer {
    static void queues() {
        DirectSshTransport t({});
        auto &p = *t.impl_;
        p.state = TransportState::Running;
        assert(t.enqueueUserInput(std::string(kUserQueueCap, 'x')) == EnqueueResult::Ok);
        assert(t.enqueueUserInput("x") == EnqueueResult::QueueFull);
        assert(p.input.userBytes() == kUserQueueCap);
        assert(t.enqueueReply("") == EnqueueResult::Ok);
        assert(t.enqueueReply(std::string(kReplyQueueCap + 1, 'r')) == EnqueueResult::TooLarge);
        assert(t.enqueueReply(std::string(kReplyQueueCap, 'r')) == EnqueueResult::Ok);
        assert(p.input.peekWrite().front() == 'r'); // replies precede new input
        assert(t.enqueueReply("r") == EnqueueResult::QueueFull);
        assert(t.state() == TransportState::Closed); // no owner: Fault closes immediately
        assert(t.enqueueUserInput("x") == EnqueueResult::InvalidState);
        assert(t.appendPaste("x") == EnqueueResult::InvalidState);
    }
    static void paste() {
        DirectSshTransport t({});
        auto &p = *t.impl_;
        p.state = TransportState::Running;
        assert(t.beginPaste(0, false) == EnqueueResult::Ok);
        assert(!p.input.hasPending());
        assert(t.beginPaste(kPasteMax + 1) == EnqueueResult::TooLarge);
        assert(t.beginPaste(kPasteMax) == EnqueueResult::Ok);
        assert(t.enqueueUserInput("later") == EnqueueResult::PasteInProgress);
        assert(t.appendPaste(std::string(kPasteMax, 'p')) == EnqueueResult::Ok);
        assert(p.input.userBytes() == kPasteMax + 12);
        assert(t.enqueueUserInput("later") == EnqueueResult::Ok);
        t.close(CloseReason::User);
    }
    static void outputAndSizes() {
        DirectSshTransport t({});
        auto &p = *t.impl_;
        for (size_t i = 0; i < kOutputPause / (16 * 1024); ++i)
            p.output.push(std::string(16 * 1024, 'o'));
        assert(p.output.readingPaused());
        while (p.output.bytes() > kOutputResume) assert(t.tryPopOutput());
        assert(!p.output.readingPaused());
        t.resize({0, 0}, 4);
        t.resize({137, 43}, 9);
        t.resize({80, 24}, 8);
        assert((p.sizes.desired == DirectSizeRequest{{137, 43}, 9}));
        t.close(CloseReason::User);
        t.resize({200, 100}, 10);
        assert(p.sizes.desired.generation == 9);
        for (size_t i = 0; i <= kCloseDrainExtra / (16 * 1024); ++i)
            p.output.push(std::string(16 * 1024, 'o'));
        assert(t.discardedBytes() == 16 * 1024);
        assert(t.tryPopOutput()->incompleteAfterThis);
    }
};
} // namespace it2w

int main() {
    using namespace it2w;
    unsigned callbacks = 0;
    SshCallbacks cb;
    cb.onStateChanged = [&](SessionId, AttemptId, TransportState, std::optional<TransportFault>) { ++callbacks; };
    cb.onFinished = [&](SessionId, AttemptId, const SshExitInfo &) { ++callbacks; };
    cb.onDiagnostic = [&](SessionId, AttemptId, const DiagRecord &) { ++callbacks; };
    {
        DirectSshTransport t(cb);
        // D-1: do not advertise OSC 52 read until a confirmation UI exists.
        assert(t.mode() == SshTransportMode::Direct && !t.supportsOsc52Read());
        assert(t.activeProcessCount() == 0 && !t.waitClosed(0));
        assert(t.enqueueUserInput("x") == EnqueueResult::InvalidState);
        // The invalid/unresolvable target and missing files are never accessed:
        // non-agent mode is rejected first, without starting an owner thread.
        SshLaunchOptions o;
        for (auto auth : {SshAuthMode::Interactive, SshAuthMode::KeyFile}) {
            o.target.auth = auth;
            auto error = t.start({1}, {1}, o);
            assert(error && error->field == "auth");
            assert(t.state() == TransportState::Idle && !t.waitClosed(0));
        }
        o.target.auth = SshAuthMode::Agent;
        auto error = t.start({1}, {1}, o);
        assert(error && error->field == "hostName");
        std::vector<std::thread> closers;
        for (int i = 0; i < 8; ++i) closers.emplace_back([&] { t.close(CloseReason::User); });
        for (auto &thread : closers) thread.join();
        assert(t.waitClosed(0) && t.state() == TransportState::Closed);
        t.close(CloseReason::Fault);
        assert(t.waitClosed(0));
        assert(!t.tryPopOutput() && !t.discardedBytes());
    }
    assert(callbacks == 0); // all callbacks belong to an actual owner attempt
    // Reentrant close on the owner before its first network operation: handles
    // must already be published, close must return, and terminal callbacks must
    // retain their required order. waitClosed must never join the owner itself.
    for (int i = 0; i < 32; ++i) {
        DirectSshTransport *active = nullptr;
        std::vector<TransportState> states;
        bool finished = false;
        SshCallbacks stopped;
        stopped.onStateChanged = [&](SessionId, AttemptId, TransportState state, std::optional<TransportFault>) {
            states.push_back(state);
            if (state == TransportState::Starting) {
                active->resize({0, 0}, 2);
                active->close(CloseReason::User);
                active->close(CloseReason::User);
                assert(!active->waitClosed(0));
            }
            if (state == TransportState::Closed || state == TransportState::Failed) assert(finished);
        };
        stopped.onFinished = [&](SessionId, AttemptId, const SshExitInfo &e) {
            assert(e.error == SshError::None && e.detail == "已取消");
            finished = true;
        };
        DirectSshTransport t(std::move(stopped));
        active = &t;
        SshLaunchOptions o;
        o.target.hostName = "must-not-resolve.invalid";
        o.target.user = "test";
        o.target.appKnownHostsFile = "C:/must-not-read/known_hosts";
        o.target.emptyKnownHostsFile = "C:/must-not-read/empty_known_hosts";
        assert(!t.start({1}, {static_cast<uint64_t>(i)}, o));
        assert(t.waitClosed(3000));
        assert((states == std::vector<TransportState>{TransportState::Starting,
            TransportState::Closing, TransportState::Closed}));
    }
    DirectSshTransportTestPeer::queues();
    DirectSshTransportTestPeer::paste();
    DirectSshTransportTestPeer::outputAndSizes();
}
