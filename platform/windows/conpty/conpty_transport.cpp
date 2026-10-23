// 未实机验证：见 conpty_transport.h。
#include "conpty_transport.h"
#include "ssh_snapshot.h"

namespace it2w {
namespace {
constexpr DWORD kReadChunk = 64 * 1024;
constexpr DWORD kCloseBudgetMs = 3000;
constexpr int kResizeFailLimit = 3;
uint64_t nowMs() { return GetTickCount64(); }
struct Lock {
    SRWLOCK &l;
    explicit Lock(SRWLOCK &x) : l(x) { AcquireSRWLockExclusive(&l); }
    ~Lock() { ReleaseSRWLockExclusive(&l); }
};
void closeH(HANDLE &h) { if (h) { CloseHandle(h); h = nullptr; } }
}  // namespace

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring buildCommandLineW(const LaunchSpec &spec) {
    std::vector<std::string> argv{narrow(spec.executable)};
    for (auto &a : spec.arguments) argv.push_back(narrow(a));
    return widen(buildWindowsCommandLine(argv));
}
std::vector<wchar_t> buildEnvironmentBlock(const LaunchSpec &spec) {
    EnvList env;
    for (auto &kv : spec.environment) env.emplace_back(narrow(kv.first), narrow(kv.second));
    env = filterChildEnvironment(env);  // 再次保证移除 SSH_ASKPASS*
    std::vector<wchar_t> block;
    for (auto &kv : env) {
        std::wstring line = widen(kv.first) + L"=" + widen(kv.second);
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

ConPtyTransport::ConPtyTransport(TransportCallbacks cb) : cb_(std::move(cb)) {
    writerWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    readerResume_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    monitorStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    closedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ConPtyTransport::~ConPtyTransport() {
    if (state_ != TransportState::Idle) {
        close(CloseReason::User);
        waitClosed(INFINITE);
    }
    for (HANDLE *h : {&reader_, &writer_, &monitor_, &closer_, &writerWake_, &readerResume_, &monitorStop_, &closedEvent_})
        closeH(*h);
}

void ConPtyTransport::setState(TransportState s, std::optional<TransportFault> f) {
    state_ = s;
    if (cb_.onStateChanged) cb_.onStateChanged(sid_, aid_, s, f);
}
void ConPtyTransport::diag(const char *stage, uint64_t ms, uint64_t bytes, std::string detail) {
    if (cb_.onDiagnostic) cb_.onDiagnostic(sid_, aid_, DiagRecord{stage, ms, bytes, std::move(detail)});
}

void ConPtyTransport::failStart(TransportFault f, const char *stage, DWORD err) {
    diag(stage, 0, 0, "Win32 错误 " + std::to_string(err));
    fault_ = f;
    // 逆序释放：线程先停，再关句柄
    writerStop_ = true;
    closePseudoConsoleOnce();
    if (reader_) {
        WaitForSingleObject(reader_, INFINITE);  // ConPTY 已关闭，读线程会读到 EOF；不提前释放 outRead
        closeH(reader_);
    }
    closeH(inWrite_); closeH(outRead_); closeH(process_); closeH(job_);
    if (snapshot_) DeleteFileW(snapshot_->c_str());
    setState(TransportState::Failed, f);
    SetEvent(closedEvent_);
}

bool ConPtyTransport::start(SessionId sid, AttemptId aid, const LaunchSpec &spec) {
    TransportState expected = TransportState::Idle;
    if (!state_.compare_exchange_strong(expected, TransportState::Starting)) return false;
    sid_ = sid; aid_ = aid; snapshot_ = spec.sshSnapshotFile;
    setState(TransportState::Starting);
    uint64_t t0 = nowMs();

    // 1. 网格 ≥ 20×5
    COORD size{SHORT(spec.initialSize.cols < 20 ? 20 : spec.initialSize.cols),
               SHORT(spec.initialSize.rows < 5 ? 5 : spec.initialSize.rows)};
    // 2. 两组管道，不可继承
    HANDLE inRead = nullptr, outWrite = nullptr;
    SECURITY_ATTRIBUTES noInherit{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (!CreatePipe(&inRead, &inWrite_, &noInherit, 0)) { failStart(TransportFault::StartFailed, "CreatePipe(in)", GetLastError()); return false; }
    if (!CreatePipe(&outRead_, &outWrite, &noInherit, 0)) {
        DWORD e = GetLastError(); closeH(inRead); failStart(TransportFault::StartFailed, "CreatePipe(out)", e); return false;
    }
    // 3. CreatePseudoConsole（flags=0）
    HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &hpc_);
    // 4. 无论成败立即关闭 ConPTY 端
    closeH(inRead); closeH(outWrite);
    if (FAILED(hr)) { hpc_ = nullptr; pcClosed_ = true; failStart(TransportFault::StartFailed, "CreatePseudoConsole", DWORD(hr)); return false; }
    // 5. 先启动读线程
    reader_ = CreateThread(nullptr, 0, readerMain, this, 0, nullptr);
    if (!reader_) { failStart(TransportFault::StartFailed, "CreateThread(reader)", GetLastError()); return false; }
    // 6. Job：KILL_ON_JOB_CLOSE，不允许 breakaway，不可继承
    job_ = CreateJobObjectW(&noInherit, nullptr);
    if (!job_) { failStart(TransportFault::StartFailed, "CreateJobObject", GetLastError()); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
        failStart(TransportFault::StartFailed, "SetInformationJobObject", GetLastError()); return false;
    }
    // 7. 属性列表：仅 PSEUDOCONSOLE
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) {
        failStart(TransportFault::StartFailed, "InitializeProcThreadAttributeList", GetLastError()); return false;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(hpc_), nullptr, nullptr)) {
        DWORD e = GetLastError(); DeleteProcThreadAttributeList(attrs);
        failStart(TransportFault::StartFailed, "UpdateProcThreadAttribute", e); return false;
    }
    // 8. 挂起创建，bInheritHandles=FALSE
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = buildCommandLineW(spec);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(L'\0');
    std::vector<wchar_t> env = buildEnvironmentBlock(spec);
    BOOL ok = CreateProcessW(spec.executable.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             spec.environment.empty() ? nullptr : env.data(),
                             spec.workingDirectory ? spec.workingDirectory->c_str() : nullptr, &si.StartupInfo, &pi);
    DWORD createErr = GetLastError();
    DeleteProcThreadAttributeList(attrs);  // 11（提前）：子进程创建后立即释放
    if (!ok) { failStart(TransportFault::StartFailed, "CreateProcessW", createErr); return false; }
    process_ = pi.hProcess;
    // 9. 关联 Job；失败则终止挂起的进程
    if (!AssignProcessToJobObject(job_, process_)) {
        DWORD e = GetLastError();
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, kCloseBudgetMs);
        CloseHandle(pi.hThread);
        failStart(TransportFault::JobAssignFailed, "AssignProcessToJobObject", e); return false;
    }
    // 10. 恢复并关闭线程句柄
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    // 11. 写线程与监视线程（进程退出 + 输入停滞计时）
    writer_ = CreateThread(nullptr, 0, writerMain, this, 0, nullptr);
    monitor_ = CreateThread(nullptr, 0, monitorMain, this, 0, nullptr);
    if (!writer_ || !monitor_) {
        // 进程已在 Job 中：走正常关闭路径
        fault_ = TransportFault::StartFailed;
        setState(TransportState::Running);
        close(CloseReason::Fault);
        return false;
    }
    diag("start", nowMs() - t0);
    setState(TransportState::Running);
    if (closeRequested_) { closeRequested_ = false; close(closeReason_); }  // Starting 期间的关闭请求
    return true;
}

// ---- 输入 ----
EnqueueResult ConPtyTransport::enqueueUserInput(std::string_view b) {
    EnqueueResult r;
    { Lock l(lock_); r = input_.enqueueUser(b); if (r == EnqueueResult::Ok) input_.notePending(nowMs()); }
    if (r == EnqueueResult::Ok) SetEvent(writerWake_);
    return r;
}
EnqueueResult ConPtyTransport::enqueueReply(std::string_view b) {
    EnqueueResult r;
    { Lock l(lock_); r = input_.enqueueReply(b); if (r == EnqueueResult::Ok) input_.notePending(nowMs()); }
    if (r == EnqueueResult::Ok) SetEvent(writerWake_);
    else if (r == EnqueueResult::QueueFull) diag("ReplyBackpressure", 0, b.size());  // 上层决定是否 close(Fault)
    return r;
}
EnqueueResult ConPtyTransport::beginPaste(std::size_t n, bool bracketed) {
    EnqueueResult r;
    { Lock l(lock_); r = input_.beginPaste(n, bracketed); if (r == EnqueueResult::Ok) input_.notePending(nowMs()); }
    if (r == EnqueueResult::Ok) SetEvent(writerWake_);
    return r;
}
EnqueueResult ConPtyTransport::appendPaste(std::string_view body) {
    EnqueueResult r;
    { Lock l(lock_); r = input_.appendPaste(body); if (r == EnqueueResult::Ok) input_.notePending(nowMs()); }
    if (r == EnqueueResult::Ok) SetEvent(writerWake_);
    return r;
}
void ConPtyTransport::cancelPasteBody() {
    { Lock l(lock_); input_.cancelPasteBody(); input_.notePending(nowMs()); }
    SetEvent(writerWake_);
}

DWORD WINAPI ConPtyTransport::writerMain(LPVOID p) { static_cast<ConPtyTransport *>(p)->writerLoop(); return 0; }
void ConPtyTransport::writerLoop() {
    std::string buf;
    while (!writerStop_) {
        {
            Lock l(lock_);
            auto v = input_.peekWrite();
            buf.assign(v.substr(0, kReadChunk));
        }
        if (buf.empty()) { WaitForSingleObject(writerWake_, INFINITE); continue; }
        DWORD written = 0;
        // 同步写；关闭时由 Closer 以 CancelSynchronousIo 取消（规格 §1.4 第 3 步，二选一待 X04 实测）
        BOOL ok = WriteFile(inWrite_, buf.data(), DWORD(buf.size()), &written, nullptr);
        if (!ok) {
            DWORD e = GetLastError();
            if (writerStop_ || e == ERROR_OPERATION_ABORTED) break;
            diag("WriteFile", 0, 0, "Win32 错误 " + std::to_string(e));
            fault_ = TransportFault::PipeBroken;
            close(CloseReason::Fault);
            break;
        }
        Lock l(lock_);
        input_.consumed(written, nowMs());
    }
    closeH(inWrite_);  // 写线程拥有 inWrite
}

// ---- 输出 ----
DWORD WINAPI ConPtyTransport::readerMain(LPVOID p) { static_cast<ConPtyTransport *>(p)->readerLoop(); return 0; }
void ConPtyTransport::readerLoop() {
    std::string buf(kReadChunk, '\0');
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(outRead_, buf.data(), kReadChunk, &n, nullptr) || n == 0) break;  // EOF / ERROR_BROKEN_PIPE
        OutputQueue::PushResult r;
        { Lock l(lock_); r = output_.push(buf.substr(0, n)); }
        if (r != OutputQueue::PushResult::Discarded && cb_.onOutputAvailable) cb_.onOutputAvailable(sid_, aid_);
        if (r == OutputQueue::PushResult::Overflow) { fault_ = TransportFault::OutputLimit; close(CloseReason::Fault); }
        for (;;) {  // 3 MiB 停读，等核心消费到 1 MiB；关闭时 beginClosing 解除暂停
            bool paused;
            { Lock l(lock_); paused = output_.readingPaused(); }
            if (!paused) break;
            WaitForSingleObject(readerResume_, INFINITE);
        }
    }
    // 读线程只读不关：outRead 由 Closer 在本线程退出后关闭（§1.4 第 6 步）
}

