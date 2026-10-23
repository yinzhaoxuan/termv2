// HV-01（T09 平台无关逻辑）：VM ID 校验、查询 JSON 解析、地址筛选/选择、解析状态机（VM-01 场景的逻辑部分）。
#include "hyperv_resolve.h"
#include <cstdio>
#include <deque>
using namespace it2w;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static const char *kId = "0f3a1b2c-0000-4000-8000-00000000abcd";
static std::string vmJson(const char *state, const char *adapters, const char *id = kId) {
    return std::string("{\"v\":1,\"ok\":true,\"vms\":[{\"id\":\"") + id + "\",\"name\":\"ubuntu\",\"state\":\"" + state +
           "\",\"adapters\":" + adapters + "}]}";
}

// 假驱动：按脚本依次返回查询结果，时间由 sleep 推进
struct Fake {
    std::deque<HvQueryResult> q;
    int queries = 0, starts = 0;
    uint64_t t = 0;
    bool cancelAt = false; int cancelAfterSleeps = -1, sleeps = 0;
    HvDriver d() {
        HvDriver x;
        x.query = [this] { ++queries; t += 100; if (q.empty()) return parseHyperVQueryJson(vmJson("Running", "[]")); auto r = q.front(); if (q.size() > 1) q.pop_front(); return r; };
        x.start = [this] { ++starts; t += 500; return parseHyperVQueryJson(vmJson("Starting", "[]")); };
        x.sleep = [this](uint32_t ms) { t += ms; return cancelAfterSleeps < 0 || ++sleeps <= cancelAfterSleeps; };
        x.now = [this] { return t; };
        return x;
    }
};

static void ids() {
    CHECK(normalizeVmId("0F3A1B2C-0000-4000-8000-00000000ABCD") == std::string(kId));
    CHECK(normalizeVmId("{0f3a1b2c-0000-4000-8000-00000000abcd}") == std::string(kId));
    CHECK(!normalizeVmId("0f3a1b2c-0000-4000-8000-00000000abc"));
    CHECK(!normalizeVmId("0f3a1b2c-0000-4000-8000-00000000abcg"));
    CHECK(!normalizeVmId("0f3a1b2c00000-4000-8000-00000000abcd"));
    CHECK(!normalizeVmId("';Stop-VM *;'"));          // 注入内容直接被拒
    CHECK(!normalizeVmId(""));
}

static void parse() {
    auto r = parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"mac\":\"00155D\",\"ips\":[\"172.20.1.5\",\"fe80::1\"]}]"));
    CHECK(r.err == HvQueryError::None && r.vms.size() == 1 && r.vms[0].adapters.size() == 1 && r.vms[0].adapters[0].ips.size() == 2);
    // ConvertTo-Json 单元素压成标量
    r = parseHyperVQueryJson(std::string("\xEF\xBB\xBF") + "{\"v\":1,\"ok\":true,\"vms\":{\"id\":\"0F3A1B2C-0000-4000-8000-00000000ABCD\",\"name\":\"x\",\"state\":\"Off\",\"adapters\":{\"id\":\"a\",\"name\":\"n\",\"ips\":\"10.0.0.2\"}}}");
    CHECK(r.vms.size() == 1 && r.vms[0].id == kId && r.vms[0].adapters.size() == 1 && r.vms[0].adapters[0].ips == std::vector<std::string>{"10.0.0.2"});
    CHECK(parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"ModuleMissing\"}").err == HvQueryError::ModuleMissing);
    CHECK(parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"AccessDenied\",\"msg\":\"x\"}").err == HvQueryError::AccessDenied);
    CHECK(parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"NotFound\"}").err == HvQueryError::NotFound);
    CHECK(parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"Weird\"}").err == HvQueryError::Failed);
    CHECK(parseHyperVQueryJson("garbage").err == HvQueryError::BadOutput);
    CHECK(parseHyperVQueryJson("{\"v\":2,\"ok\":true}").err == HvQueryError::BadOutput);
    CHECK(parseHyperVQueryJson("").err == HvQueryError::BadOutput);
}

static void addresses() {
    for (const char *ok : {"172.20.1.5", "192.168.1.10", "10.0.0.1", "2001:db8::5", "fe80::1%eth0", "fd00::1"}) CHECK(isUsableVmAddress(ok));
    for (const char *bad : {"", "0.0.0.0", "127.0.0.1", "127.1.2.3", "169.254.3.4", "::", "::1", "fe80::1", "FE80::abcd", "febf::1",
                            "1.2.3", "1.2.3.4.5", "256.1.1.1", "a.b.c.d", "fe80::1%"}) CHECK(!isUsableVmAddress(bad));
    CHECK(isUsableVmAddress("fec0::1"));   // 不属于 fe80::/10
    HvVm vm{kId, "u", "Running", {{"a1", "eth0", "", {"172.20.1.5", "2001:db8::5", "fe80::9"}}, {"a2", "eth1", "", {"169.254.1.1"}}}};
    HvPolicy p; p.vmId = kId;
    auto c = selectCandidates(vm, p);
    CHECK(c.size() == 1 && c[0].address == "172.20.1.5");
    p.addressFamily = "ipv6"; c = selectCandidates(vm, p);
    CHECK(c.size() == 1 && c[0].address == "2001:db8::5");
    p.addressFamily = "any"; CHECK(selectCandidates(vm, p).size() == 2);
    p.addressFamily = "ipv4"; p.adapterId = "a2"; CHECK(selectCandidates(vm, p).empty());
    // 偏好族为空时退到另一族
    HvVm v6only{kId, "u", "Running", {{"a1", "eth0", "", {"2001:db8::7"}}}};
    p.adapterId.reset(); c = selectCandidates(v6only, p);
    CHECK(c.size() == 1 && c[0].address == "2001:db8::7");
}

