#include "hyperv_resolve.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

namespace it2w {

std::optional<std::string> normalizeVmId(std::string_view id) {
    if (id.size() == 38 && id.front() == '{' && id.back() == '}') id = id.substr(1, 36);
    if (id.size() != 36) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < 36; ++i) {
        char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return std::nullopt; out += c; continue; }
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        out += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

const char *hvQueryErrorName(HvQueryError e) {
    switch (e) {
    case HvQueryError::None: return "None";
    case HvQueryError::ModuleMissing: return "ModuleMissing";
    case HvQueryError::AccessDenied: return "AccessDenied";
    case HvQueryError::NotFound: return "NotFound";
    case HvQueryError::Failed: return "Failed";
    case HvQueryError::Timeout: return "Timeout";
    case HvQueryError::BadOutput: return "BadOutput";
    case HvQueryError::LaunchFailed: return "LaunchFailed";
    }
    return "?";
}
const char *hvFailureName(HvFailure f) {
    switch (f) {
    case HvFailure::None: return "None";
    case HvFailure::VmNotFound: return "VmNotFound";
    case HvFailure::VmAccessDenied: return "VmAccessDenied";
    case HvFailure::VmAddressUnavailable: return "VmAddressUnavailable";
    case HvFailure::VmNotRunning: return "VmNotRunning";
    case HvFailure::AddressAmbiguous: return "AddressAmbiguous";
    case HvFailure::ExecutableNotFound: return "ExecutableNotFound";
    }
    return "?";
}

namespace {
std::string str(const nlohmann::json &j, const char *k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
// ConvertTo-Json 会把单元素数组压成标量，这里两种都接受
template <class F> void each(const nlohmann::json &j, const char *k, F f) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return;
    if (it->is_array()) { for (auto &e : *it) f(e); } else f(*it);
}
}  // namespace

HvQueryResult parseHyperVQueryJson(std::string_view text) {
    HvQueryResult r;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.remove_prefix(3);
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1) { r.err = HvQueryError::BadOutput; return r; }
    std::string err = str(j, "err");
    r.msg = str(j, "msg");
    if (!j.value("ok", false)) {
        r.err = err == "ModuleMissing" ? HvQueryError::ModuleMissing
              : err == "AccessDenied"  ? HvQueryError::AccessDenied
              : err == "NotFound"      ? HvQueryError::NotFound
                                       : HvQueryError::Failed;
        return r;
    }
    each(j, "vms", [&](const nlohmann::json &v) {
        if (!v.is_object()) return;
        HvVm vm{str(v, "id"), str(v, "name"), str(v, "state"), {}};
        if (auto n = normalizeVmId(vm.id)) vm.id = *n;
        each(v, "adapters", [&](const nlohmann::json &a) {
            if (!a.is_object()) return;
            HvAdapter ad{str(a, "id"), str(a, "name"), str(a, "mac"), {}};
            each(a, "ips", [&](const nlohmann::json &ip) { if (ip.is_string()) ad.ips.push_back(ip.get<std::string>()); });
            vm.adapters.push_back(std::move(ad));
        });
        r.vms.push_back(std::move(vm));
    });
    return r;
}

bool isIpv6(std::string_view ip) { return ip.find(':') != std::string_view::npos; }

