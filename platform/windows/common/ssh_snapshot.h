// SSH 配置快照生成（设计 13.4，T03A 规格 §3）与 Windows 进程参数/环境规则（13.5）。
// 跨平台纯逻辑，便于在 Linux 上单测；Windows 层只负责 UTF-8↔UTF-16 转换与落盘。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace it2w {

enum class SshAuthMode { Agent, KeyFile, Interactive };

struct SshTarget {
    std::string alias = "it2w-target";  // 快照内的 Host 别名，只允许 [A-Za-z0-9._-]
    std::string hostName;               // 主机名 / IPv4 / IPv6（不带方括号与端口）
    uint16_t port = 22;
    std::string user;
    uint32_t connectTimeoutSec = 10;
    SshAuthMode auth = SshAuthMode::Agent;
    std::string keyFile;                // KeyFile 模式必填，绝对路径
    std::string appKnownHostsFile;      // 应用拥有的 known_hosts
    std::string emptyKnownHostsFile;    // 应用拥有的空文件，用作 GlobalKnownHostsFile
};

struct SnapshotError { std::string field, reason; };

// 与快照生成共用的字段校验；保留原有校验顺序与错误文本。
std::optional<SnapshotError> validateSshTarget(const SshTarget &t);

// 生成快照文本（UTF-8 无 BOM，LF 换行）。任何字段不合法时返回错误，不静默省略。
// 永不输出 ProxyCommand / ProxyJump / LocalCommand / RemoteCommand；ControlMaster 恒为 no、ControlPath none、CertificateFile none。
struct SnapshotResult { std::optional<std::string> text; std::optional<SnapshotError> error; };
SnapshotResult buildSshSnapshot(const SshTarget &t);

// 路径规则：拒绝 `"`、`%`、控制字符、前导 `~`，必须为绝对路径（X:/ 或 //server/）；
// 反斜线转正斜线；返回加双引号的形式。失败返回 nullopt。
std::optional<std::string> quoteConfigPath(const std::string &path, std::string *reason = nullptr);

// 按 CommandLineToArgvW 规则编码单个参数 / 整个命令行（argv[0] 为可执行文件路径）。
std::string quoteWindowsArg(const std::string &arg);
std::string buildWindowsCommandLine(const std::vector<std::string> &argv);
// CommandLineToArgvW 的参考解析（仅用于测试往返；不处理 argv[0] 的特殊规则）。
std::vector<std::string> parseWindowsCommandLine(const std::string &cmd);

// ssh 参数：ssh.exe -F <snapshot> <alias>（不经 shell 拼接）
std::vector<std::string> sshArguments(const std::string &sshExe, const std::string &snapshotPath,
                                      const std::string &alias);

// 环境：name=value 列表；移除 SSH_ASKPASS、SSH_ASKPASS_REQUIRE（Windows 下名称不区分大小写），
// 然后按名称（不区分大小写）排序，满足 CreateProcess 环境块要求。
using EnvList = std::vector<std::pair<std::string, std::string>>;
EnvList filterChildEnvironment(const EnvList &in);

}  // namespace it2w
