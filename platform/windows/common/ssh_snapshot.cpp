#include "ssh_snapshot.h"
#include <algorithm>
#include <cctype>

namespace it2w {
namespace {
bool has_ctrl(const std::string &s) {
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return true;
    return false;
}
bool valid_alias(const std::string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return s.find_first_of("*?!") == std::string::npos;
}
bool valid_host(const std::string &s) {
    if (s.empty() || s.size() > 253) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '_' || c >= 0x80)) return false;
    return s[0] != '-';
}
bool valid_user(const std::string &s) {
    if (s.empty() || s.size() > 128 || s[0] == '-' || has_ctrl(s)) return false;
    return s.find_first_of(" \t\"'%\\#=,") == std::string::npos;
}
char lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
bool iequal(const std::string &a, const std::string &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}
}  // namespace

std::optional<std::string> quoteConfigPath(const std::string &path, std::string *reason) {
    auto fail = [&](const char *r) { if (reason) *reason = r; return std::optional<std::string>(); };
    if (path.empty()) return fail("空路径");
    if (has_ctrl(path)) return fail("含控制字符");
    if (path.find('"') != std::string::npos) return fail("含双引号");
    if (path.find('%') != std::string::npos) return fail("含 %（ssh token 展开）");
    if (path[0] == '~') return fail("以 ~ 开头（ssh 家目录展开）");
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    bool drive = p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && p[2] == '/';
    bool unc = p.size() >= 3 && p[0] == '/' && p[1] == '/' && p[2] != '/';
    if (!drive && !unc) return fail("不是绝对路径");
    return "\"" + p + "\"";
}

std::optional<SnapshotError> validateSshTarget(const SshTarget &t) {
    auto err = [](const char *f, std::string why) { return SnapshotError{f, std::move(why)}; };
    if (!valid_alias(t.alias)) return err("alias", "只允许字母数字 . _ -");
    if (!valid_host(t.hostName)) return err("hostName", "非法主机名");
    if (t.port == 0) return err("port", "端口为 0");
    if (!valid_user(t.user)) return err("user", "非法用户名");
    if (t.connectTimeoutSec == 0 || t.connectTimeoutSec > 600) return err("connectTimeoutSec", "超出 1..600");
    std::string why;
    auto known = quoteConfigPath(t.appKnownHostsFile, &why);
    if (!known) return err("appKnownHostsFile", why);
    auto empty = quoteConfigPath(t.emptyKnownHostsFile, &why);
    if (!empty) return err("emptyKnownHostsFile", why);
    if (t.auth == SshAuthMode::KeyFile && !quoteConfigPath(t.keyFile, &why))
        return err("keyFile", why);
    return std::nullopt;
}

SnapshotResult buildSshSnapshot(const SshTarget &t) {
    SnapshotResult r;
    r.error = validateSshTarget(t);
    if (r.error) return r;
    auto known = quoteConfigPath(t.appKnownHostsFile);
    auto empty = quoteConfigPath(t.emptyKnownHostsFile);

    std::string s;
    auto kv = [&](const char *k, const std::string &v) { s += "  "; s += k; s += ' '; s += v; s += '\n'; };
    s += "# iTerm2-Windows 生成的 SSH 配置快照；每个连接尝试独立一份，结束后删除\n";
    s += "Host " + t.alias + "\n";
    kv("HostName", t.hostName);
    kv("Port", std::to_string(t.port));
    kv("User", t.user);
    kv("StrictHostKeyChecking", "ask");
    kv("UserKnownHostsFile", *known);
    kv("GlobalKnownHostsFile", *empty);
    kv("VerifyHostKeyDNS", "no");
    kv("UpdateHostKeys", "no");
    kv("RequestTTY", "force");
    kv("ConnectTimeout", std::to_string(t.connectTimeoutSec));
    kv("BatchMode", "no");
    kv("ForwardAgent", "no");
    kv("ForwardX11", "no");
    kv("PermitLocalCommand", "no");
    kv("ClearAllForwardings", "yes");
    switch (t.auth) {
    case SshAuthMode::Agent:
        kv("PreferredAuthentications", "publickey");
        kv("PubkeyAuthentication", "yes");
        kv("PasswordAuthentication", "no");
        kv("KbdInteractiveAuthentication", "no");
        kv("IdentityFile", "none");
        kv("IdentitiesOnly", "no");
        break;
    case SshAuthMode::KeyFile: {
        auto key = quoteConfigPath(t.keyFile);
        kv("PreferredAuthentications", "publickey");
        kv("PubkeyAuthentication", "yes");
        kv("PasswordAuthentication", "no");
        kv("KbdInteractiveAuthentication", "no");
        kv("IdentityFile", *key);
        kv("IdentitiesOnly", "yes");
        kv("IdentityAgent", "none");
        break;
    }
    case SshAuthMode::Interactive:
        kv("PreferredAuthentications", "keyboard-interactive,password");
        kv("PubkeyAuthentication", "no");
        kv("PasswordAuthentication", "yes");
        kv("KbdInteractiveAuthentication", "yes");
        kv("IdentityFile", "none");      // SSH-CFG-01 A4：不加载 ~/.ssh/id_*
        kv("IdentitiesOnly", "yes");
        kv("IdentityAgent", "none");
        break;
    }
    // 三种模式共用：显式排除证书与连接复用（A4 / 纵深防御）
    kv("CertificateFile", "none");
    kv("ControlMaster", "no");
    kv("ControlPath", "none");
    r.text = std::move(s);
    return r;
}