bool isUsableVmAddress(std::string_view ip) {
    if (ip.empty()) return false;
    if (!isIpv6(ip)) {
        unsigned a[4]; int n = 0; size_t pos = 0;
        for (; n < 4; ++n) {
            size_t e = ip.find('.', pos);
            std::string_view part = ip.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
            if (part.empty() || part.size() > 3 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
            a[n] = unsigned(std::stoul(std::string(part)));
            if (a[n] > 255) return false;
            if (e == std::string_view::npos) { ++n; break; }
            pos = e + 1;
        }
        if (n != 4 || std::count(ip.begin(), ip.end(), '.') != 3) return false;
        if (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0) return false;
        if (a[0] == 127) return false;
        if (a[0] == 169 && a[1] == 254) return false;  // 无作用域的链路本地（APIPA）
        return true;
    }
    std::string s(ip);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    size_t pct = s.find('%');
    std::string addr = s.substr(0, pct);
    if (addr == "::" || addr == "::1") return false;
    for (char c : addr) if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.')) return false;
    // fe80::/10：首段为 4 位且 fe8x–febx
    size_t c1 = addr.find(':');
    bool linkLocal = c1 == 4 && addr[0] == 'f' && addr[1] == 'e' && (addr[2] == '8' || addr[2] == '9' || addr[2] == 'a' || addr[2] == 'b');
    if (linkLocal && (pct == std::string::npos || pct + 1 >= s.size())) return false;
    return true;
}

std::vector<HvCandidate> selectCandidates(const HvVm &vm, const HvPolicy &p) {
    std::vector<HvCandidate> v4, v6;
    for (auto &a : vm.adapters) {
        if (p.adapterId && a.id != *p.adapterId) continue;
        for (auto &ip : a.ips) {
            if (!isUsableVmAddress(ip)) continue;
            (isIpv6(ip) ? v6 : v4).push_back({a.id, a.name, ip});
        }
    }
    if (p.addressFamily == "ipv6") return v6.empty() ? v4 : v6;
    if (p.addressFamily == "any") { v4.insert(v4.end(), v6.begin(), v6.end()); return v4; }
    return v4.empty() ? v6 : v4;
}

HvAction HyperVResolver::begin(uint64_t now) {
    deadline_ = now + p_.budgetMs;
    startSent_ = false;
    return HvAction{};
}

HvAction HyperVResolver::failOrFallback(HvFailure f, std::string text) {
    HvAction a;
    if (p_.fallbackHost && !p_.fallbackHost->empty()) {
        a.kind = HvAction::Connect; a.host = *p_.fallbackHost; a.fallback = true;
        a.text = std::string(hvFailureName(f)) + "：" + text + "；改用手动地址";
        return a;
    }
    a.kind = HvAction::Fail; a.failure = f; a.text = std::move(text);
    return a;
}

HvAction HyperVResolver::waitOr(uint64_t now, HvFailure f, std::string text) {
    if (now + p_.pollMs <= deadline_) { HvAction a; a.kind = HvAction::Wait; a.waitMs = p_.pollMs; a.text = text; return a; }
    return failOrFallback(f, std::move(text));
}

HvAction HyperVResolver::onQueryResult(const HvQueryResult &r, uint64_t now) {
    switch (r.err) {
    case HvQueryError::ModuleMissing:
        return failOrFallback(HvFailure::VmAccessDenied, "Hyper-V 管理模块不可用（未启用 Hyper-V 或未安装管理工具），可改用手动地址");
    case HvQueryError::AccessDenied:
        return failOrFallback(HvFailure::VmAccessDenied, "没有查询 Hyper-V 的权限（需要 Hyper-V Administrators 组），普通 SSH 与手动地址不受影响");
    case HvQueryError::LaunchFailed:
        return failOrFallback(HvFailure::ExecutableNotFound, "无法启动 powershell.exe");
    case HvQueryError::NotFound: {
        HvAction a; a.kind = HvAction::Fail; a.failure = HvFailure::VmNotFound;
        a.text = "虚拟机已移除或标识变化，请重新选择";  // 不按同名回退，也不用手动地址冒充原主机
        return a;
    }
    case HvQueryError::Failed: case HvQueryError::Timeout: case HvQueryError::BadOutput:
        return waitOr(now, HvFailure::VmAddressUnavailable, std::string("Hyper-V 查询失败（") + hvQueryErrorName(r.err) + "）");
    case HvQueryError::None: break;
    }
    const HvVm *vm = nullptr;
    for (auto &v : r.vms) if (v.id == p_.vmId) { vm = &v; break; }
    if (!vm) { HvAction a; a.kind = HvAction::Fail; a.failure = HvFailure::VmNotFound; a.text = "虚拟机已移除或标识变化，请重新选择"; return a; }
    if (vm->state != "Running") {
        bool transitional = vm->state == "Starting" || vm->state == "Resuming" || vm->state == "Saving" || vm->state == "Stopping";
        if (transitional || startSent_) return waitOr(now, HvFailure::VmAddressUnavailable, "等待虚拟机运行（当前 " + vm->state + "）");
        if (p_.startIfOff) { startSent_ = true; HvAction a; a.kind = HvAction::Start; return a; }
        HvAction a; a.kind = HvAction::Fail; a.failure = HvFailure::VmNotRunning;
        a.text = "虚拟机未运行（状态 " + (vm->state.empty() ? std::string("未知") : vm->state) + "），可选择“启动并连接”";
        return a;
    }
    auto c = selectCandidates(*vm, p_);
    if (c.size() == 1) { HvAction a; a.kind = HvAction::Connect; a.host = c[0].address; a.text = "网卡 " + c[0].adapterName; return a; }
    if (c.size() > 1) {
        HvAction a; a.kind = HvAction::Fail; a.failure = HvFailure::AddressAmbiguous;
        a.text = "有多个候选地址，请选择网卡：";
        for (size_t i = 0; i < c.size(); ++i) a.text += (i ? "，" : "") + c[i].adapterName + " " + c[i].address;
        a.candidates = std::move(c);
        return a;
    }
    return waitOr(now, HvFailure::VmAddressUnavailable, "等待地址超时，请检查来宾集成服务，或改用手动地址");
}

HvAction HyperVResolver::onStartResult(const HvQueryResult &r, uint64_t now) {
    if (r.err == HvQueryError::AccessDenied)
        { HvAction a; a.kind = HvAction::Fail; a.failure = HvFailure::VmAccessDenied; a.text = "没有启动虚拟机的权限（需要 Hyper-V Administrators 组）"; return a; }
    if (r.err == HvQueryError::NotFound || r.err == HvQueryError::ModuleMissing || r.err == HvQueryError::LaunchFailed)
        return onQueryResult(r, now);
    // 启动请求已发出（成功只表示请求完成）；失败或超时也不再重发，继续轮询状态
    return waitOr(now, HvFailure::VmAddressUnavailable, "已请求启动，等待虚拟机运行与地址");
}

HvAction runHyperVResolution(HyperVResolver &r, const HvDriver &d) {
    auto cancel = [] { HvAction a; a.kind = HvAction::Fail; return a; };
    HvAction a = r.begin(d.now());
    for (int guard = 0; guard < 10000; ++guard) {
        if (d.cancelled && d.cancelled()) return cancel();
        switch (a.kind) {
        case HvAction::Query: a = r.onQueryResult(d.query(), d.now()); break;
        case HvAction::Start: a = r.onStartResult(d.start(), d.now()); break;
        case HvAction::Wait:
            if (!d.sleep(a.waitMs)) return cancel();
            a = HvAction{};
            continue;
        case HvAction::Connect: case HvAction::Fail: return a;
        }
        if (d.onStep) d.onStep(a);
    }
    HvAction f; f.kind = HvAction::Fail; f.failure = HvFailure::VmAddressUnavailable; f.text = "解析未收敛";
    return f;
}

}  // namespace it2w
