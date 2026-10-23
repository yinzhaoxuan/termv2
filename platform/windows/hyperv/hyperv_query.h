// T09：运行固定 PowerShell 脚本查询/启动本机 Hyper-V 虚拟机（docs/T09-Hyper-V辅助-v0.1.md §2.1）。
// VM ID 经 normalizeVmId 校验后由环境变量 IT2W_VMID 传入；脚本文本为常量，不拼接任何用户内容。
// 未实机验证：须在启用 Hyper-V 的 Win11 上覆盖 VM-01。
#pragma once
#ifndef _WIN32
#error "hyperv_query 仅用于 Windows"
#endif
#include "hyperv_resolve.h"
#include <windows.h>

namespace it2w {

// vmId 为空：列出全部 VM（UI 选择器）；start=true：先 Start-VM 再返回状态。
// cancelEvent 可为 nullptr；超时或取消时终止子进程（Job KILL_ON_JOB_CLOSE）并返回 Timeout。
HvQueryResult runHyperVScript(const std::string &vmId, bool start, DWORD timeoutMs, HANDLE cancelEvent);
inline HvQueryResult queryHyperVVm(const std::string &vmId, DWORD ms, HANDLE cancel) { return runHyperVScript(vmId, false, ms, cancel); }
inline HvQueryResult startHyperVVm(const std::string &vmId, DWORD ms, HANDLE cancel) { return runHyperVScript(vmId, true, ms, cancel); }
inline HvQueryResult listHyperVVms(DWORD ms, HANDLE cancel) { return runHyperVScript({}, false, ms, cancel); }

}  // namespace it2w