std::optional<OutputChunk> ConPtyTransport::tryPopOutput() {
    std::optional<OutputChunk> out;
    bool wake = false;
    {
        Lock l(lock_);
        auto s = output_.pop();
        if (!s) return std::nullopt;
        out = OutputChunk{std::move(*s), output_.incomplete() && output_.bytes() == 0};
        if (output_.shouldResume()) { output_.resumed(); wake = true; }
    }
    if (wake) SetEvent(readerResume_);
    return out;
}
uint64_t ConPtyTransport::discardedBytes() { Lock l(lock_); return output_.discardedBytes(); }

// ---- 监视：进程退出与输入停滞 ----
DWORD WINAPI ConPtyTransport::monitorMain(LPVOID p) { static_cast<ConPtyTransport *>(p)->monitorLoop(); return 0; }
void ConPtyTransport::monitorLoop() {
    HANDLE hs[2] = {process_, monitorStop_};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 250);
        if (w == WAIT_OBJECT_0) { close(CloseReason::ProcessExited); return; }  // 不等 EOF
        if (w == WAIT_OBJECT_0 + 1) return;
        bool stalled;
        { Lock l(lock_); stalled = input_.stalled(nowMs()); }
        if (stalled) {
            diag("InputStalled", kStallMs, 0, "输入传输停滞");
            fault_ = TransportFault::InputStalled;
            close(CloseReason::Fault);
            return;
        }
    }
}

