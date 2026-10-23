#include "ssh_transport.h"
#include <cstdio>

namespace it2w {

SshTransport::SshTransport(SshCallbacks cb) : cb_(std::move(cb)) {
    TransportCallbacks tc;
    tc.onStateChanged = [this](SessionId s, AttemptId a, TransportState st, std::optional<TransportFault> f) {
        if (cb_.onStateChanged) cb_.onStateChanged(s, a, st, f);
        if (st == TransportState::Failed && f == TransportFault::StartFailed && !finished_) {
            finished_ = true;
            SshExitInfo e; e.error = SshError::SshNotFound; e.detail = "ssh.exe 启动失败";
            { std::lock_guard<std::mutex> g(tailMu_); e.outputTail = utf8Tail(tail_.text(), kSshOutputTailBytes); }
            if (cb_.onFinished) cb_.onFinished(s, a, e);
        }
    };
    tc.onOutputAvailable = [this](SessionId s, AttemptId a) { if (cb_.onOutputAvailable) cb_.onOutputAvailable(s, a); };
    tc.onExited = [this](SessionId s, AttemptId a, uint32_t code) { handleExit(s, a, code); };
    tc.onOutputTap = [this](SessionId, AttemptId, std::string_view b) { std::lock_guard<std::mutex> g(tailMu_); tail_.append(b); };
    tc.onDiagnostic = [this](SessionId s, AttemptId a, const DiagRecord &d) { if (cb_.onDiagnostic) cb_.onDiagnostic(s, a, d); };
    pty_ = std::make_unique<ConPtyTransport>(std::move(tc));
}

SshTransport::~SshTransport() { pty_.reset(); }

std::wstring SshTransport::defaultSshExe() {
    wchar_t root[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(root, MAX_PATH);
    std::wstring p(root, n);
    return p + L"\\OpenSSH\\ssh.exe";
}

std::optional<SnapshotError> SshTransport::start(SessionId sid, AttemptId aid, const SshLaunchOptions &o) {
    auto snap = buildSshSnapshot(o.target);
    if (snap.error) return snap.error;
    finished_ = false;

    std::wstring exe = o.sshExe.empty() ? defaultSshExe() : o.sshExe;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        finished_ = true;
        SshExitInfo e; e.error = SshError::SshNotFound; e.detail = narrow(exe);
        if (cb_.onFinished) cb_.onFinished(sid, aid, e);
        return std::nullopt;
    }

    // 快照落盘：UTF-8 无 BOM；CREATE_NEW 防止覆盖；仅当前用户可读由目录 ACL 保证（T03A §3.3）
    wchar_t name[96];
    swprintf(name, 96, L"\\ssh-%llu-%llu.conf", (unsigned long long)sid.value, (unsigned long long)aid.value);
    std::wstring path = o.snapshotDir + name;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return SnapshotError{"snapshotDir", "无法创建快照文件"};
    DWORD wr = 0;
    BOOL ok = WriteFile(h, snap.text->data(), DWORD(snap.text->size()), &wr, nullptr) && wr == snap.text->size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(path.c_str()); return SnapshotError{"snapshotDir", "写入快照失败"}; }

    LaunchSpec ls;
    ls.executable = exe;
    auto args = sshArguments(narrow(exe), narrow(path), o.target.alias);
    for (size_t i = 1; i < args.size(); ++i) ls.arguments.push_back(widen(args[i]));
    ls.initialSize = o.initialSize;
    ls.sshSnapshotFile = path;  // attempt 结束时删除
    EnvList env = o.environment;
    if (env.empty()) {
        if (wchar_t *blk = GetEnvironmentStringsW()) {
            for (wchar_t *p = blk; *p; p += wcslen(p) + 1) {
                std::wstring kv = p;
                size_t eq = kv.find(L'=', 1);
                if (eq == std::wstring::npos) continue;
                env.emplace_back(narrow(kv.substr(0, eq)), narrow(kv.substr(eq + 1)));
            }
            FreeEnvironmentStringsW(blk);
        }
    }
    for (auto &kv : filterChildEnvironment(env)) ls.environment.emplace_back(widen(kv.first), widen(kv.second));
    {
        std::lock_guard<std::mutex> g(tailMu_);
        tail_ = OutputTail();
    }
    pty_->start(sid, aid, ls);  // 失败经 onStateChanged(Failed, StartFailed) → onFinished(SshNotFound)
    return std::nullopt;
}

std::optional<OutputChunk> SshTransport::tryPopOutput() {
    auto c = pty_->tryPopOutput();
    return c;  // 尾部由读线程 onOutputTap 记录
}

void SshTransport::handleExit(SessionId s, AttemptId a, uint32_t code) {
    if (finished_) return;
    finished_ = true;
    // 尾部在读线程入队前记录：UI 未取走的输出也在其中
    std::string t;
    { std::lock_guard<std::mutex> g(tailMu_); t = tail_.text(); }
    SshExitInfo e = mapSshExit(code, t);
    e.outputTail = utf8Tail(t, kSshOutputTailBytes);
    if (cb_.onFinished) cb_.onFinished(s, a, e);
}

}  // namespace it2w
