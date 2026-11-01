#include "direct_ssh_internal.h"
#include <cstdlib>

namespace it2w {
using namespace direct_ssh;

void CALLBACK DirectSshTransport::Impl::authExpired(PTP_CALLBACK_INSTANCE, void *p, PTP_TIMER) {
    auto &self = *static_cast<Impl *>(p);
    {
        Lock g(self.lock);
        if (!self.authDeadline || now() < self.authDeadline || self.closeRequested) return;
        self.authTimedOut = true;
    }
    // Timer callbacks only request closure. The canceller still exclusively
    // performs CancelSynchronousIo; neither thread touches protocol resources.
    self.requestClose(CloseReason::Fault);
}

void DirectSshTransport::Impl::authenticate() {
    authInProgress = true;
    setPhase(DirectStage::Agent, "agent");
    const auto until = now() + 60000;
    authTimer = CreateThreadpoolTimer(authExpired, this, nullptr);
    if (!authTimer) fail(DirectFailureKind::Other, static_cast<int>(GetLastError()), "auth timer");
    {
        Lock g(lock);
        authDeadline = until;
    }
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-60000LL * 10000);
    FILETIME time{due.LowPart, due.HighPart};
    SetThreadpoolTimer(authTimer, &time, 0, 0);
    // The timer is disarmed/joined in releaseAgent on both success and failure.
    const char *env = std::getenv("SSH_AUTH_SOCK");
    const auto path = directAgentPipe(env ? env : "");
    diag("ssh.direct.agent", 0, 0, path);
    agent = libssh2_agent_init(session);
    if (!agent) fail(DirectFailureKind::Other, LIBSSH2_ERROR_ALLOC);
    libssh2_agent_set_identity_path(agent, path.c_str());
    const char *selected = libssh2_agent_get_identity_path(agent);
    if (!selected || path != selected)
        fail(DirectFailureKind::AgentUnavailable, 0, "cannot set explicit agent pipe");

    auto gated = [&](auto call) {
        return retry([&] {
            struct AuthIoGate {
                Impl &self;
                explicit AuthIoGate(Impl &s) : self(s) { self.inAgentIo = true; }
                ~AuthIoGate() {
                    // Observe cancellation BEFORE lowering the gate. A pipe
                    // interrupted here makes protocol shutdown unsafe.
                    if (self.closeRequested) self.authInterrupted = true;
                    self.inAgentIo = false;
                }
            } gate(*this);
            checkpoint(until); // closes the gate-before-call cancellation race
            return call();
        }, until);
    };
    int rc = gated([&] {
        int result = libssh2_agent_connect(agent);
        if (!result) agentConnected = true; // publish before post-call cancellation
        return result;
    });
    if (rc) fail(DirectFailureKind::AgentUnavailable, rc, "cannot open " + path);
    rc = gated([&] { return libssh2_agent_list_identities(agent); });
    if (rc) fail(DirectFailureKind::AgentUnavailable, rc, "agent list identities");
    setPhase(DirectStage::Auth, "auth");
    libssh2_agent_publickey *identity = nullptr, *previous = nullptr;
    bool accepted = false;
    for (unsigned tried = 0; tried < 64; ++tried) {
        checkpoint(until);
        rc = libssh2_agent_get_identity(agent, &identity, previous);
        if (rc == 1) break;
        check(rc, "agent get identity");
        rc = gated([&] { return libssh2_agent_userauth(agent, target.user.c_str(), identity); });
        if (!rc) { accepted = true; break; }
        if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED && rc != LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
            check(rc, "agent userauth");
        previous = identity;
    }
    if (!accepted || !libssh2_userauth_authenticated(session))
        fail(DirectFailureKind::NoAcceptedIdentity, rc, "agent has no accepted identity (limit 64)");
    releaseAgent();
    checkpoint(until);
    authInProgress = false;
}

void DirectSshTransport::Impl::releaseAgent() {
    {
        Lock g(lock);
        authDeadline = 0;
    }
    if (authTimer) {
        SetThreadpoolTimer(authTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(authTimer, TRUE);
        CloseThreadpoolTimer(authTimer);
        authTimer = nullptr;
    }
    if (agent) {
        if (agentConnected) libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        agent = nullptr;
        agentConnected = false;
    }
}

} // namespace it2w
