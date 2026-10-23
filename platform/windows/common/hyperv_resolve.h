// T09 Hyper-V 地址解析（设计第 7、16、17 节；docs/T09-Hyper-V辅助-v0.1.md）。跨平台纯逻辑：
// VM ID 校验、查询结果 JSON 解析、地址筛选与选择、解析状态机（计时、启动一次、回退、歧义）。
// Windows 层（platform/windows/hyperv）只负责运行固定 PowerShell 脚本并把输出交给这里。
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace it2w {

struct HvAdapter { std::string id, name, mac; std::vector<std::string> ips; };
struct HvVm { std::string id, name, state; std::vector<HvAdapter> adapters; };

enum class HvQueryError {
    None,
    ModuleMissing,   // 没有 Hyper-V PowerShell 模块
    AccessDenied,    // 非 Hyper-V Administrators / 未提权
    NotFound,        // VM ID 不存在
    Failed,          // 脚本内其他异常（可重试）
    Timeout,         // 本地：单次查询超时（可重试）
    BadOutput,       // 本地：输出不是预期 JSON（可重试）
    LaunchFailed,    // 本地：powershell.exe 不存在或启动失败
};
struct HvQueryResult { HvQueryError err = HvQueryError::None; std::string msg; std::vector<HvVm> vms; };

// 8-4-4-4-12 十六进制，可带花括号；返回规范化小写形式，非法返回 nullopt
std::optional<std::string> normalizeVmId(std::string_view id);
// 解析固定脚本输出（单个 JSON 对象，见 hyperv_query.cpp）；前后空白与 BOM 忽略
HvQueryResult parseHyperVQueryJson(std::string_view text);
const char *hvQueryErrorName(HvQueryError);

// 默认排除：0.0.0.0、127/8、169.254/16、::、::1、无 %scope 的 fe80::/10；不排除私有网段
bool isUsableVmAddress(std::string_view ip);
bool isIpv6(std::string_view ip);

struct HvPolicy {
    std::string vmId;                       // 规范化后的 GUID
    std::optional<std::string> adapterId;   // 记住的网卡
    std::string addressFamily = "ipv4";     // ipv4 | ipv6 | any（ipv4/ipv6 是偏好，不是过滤）
    std::optional<std::string> fallbackHost;// 手动回退地址
    bool startIfOff = false;                // 用户已确认"启动并连接"
    uint32_t pollMs = 2000, queryTimeoutMs = 5000, budgetMs = 60000;
};

struct HvCandidate { std::string adapterId, adapterName, address; };
// 按网卡、可用性与地址族偏好得到候选（偏好族为空时退到另一族）
std::vector<HvCandidate> selectCandidates(const HvVm &, const HvPolicy &);

// 第 17 节稳定错误类别（Hyper-V 相关子集）
enum class HvFailure { None, VmNotFound, VmAccessDenied, VmAddressUnavailable, VmNotRunning, AddressAmbiguous, ExecutableNotFound };
const char *hvFailureName(HvFailure);

struct HvAction {
    enum Kind { Query, Start, Wait, Connect, Fail } kind = Query;
    uint32_t waitMs = 0;          // Wait：等待后再 Query
    std::string host;             // Connect
    bool fallback = false;        // Connect：使用的是手动回退地址
    HvFailure failure = HvFailure::None;  // Fail
    std::string text;             // Fail：给用户看的文本；Connect：诊断说明
    std::vector<HvCandidate> candidates;  // Fail(AddressAmbiguous)：供 UI 选择
};

// 状态机：begin() → 调用方执行 Query/Start/Wait 并把结果交回，直到 Connect 或 Fail。
// 时间由调用方传入（毫秒，单调），便于单测。
class HyperVResolver {
public:
    explicit HyperVResolver(HvPolicy p) : p_(std::move(p)) {}
    HvAction begin(uint64_t nowMs);
    HvAction onQueryResult(const HvQueryResult &, uint64_t nowMs);
    HvAction onStartResult(const HvQueryResult &, uint64_t nowMs);
    bool startSent() const { return startSent_; }
    const HvPolicy &policy() const { return p_; }
private:
    HvAction waitOr(uint64_t now, HvFailure f, std::string text);
    HvAction failOrFallback(HvFailure f, std::string text);
    HvPolicy p_;
    uint64_t deadline_ = 0;
    bool startSent_ = false;
};

// 驱动循环（平台无关，便于用假查询单测）：
// query/start 执行一次固定脚本（各自带单次超时）；sleep(ms) 返回 false 表示已取消；now 为单调毫秒。
// 返回最终 Connect / Fail；取消时返回 Fail 且 text 为空、failure=None。
struct HvDriver {
    std::function<HvQueryResult()> query;
    std::function<HvQueryResult()> start;
    std::function<bool(uint32_t)> sleep;
    std::function<uint64_t()> now;
    std::function<bool()> cancelled;
    std::function<void(const HvAction &)> onStep;   // 可选：每步诊断（Wait 文本等）
};
HvAction runHyperVResolution(HyperVResolver &, const HvDriver &);

}  // namespace it2w