// ---- resize：只在 Running，与 Close 串行 ----
void ConPtyTransport::resize(GridSize g, uint64_t generation) {
    HRESULT hr;
    {
        Lock l(pcLock_);
        if (state_ != TransportState::Running || pcClosed_ || !hpc_) return;
        COORD c{SHORT(g.cols < 20 ? 20 : g.cols), SHORT(g.rows < 5 ? 5 : g.rows)};
        hr = ResizePseudoConsole(hpc_, c);
    }
    if (FAILED(hr)) {
        diag("ResizePseudoConsole", 0, generation, "HRESULT " + std::to_string(unsigned(hr)));
        if (++resizeFailures_ >= kResizeFailLimit) { fault_ = TransportFault::ResizeFailed; close(CloseReason::Fault); }
    } else resizeFailures_ = 0;
}

void ConPtyTransport::closePseudoConsoleOnce() {
    Lock l(pcLock_);
    if (pcClosed_.exchange(true) || !hpc_) return;
    HPCON h = hpc_;
    hpc_ = nullptr;
    // 注意：在锁内调用 ClosePseudoConsole 会让 resize 等待它返回；读线程持续读取，旧系统上也不会死锁。
    ClosePseudoConsole(h);
}

// ---- 关闭（§1.4）----
void ConPtyTransport::close(CloseReason why) {
    TransportState s = state_;
    if (s == TransportState::Idle || s == TransportState::Closed || s == TransportState::Failed) return;
    if (s == TransportState::Starting) { closeReason_ = why; closeRequested_ = true; return; }
    if (closeStarted_.exchange(true)) return;  // 幂等
    closeReason_ = why;
    closer_ = CreateThread(nullptr, 0, closerMain, this, 0, nullptr);
    if (!closer_) doClose();  // 无法建线程时在当前线程执行（调用方不得是 UI 线程）
}
DWORD WINAPI ConPtyTransport::closerMain(LPVOID p) { static_cast<ConPtyTransport *>(p)->doClose(); return 0; }

