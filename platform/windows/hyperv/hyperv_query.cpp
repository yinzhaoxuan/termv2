#include "hyperv_query.h"
#include "conpty_transport.h"   // widen/narrow
#include "ssh_snapshot.h"       // EnvList / filterChildEnvironment
#include <algorithm>
#include <string>
#include <vector>

namespace it2w {
namespace {

// 固定脚本：只读 $env:IT2W_VMID / $env:IT2W_ACTION；输出单个 JSON 对象（v=1）
const wchar_t kScript[] =
    L"$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
    L"function Emit($o){[Console]::Out.Write(($o|ConvertTo-Json -Compress -Depth 6))};"
    L"try{"
    L"if(-not(Get-Command Get-VM -ErrorAction SilentlyContinue)){Emit @{v=1;ok=$false;err='ModuleMissing'};exit 0};"
    L"$id=$env:IT2W_VMID;"
    L"if($id){$vms=@(Get-VM -Id ([guid]$id))}else{$vms=@(Get-VM)};"
    L"if($env:IT2W_ACTION -eq 'start' -and $vms.Count -eq 1){Start-VM -VM $vms[0]|Out-Null;$vms=@(Get-VM -Id $vms[0].Id)};"
    L"$list=@(foreach($vm in $vms){@{id=$vm.Id.ToString();name=$vm.Name;state=$vm.State.ToString();"
    L"adapters=@(foreach($a in @(Get-VMNetworkAdapter -VM $vm)){@{id=[string]$a.Id;name=[string]$a.Name;mac=[string]$a.MacAddress;ips=@($a.IPAddresses|ForEach-Object{[string]$_})}})}});"
    L"Emit @{v=1;ok=$true;vms=$list}"
    L"}catch{"
    L"$c=[string]$_.CategoryInfo.Category;$q=[string]$_.FullyQualifiedErrorId;$e='Failed';"
    L"if($c -eq 'PermissionDenied' -or $c -eq 'SecurityError' -or $_.Exception -is [System.UnauthorizedAccessException] -or $q -match 'AccessDenied|Unauthorized'){$e='AccessDenied'}"
    L"elseif($c -eq 'ObjectNotFound' -or $q -match 'NotFound'){$e='NotFound'};"
    L"Emit @{v=1;ok=$false;err=$e;msg=[string]$_.Exception.Message}}";

std::wstring base64Utf16(const std::wstring &s) {
    static const char *tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> b;
    for (wchar_t c : s) { b.push_back(static_cast<unsigned char>(c & 0xff)); b.push_back(static_cast<unsigned char>((c >> 8) & 0xff)); }
    std::wstring o;
    for (size_t i = 0; i < b.size(); i += 3) {
        unsigned v = b[i] << 16 | (i + 1 < b.size() ? b[i + 1] << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
        o += wchar_t(tb[(v >> 18) & 63]); o += wchar_t(tb[(v >> 12) & 63]);
        o += i + 1 < b.size() ? wchar_t(tb[(v >> 6) & 63]) : L'=';
        o += i + 2 < b.size() ? wchar_t(tb[v & 63]) : L'=';
    }
    return o;
}

// 当前环境去掉 IT2W_*，加上本次参数；按名称排序（CreateProcess 要求）
std::wstring envBlock(const std::string &vmId, bool start) {
    EnvList env;
    if (wchar_t *blk = GetEnvironmentStringsW()) {
        for (wchar_t *p = blk; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            size_t eq = kv.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            std::string k = narrow(kv.substr(0, eq));
            if (_stricmp(k.c_str(), "IT2W_VMID") == 0 || _stricmp(k.c_str(), "IT2W_ACTION") == 0) continue;
            env.emplace_back(k, narrow(kv.substr(eq + 1)));
        }
        FreeEnvironmentStringsW(blk);
    }
    if (!vmId.empty()) env.emplace_back("IT2W_VMID", vmId);
    if (start) env.emplace_back("IT2W_ACTION", "start");
    std::wstring out;
    env = filterChildEnvironment(env);
    std::sort(env.begin(), env.end(), [](auto &a, auto &b) { return _stricmp(a.first.c_str(), b.first.c_str()) < 0; });
    for (auto &kv : env) { out += widen(kv.first) + L"=" + widen(kv.second); out.push_back(L'\0'); }
    out.push_back(L'\0');
    return out;
}

}  // namespace

HvQueryResult runHyperVScript(const std::string &vmIdIn, bool start, DWORD timeoutMs, HANDLE cancel) {
    HvQueryResult r;
    std::string vmId;
    if (!vmIdIn.empty()) {
        auto n = normalizeVmId(vmIdIn);
        if (!n) { r.err = HvQueryError::NotFound; r.msg = "VM ID 格式无效"; return r; }
        vmId = *n;
    }
    wchar_t sys[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { r.err = HvQueryError::LaunchFailed; r.msg = narrow(exe); return r; }
    std::wstring cmd = L"\"" + exe + L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + base64Utf16(kScript);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { r.err = HvQueryError::LaunchFailed; return r; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring env = envBlock(vmId, start);
    std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    BOOL ok = CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(), nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd); if (job) CloseHandle(job);
        r.err = HvQueryError::LaunchFailed; r.msg = "CreateProcess " + std::to_string(GetLastError());
        return r;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string out;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool timedOut = false, exited = false;
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char buf[4096]; DWORD got = 0;
            if (!ReadFile(rd, buf, avail < sizeof buf ? avail : DWORD(sizeof buf), &got, nullptr) || !got) break;
            if (out.size() < (1u << 20)) out.append(buf, got);
        }
        if (exited) break;
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { timedOut = true; break; }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { timedOut = true; break; }
        exited = WaitForSingleObject(pi.hProcess, DWORD(deadline - now < 50 ? deadline - now : 50)) == WAIT_OBJECT_0;
    }
    if (timedOut) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) CloseHandle(job);
    if (timedOut) { r.err = HvQueryError::Timeout; return r; }
    return parseHyperVQueryJson(out);
}

}  // namespace it2w
