// t03a_probe：T03A 最小读写驱动。经 ConPtyTransport 启动一个程序，把本进程 stdin 的行
// 作为用户输入写入（行尾 CR），原始输出字节写到 stdout 与可选日志文件，结束时打印诊断。
// 用法：
//   t03a_probe [--log out.bin] [--seconds N] [--close-after-ms N] [--loops N]
//              [--size COLSxROWS] [--send TEXT] [--paste-bytes N] [--ssh-snapshot FILE] -- <exe 绝对路径> [参数...]
// 例：t03a_probe --seconds 10 -- C:\Windows\System32\cmd.exe
//     t03a_probe --loops 100 --close-after-ms 500 -- C:\Windows\System32\cmd.exe /c "for /l %i in () do @echo xxxxx"   (X01)
#include "conpty_transport.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

using namespace it2w;

namespace {
const char *stateName(TransportState s) {
    switch (s) {
    case TransportState::Idle: return "Idle"; case TransportState::Starting: return "Starting";
    case TransportState::Running: return "Running"; case TransportState::Closing: return "Closing";
    case TransportState::Closed: return "Closed"; case TransportState::Failed: return "Failed";
    }
    return "?";
}
HANDLE gOutputEvent;
DWORD WINAPI stdinPump(LPVOID p) {
    auto *t = static_cast<ConPtyTransport *>(p);
    char line[4096];
    while (std::fgets(line, sizeof line, stdin)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        s += '\r';
        auto r = t->enqueueUserInput(s);
        if (r != EnqueueResult::Ok) std::fprintf(stderr, "[probe] enqueue 失败 %d\n", int(r));
    }
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring logPath, snapshot, sendText;
    DWORD seconds = 0, closeAfterMs = 0; int loops = 1; size_t pasteBytes = 0;
    GridSize size{120, 30};
    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--") { ++i; break; }
        else if (a == L"--log") logPath = next();
        else if (a == L"--seconds") seconds = wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--close-after-ms") closeAfterMs = wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--loops") loops = _wtoi(next().c_str());
        else if (a == L"--paste-bytes") pasteBytes = wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--send") sendText = next();
        else if (a == L"--ssh-snapshot") snapshot = next();
        else if (a == L"--size") { unsigned c = 0, r = 0; swscanf(next().c_str(), L"%ux%u", &c, &r); size = {uint16_t(c), uint16_t(r)}; }
        else { std::fwprintf(stderr, L"未知参数 %ls\n", a.c_str()); return 2; }
    }
    if (i >= argc) { std::fprintf(stderr, "用法见源码注释：需要 -- <exe> [args]\n"); return 2; }
    LaunchSpec spec;
    spec.executable = argv[i];
    for (int k = i + 1; k < argc; ++k) spec.arguments.push_back(argv[k]);
    spec.initialSize = size;
    if (!snapshot.empty()) spec.sshSnapshotFile = snapshot;
    // 环境：继承当前环境，过滤 SSH_ASKPASS*（设计 13.5 / SSH-CFG-01 C22）
    if (wchar_t *env = GetEnvironmentStringsW()) {
        for (wchar_t *p = env; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            auto eq = kv.find(L'=', 1);
            if (eq != std::wstring::npos) spec.environment.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        FreeEnvironmentStringsW(env);
    }

    FILE *log = logPath.empty() ? nullptr : _wfopen(logPath.c_str(), L"wb");
    gOutputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int rc = 0;
    for (int loop = 0; loop < loops; ++loop) {
        TransportCallbacks cb;
        cb.onStateChanged = [&](SessionId, AttemptId a, TransportState s, std::optional<TransportFault> f) {
            std::fprintf(stderr, "[probe] attempt %llu 状态 %s%s%d\n", (unsigned long long)a.value, stateName(s),
                         f ? " fault=" : "", f ? int(*f) : -1);
        };
        cb.onOutputAvailable = [](SessionId, AttemptId) { SetEvent(gOutputEvent); };
        cb.onExited = [](SessionId, AttemptId, uint32_t code) { std::fprintf(stderr, "[probe] 退出码 %u\n", code); };
        cb.onDiagnostic = [](SessionId, AttemptId, const DiagRecord &d) {
            std::fprintf(stderr, "[probe] 诊断 %s %llums bytes=%llu %s\n", d.stage.c_str(),
                         (unsigned long long)d.elapsedMs, (unsigned long long)d.bytes, d.detail.c_str());
        };
        ConPtyTransport t(cb);
        if (!t.start(SessionId{1}, AttemptId{uint64_t(loop + 1)}, spec)) { rc = 1; break; }
        if (loop == 0 && loops == 1) CreateThread(nullptr, 0, stdinPump, &t, 0, nullptr);
        if (!sendText.empty()) t.enqueueUserInput(narrow(sendText) + "\r");
        if (pasteBytes) {  // IO-01 I01/I03
            auto r = t.beginPaste(pasteBytes);
            std::fprintf(stderr, "[probe] beginPaste(%zu) = %d\n", pasteBytes, int(r));
            if (r == EnqueueResult::Ok) t.appendPaste(std::string(pasteBytes, 'p'));
        }
        ULONGLONG t0 = GetTickCount64(), closeAt = closeAfterMs ? t0 + closeAfterMs : seconds ? t0 + seconds * 1000ull : 0;
        uint64_t total = 0; bool closing = false;
        for (;;) {
            while (auto c = t.tryPopOutput()) {
                total += c->bytes.size();
                if (loops == 1) { fwrite(c->bytes.data(), 1, c->bytes.size(), stdout); fflush(stdout); }
                if (log) fwrite(c->bytes.data(), 1, c->bytes.size(), log);
            }
            if (t.waitClosed(0)) break;
            if (!closing && closeAt && GetTickCount64() >= closeAt) {
                closing = true;
                ULONGLONG c0 = GetTickCount64();
                t.close(CloseReason::User);
                std::fprintf(stderr, "[probe] close() 调用返回耗时 %llums\n", GetTickCount64() - c0);
            }
            WaitForSingleObject(gOutputEvent, 50);
        }
        while (auto c = t.tryPopOutput()) { total += c->bytes.size(); if (log) fwrite(c->bytes.data(), 1, c->bytes.size(), log); }
        std::fprintf(stderr, "[probe] loop %d 输出 %llu 字节，丢弃 %llu，总耗时 %llums\n", loop, (unsigned long long)total,
                     (unsigned long long)t.discardedBytes(), GetTickCount64() - t0);
        if (t.state() == TransportState::Failed) rc = 1;
    }
    if (log) fclose(log);
    return rc;
}