void ConPtyTransport::doClose() {
    closeStartMs_ = nowMs();
    auto stage = [&](const char *name, uint64_t t) { diag(name, nowMs() - t); return nowMs(); };
    // 1. Closing；拒绝新输入；输出转排空路径
    {
        Lock l(lock_);
        input_.close();
        output_.beginClosing();
    }
    setState(TransportState::Closing, fault_);
    SetEvent(readerResume_);
    SetEvent(monitorStop_);
    uint64_t t = nowMs();
    // 3. 写线程：停止并取消阻塞中的 WriteFile
    writerStop_ = true;
    SetEvent(writerWake_);
    if (writer_) {
        for (int i = 0; i < 20 && WaitForSingleObject(writer_, 50) == WAIT_TIMEOUT; ++i) CancelSynchronousIo(writer_);
        if (WaitForSingleObject(writer_, 0) == WAIT_TIMEOUT) diag("writer-cancel-timeout", nowMs() - t);
    }
    t = stage("close.writer", t);
    // 4. 终止整个进程树
    if (job_) TerminateJobObject(job_, 1);
    t = stage("close.terminateJob", t);
    // 5. ClosePseudoConsole（幂等）
    closePseudoConsoleOnce();
    t = stage("close.closePseudoConsole", t);
    // 6. 读线程读到 EOF 后关闭 outRead
    bool timedOut = false;
    uint64_t budgetLeft = kCloseBudgetMs > (nowMs() - closeStartMs_) ? kCloseBudgetMs - (nowMs() - closeStartMs_) : 0;
    if (reader_ && WaitForSingleObject(reader_, DWORD(budgetLeft)) == WAIT_TIMEOUT) {
        // 超时：记录卡住阶段；不提前释放读线程仍在使用的 outRead
        timedOut = true;
        diag("close.reader-timeout", nowMs() - t);
        WaitForSingleObject(reader_, INFINITE);
    }
    closeH(outRead_);
    closeH(reader_);
    { Lock l(lock_); if (output_.incomplete()) diag("close.drain-discarded", 0, output_.discardedBytes(), "数据不完整"); }
    t = stage("close.reader", t);
    // 7. 进程退出码，关闭 hProcess，最后关闭 hJob
    if (process_) {
        WaitForSingleObject(process_, INFINITE);  // Job 已终止，应很快返回
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        exitCode_ = code;
        closeH(process_);
        if (cb_.onExited) cb_.onExited(sid_, aid_, exitCode_);
    }
    if (monitor_) WaitForSingleObject(monitor_, INFINITE);
    { Lock l(lock_); closeH(job_); }  // 与 activeProcessCount 串行
    t = stage("close.process", t);
    // 9. 删除 SSH 配置快照
    if (snapshot_) DeleteFileW(snapshot_->c_str());
    diag("close.total", nowMs() - closeStartMs_);
    if (timedOut && !fault_) fault_ = TransportFault::CloseTimeout;
    // 8. 最终状态（核心执行器排空由上层在收到 Closed/Failed 后完成）
    setState(closeReason_ == CloseReason::Fault ? TransportState::Failed : TransportState::Closed, fault_);
    SetEvent(closedEvent_);
}

uint32_t ConPtyTransport::activeProcessCount() {
    Lock l(lock_);
    if (state_ != TransportState::Running || !job_) return 0;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) return 0;
    return info.ActiveProcesses;
}

bool ConPtyTransport::waitClosed(DWORD ms) { return WaitForSingleObject(closedEvent_, ms) == WAIT_OBJECT_0; }

}  // namespace it2w