std::string quoteWindowsArg(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    std::size_t bs = 0;
    for (char c : arg) {
        if (c == '\\') { ++bs; continue; }
        if (c == '"') { out.append(bs * 2 + 1, '\\'); out += '"'; }
        else { out.append(bs, '\\'); out += c; }
        bs = 0;
    }
    out.append(bs * 2, '\\');
    out += '"';
    return out;
}

std::string buildWindowsCommandLine(const std::vector<std::string> &argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out += ' ';
        // argv[0] 按 CreateProcess 规则：总是加引号且不得含引号（调用方保证为已校验的绝对路径）
        out += i == 0 ? "\"" + argv[0] + "\"" : quoteWindowsArg(argv[i]);
    }
    return out;
}

std::vector<std::string> parseWindowsCommandLine(const std::string &cmd) {
    std::vector<std::string> out;
    std::size_t i = 0, n = cmd.size();
    while (true) {
        while (i < n && (cmd[i] == ' ' || cmd[i] == '\t')) ++i;
        if (i >= n) break;
        std::string cur; bool inq = false;
        while (i < n) {
            char c = cmd[i];
            if (!inq && (c == ' ' || c == '\t')) break;
            if (c == '\\') {
                std::size_t bs = 0;
                while (i < n && cmd[i] == '\\') { ++bs; ++i; }
                if (i < n && cmd[i] == '"') {
                    cur.append(bs / 2, '\\');
                    if (bs % 2) { cur += '"'; ++i; }
                } else cur.append(bs, '\\');
                continue;
            }
            if (c == '"') {
                if (inq && i + 1 < n && cmd[i + 1] == '"') { cur += '"'; i += 2; continue; }
                inq = !inq; ++i; continue;
            }
            cur += c; ++i;
        }
        out.push_back(std::move(cur));
    }
    return out;
}

std::vector<std::string> sshArguments(const std::string &sshExe, const std::string &snapshotPath,
                                      const std::string &alias) {
    return {sshExe, "-F", snapshotPath, alias};
}

EnvList filterChildEnvironment(const EnvList &in) {
    EnvList out;
    for (auto &kv : in) {
        if (iequal(kv.first, "SSH_ASKPASS") || iequal(kv.first, "SSH_ASKPASS_REQUIRE")) continue;
        if (kv.first.empty() || kv.first.find('=') != std::string::npos ||
            kv.first.find('\0') != std::string::npos || kv.second.find('\0') != std::string::npos)
            continue;
        out.push_back(kv);
    }
    std::stable_sort(out.begin(), out.end(), [](auto &a, auto &b) {
        return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                                            [](char x, char y) { return std::toupper((unsigned char)x) < std::toupper((unsigned char)y); });
    });
    return out;
}

}  // namespace it2w
