// T08：uishell::PaneBackend 的 Win32 实现（itcore + ConPtyTransport/SshTransport + D2DRenderer）。仅 WIN32。
// 未实机验证：子窗口/交换链、IME、焦点与键盘转发、Job 进程计数、关闭时序均待 DESKTOP-HOODQAJ。
#pragma once
#ifndef _WIN32
#error "win32_pane_backend 仅用于 Windows"
#endif
#include "uishell/pane_backend.h"
#include "ssh_transport.h"
#include "hyperv_transport.h"
#include <optional>
#include <string>

namespace it2app {

struct PaneBackendConfig {
    // connectionId → SSH 启动参数（由 UI 从 uicfg Connection 构造）。返回 nullopt 时 *error 给出原因，
    // 窗格进入 Failed。接口签名里 factory 只有 connectionId，所以解析器在构造工厂时注入。
    std::function<std::optional<it2w::SshLaunchOptions>(const std::string &connectionId, std::string *error)> resolveSsh;
    // T09：connectionId → Hyper-V 启动参数（endpoint.mode=hyperv）。返回 nullopt 表示不是 Hyper-V 连接，
    // 再走 resolveSsh。policy.vmId 须为 GUID；ssh.target.hostName 留空，由后台解析填入。
    std::function<std::optional<it2w::HyperVLaunch>(const std::string &connectionId, std::string *error)> resolveHyperV;
    std::wstring localShell;            // 空：%ComSpec%，再退回 System32\cmd.exe
    std::wstring fontFamily = L"Consolas";
    float fontSizePt = 12.f;
    uint32_t scrollbackLines = 5000;
    uint64_t memoryLimitBytes = 32ull << 20;
};

uishell::PaneBackendFactory makeWin32PaneBackendFactory(PaneBackendConfig cfg);
// 程序退出时（所有窗格销毁之后、WinMain 返回之前）在 UI 线程调用：等待所有会话关闭线程结束。
// 超时返回 false，剩余线程保持 detach，子进程由 Job 对象 KILL_ON_JOB_CLOSE 随进程退出回收。
bool shutdownPaneBackends(unsigned timeoutMs = 3000);

}  // namespace it2app
