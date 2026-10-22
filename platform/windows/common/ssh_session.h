// T07 SSH 会话：跨平台纯逻辑（ssh -G 解析、退出码/错误映射、输出尾部缓存）。
// Windows 层 SshTransport 只负责落盘快照与在 ConPTY 中启动 ssh -F <快照> <alias>。
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace it2w {

// `ssh -G` 输出：键统一小写；同键多值（identityfile 等）按出现顺序保存
using SshEffectiveConfig = std::map<std::string, std::vector<std::string>>;
SshEffectiveConfig parseSshG(std::string_view text);
// 便捷取首值；不存在返回空串
std::string sshGFirst(const SshEffectiveConfig &, const std::string &key);

// 校验快照在 -G 下的生效值符合规格（SSH-CFG-01 A1–A4 子集）；返回不符合项描述，空表示通过
std::vector<std::string> checkEffectiveConfig(const SshEffectiveConfig &, bool interactive);

enum class SshError {
    None,               // 远端 shell 正常退出（退出码 0）
    RemoteExit,         // 远端命令非 0 退出（1..254），不算连接错误
    AuthFailed,         // Permission denied (...)
    HostKeyUnknown,     // Host key verification failed（未知主机且未确认）
    HostKeyChanged,     // REMOTE HOST IDENTIFICATION HAS CHANGED
    ConnectionRefused,
    Timeout,            // Connection timed out / Operation timed out
    HostNotFound,       // Could not resolve hostname
    NetworkUnreachable,
    ClientIncompatible, // Bad configuration option / unsupported option（旧版 Win32-OpenSSH）
    ConfigError,        // 快照不可读、其他 -F 配置错误
    ConnectionClosed,   // Connection closed/reset by remote 等
    SshNotFound,        // 启动 ssh.exe 失败
    Unknown             // 255 但无法识别
};

struct SshExitInfo {
    SshError error = SshError::Unknown;
    uint32_t exitCode = 0;
    std::string detail;   // 命中的 ssh 诊断行（已去掉控制字符），供 UI 显示
};

// ssh 约定：255 表示 ssh 自身错误，其他为远端退出码。ConPTY 下 stderr 与 stdout 合流，
// 因此依据输出尾部识别错误。
SshExitInfo mapSshExit(uint32_t exitCode, std::string_view outputTail);
const char *sshErrorName(SshError);

// 保留最近 N 字节输出（错误识别用）；去掉 CSI/OSC 转义，便于匹配
class OutputTail {
public:
    explicit OutputTail(size_t cap = 8192) : cap_(cap) {}
    void append(std::string_view bytes);
    std::string text() const;  // 去转义后的文本
private:
    size_t cap_;
    std::string buf_;
};
std::string stripTerminalEscapes(std::string_view s);

}  // namespace it2w
