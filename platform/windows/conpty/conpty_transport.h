// SessionTransport 的 ConPTY 实现（T03A 规格 §1、§2）。仅 Windows 编译。
// 未实机验证：所有 ConPTY / Job / 管道 / 关闭时序行为均待 T03A 在 Win11 上实测。
#pragma once
#ifndef _WIN32
#error "conpty_transport 仅用于 Windows"
#endif
#include "io_queues.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace it2w {

enum class TransportState { Idle, Starting, Running, Closing, Closed, Failed };
enum class TransportFault { StartFailed, JobAssignFailed, PipeBroken, InputStalled, ReplyBackpressure,
                            OutputLimit, ResizeFailed, CloseTimeout };
enum class CloseReason { User, ProcessExited, Fault };
struct AttemptId { uint64_t value; };
struct SessionId { uint64_t value; };
struct GridSize { uint16_t cols = 80, rows = 24; };

struct LaunchSpec {
    std::wstring executable;                 // 绝对路径
    std::vector<std::wstring> arguments;     // 不含 argv[0]
    std::optional<std::wstring> workingDirectory;
    std::vector<std::pair<std::wstring, std::wstring>> environment;  // 已过滤、已排序；空表示继承
    GridSize initialSize;                    // 钳到 ≥ 20×5
    std::optional<std::wstring> sshSnapshotFile;  // attempt 结束时删除
};

struct DiagRecord { std::string stage; uint64_t elapsedMs = 0; uint64_t bytes = 0; std::string detail; };

struct TransportCallbacks {  // 均从传输内部线程调用；接收方须比对 attempt
    std::function<void(SessionId, AttemptId, TransportState, std::optional<TransportFault>)> onStateChanged;
    std::function<void(SessionId, AttemptId)> onOutputAvailable;
    std::function<void(SessionId, AttemptId, uint32_t)> onExited;
    std::function<void(SessionId, AttemptId, const DiagRecord &)> onDiagnostic;
    // 可选：读线程在入队前把原始输出给出（不依赖 UI 取输出；SshTransport 用它记错误尾部）。须快速返回
    std::function<void(SessionId, AttemptId, std::string_view)> onOutputTap;
};

struct OutputChunk { std::string bytes; bool incompleteAfterThis = false; };

class ConPtyTransport {
public:
    explicit ConPtyTransport(TransportCallbacks cb);
    ~ConPtyTransport();  // 隐式 close 并等待清理完成
    ConPtyTransport(const ConPtyTransport &) = delete;
    ConPtyTransport &operator=(const ConPtyTransport &) = delete;

    bool start(SessionId, AttemptId, const LaunchSpec &);  // 同步执行 §1.2；结果亦经 onStateChanged
    EnqueueResult enqueueUserInput(std::string_view bytes);
    EnqueueResult enqueueReply(std::string_view bytes);
    EnqueueResult beginPaste(std::size_t totalBytes, bool bracketed = true);  // bracketed=false：DECSET 2004 关闭时不加标记
    EnqueueResult appendPaste(std::string_view body);
    void cancelPasteBody();
    void resize(GridSize, uint64_t generation);
    void close(CloseReason);
    std::optional<OutputChunk> tryPopOutput();

    TransportState state() const { return state_.load(); }
    bool waitClosed(DWORD ms);  // 测试/探针用
    uint32_t exitCode() const { return exitCode_; }
    uint64_t discardedBytes();
    // Job 内活动进程数（JobObjectBasicAccountingInformation）；非 Running 返回 0。T08 hasForegroundJob 用
    uint32_t activeProcessCount();

private:
    static DWORD WINAPI readerMain(LPVOID);
    static DWORD WINAPI writerMain(LPVOID);
    static DWORD WINAPI monitorMain(LPVOID);
    static DWORD WINAPI closerMain(LPVOID);
    void readerLoop();
    void writerLoop();
    void monitorLoop();
    void doClose();
    void setState(TransportState, std::optional<TransportFault> = std::nullopt);
    void diag(const char *stage, uint64_t ms, uint64_t bytes = 0, std::string detail = {});
    void failStart(TransportFault, const char *stage, DWORD err);
    void closePseudoConsoleOnce();

    TransportCallbacks cb_;
    SessionId sid_{0};
    AttemptId aid_{0};
    std::atomic<TransportState> state_{TransportState::Idle};
    std::optional<TransportFault> fault_;
    std::atomic<bool> closeRequested_{false}, closeStarted_{false}, writerStop_{false};
    CloseReason closeReason_ = CloseReason::User;

    SRWLOCK lock_ = SRWLOCK_INIT;       // 保护队列
    SRWLOCK pcLock_ = SRWLOCK_INIT;     // 保护 hpc_ 有效性（resize 与 close 串行），不在持锁时等待 I/O
    InputScheduler input_;
    OutputQueue output_;

    HANDLE inWrite_ = nullptr, outRead_ = nullptr;
    HPCON hpc_ = nullptr;
    std::atomic<bool> pcClosed_{false};
    HANDLE job_ = nullptr, process_ = nullptr;
    HANDLE reader_ = nullptr, writer_ = nullptr, monitor_ = nullptr, closer_ = nullptr;
    HANDLE writerWake_ = nullptr, readerResume_ = nullptr, monitorStop_ = nullptr, closedEvent_ = nullptr;
    std::optional<std::wstring> snapshot_;
    uint32_t exitCode_ = 0;
    int resizeFailures_ = 0;
    uint64_t closeStartMs_ = 0;
};

// UTF-8 ↔ UTF-16
std::wstring widen(const std::string &);
std::string narrow(const std::wstring &);
// 构造 CreateProcessW 需要的命令行与环境块（CREATE_UNICODE_ENVIRONMENT）
std::wstring buildCommandLineW(const LaunchSpec &);
std::vector<wchar_t> buildEnvironmentBlock(const LaunchSpec &);

}  // namespace it2w