static HvPolicy pol() { HvPolicy p; p.vmId = kId; return p; }

static void machine() {
    // 运行中、单地址 → Connect
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"172.20.1.5\"]}]"))};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Connect && a.host == "172.20.1.5" && !a.fallback && f.starts == 0); }
    // 多网卡各一个地址 → AddressAmbiguous，不盲选
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"172.20.1.5\"]},{\"id\":\"a2\",\"name\":\"eth1\",\"ips\":[\"192.168.7.2\"]}]"))};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::AddressAmbiguous && a.candidates.size() == 2);
      // 记住网卡后唯一
      HvPolicy p = pol(); p.adapterId = "a2"; Fake g; g.q = f.q; HyperVResolver r2(p); auto b = runHyperVResolution(r2, g.d());
      CHECK(b.kind == HvAction::Connect && b.host == "192.168.7.2"); }
    // 未运行且不允许启动 → VmNotRunning，不发启动
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Off", "[]"))};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::VmNotRunning && f.starts == 0); }
    // 允许启动：只发一次启动，等待运行与地址后连接
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Off", "[]")), parseHyperVQueryJson(vmJson("Off", "[]")),
                     parseHyperVQueryJson(vmJson("Starting", "[]")), parseHyperVQueryJson(vmJson("Running", "[]")),
                     parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"172.20.1.9\"]}]"))};
      HvPolicy p = pol(); p.startIfOff = true; HyperVResolver r(p); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Connect && a.host == "172.20.1.9" && f.starts == 1 && r.startSent()); }
    // 运行但一直无地址 → 60 s 预算后 VmAddressUnavailable；轮询间隔 2 s
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"fe80::5\"]}]"))};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::VmAddressUnavailable);
      CHECK(f.t >= 58000 && f.t <= 62000 + 31 * 100); CHECK(f.queries >= 25 && f.queries <= 31); }
    // 无地址 + 手动回退
    { Fake f; HvPolicy p = pol(); p.fallbackHost = "ubuntu.lan"; HyperVResolver r(p); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Connect && a.host == "ubuntu.lan" && a.fallback); }
    // VM 不存在（包括同名不同 ID 的 VM）→ VmNotFound；即使有回退地址也不连
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"172.20.1.5\"]}]", "11111111-2222-3333-4444-555555555555"))};
      HvPolicy p = pol(); p.fallbackHost = "x"; HyperVResolver r(p); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::VmNotFound); }
    { Fake f; f.q = {parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"NotFound\"}")};
      HyperVResolver r(pol()); CHECK(runHyperVResolution(r, f.d()).failure == HvFailure::VmNotFound); }
    // 无权限 / 模块缺失 → VmAccessDenied（有回退则回退）
    { Fake f; f.q = {parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"AccessDenied\"}")};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::VmAccessDenied && f.queries == 1); }
    { Fake f; f.q = {parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"ModuleMissing\"}")};
      HvPolicy p = pol(); p.fallbackHost = "10.1.1.1"; HyperVResolver r(p); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Connect && a.fallback && a.host == "10.1.1.1"); }
    { Fake f; HvQueryResult lf; lf.err = HvQueryError::LaunchFailed; f.q = {lf};
      HyperVResolver r(pol()); CHECK(runHyperVResolution(r, f.d()).failure == HvFailure::ExecutableNotFound); }
    // 单次超时可重试，随后成功
    { Fake f; HvQueryResult to; to.err = HvQueryError::Timeout;
      f.q = {to, to, parseHyperVQueryJson(vmJson("Running", "[{\"id\":\"a1\",\"name\":\"eth0\",\"ips\":[\"172.20.1.5\"]}]"))};
      HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Connect && f.queries == 3); }
    // 启动无权限
    { Fake f; f.q = {parseHyperVQueryJson(vmJson("Off", "[]"))}; HvDriver d = f.d();
      d.start = [] { return parseHyperVQueryJson("{\"v\":1,\"ok\":false,\"err\":\"AccessDenied\"}"); };
      HvPolicy p = pol(); p.startIfOff = true; HyperVResolver r(p); auto a = runHyperVResolution(r, d);
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::VmAccessDenied); }
    // 取消：返回 Fail/None，不再查询
    { Fake f; f.cancelAfterSleeps = 2; HyperVResolver r(pol()); auto a = runHyperVResolution(r, f.d());
      CHECK(a.kind == HvAction::Fail && a.failure == HvFailure::None && f.queries == 3); }
}

int main() {
    ids(); parse(); addresses(); machine();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("HV-01 ok");
    return 0;
}
