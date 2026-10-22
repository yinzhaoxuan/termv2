#include "ssh_session.h"
#include <algorithm>
#include <cctype>

namespace it2w {

static std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

SshEffectiveConfig parseSshG(std::string_view text) {
    SshEffectiveConfig m;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;
        size_t sp = line.find(' ');
        std::string k = lower(std::string(line.substr(0, sp)));
        std::string v = sp == std::string_view::npos ? std::string() : std::string(line.substr(sp + 1));
        m[k].push_back(std::move(v));
    }
    return m;
}

std::string sshGFirst(const SshEffectiveConfig &m, const std::string &key) {
    auto it = m.find(lower(key));
    return it == m.end() || it->second.empty() ? std::string() : it->second.front();
}

std::vector<std::string> checkEffectiveConfig(const SshEffectiveConfig &m, bool interactive) {
    std::vector<std::string> bad;
    auto expect = [&](const char *k, const char *v) {
        auto got = lower(sshGFirst(m, k));
        std::string want = v;
        // -G 的写法因版本而异：布尔值输出 yes/no 或 true/false；none 值可能整行省略
        auto norm = [](std::string x) { return x == "true" ? std::string("yes") : x == "false" ? std::string("no") : x; };
        if (want == "none" && got.empty()) return;
        if (norm(got) != norm(want)) bad.push_back(std::string(k) + "=" + got + "（期望 " + v + "）");
    };
    expect("forwardagent", "no");
    expect("forwardx11", "no");
    expect("permitlocalcommand", "no");
    expect("clearallforwardings", "yes");
    expect("controlmaster", "false");
    expect("controlpath", "none");
    expect("proxycommand", "");
    expect("proxyjump", "");
    expect("batchmode", "no");
    if (interactive) {
        expect("pubkeyauthentication", "no");
        expect("identitiesonly", "yes");
        expect("identityagent", "none");
    }
    auto it = m.find("identityfile");
    if (interactive && it != m.end())
        for (auto &v : it->second) if (lower(v) != "none") bad.push_back("identityfile=" + v + "（interactive 不应加载身份）");
    it = m.find("certificatefile");
    if (it != m.end())
        for (auto &v : it->second) if (lower(v) != "none") bad.push_back("certificatefile=" + v);
    return bad;
}

std::string stripTerminalEscapes(std::string_view s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == 0x1B && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == '[') {  // CSI
                i += 2;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
                continue;
            }
            if (n == ']') {  // OSC，BEL 或 ST 结束
                i += 2;
                while (i < s.size() && s[i] != '\a' && !(s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == '\\')) ++i;
                if (i < s.size() && s[i] == 0x1B) ++i;
                continue;
            }
            ++i;
            continue;
        }
        if (c == '\r') { o += '\n'; continue; }
        if (c < 0x20 && c != '\n' && c != '\t') continue;
        o += char(c);
    }
    return o;
}

void OutputTail::append(std::string_view b) {
    buf_.append(b);
    if (buf_.size() > cap_) buf_.erase(0, buf_.size() - cap_);
}
std::string OutputTail::text() const { return stripTerminalEscapes(buf_); }

const char *sshErrorName(SshError e) {
    switch (e) {
    case SshError::None: return "None";
    case SshError::RemoteExit: return "RemoteExit";
    case SshError::AuthFailed: return "AuthFailed";
    case SshError::HostKeyUnknown: return "HostKeyUnknown";
    case SshError::HostKeyChanged: return "HostKeyChanged";
    case SshError::ConnectionRefused: return "ConnectionRefused";
    case SshError::Timeout: return "Timeout";
    case SshError::HostNotFound: return "HostNotFound";
    case SshError::NetworkUnreachable: return "NetworkUnreachable";
    case SshError::ClientIncompatible: return "ClientIncompatible";
    case SshError::ConfigError: return "ConfigError";
    case SshError::ConnectionClosed: return "ConnectionClosed";
    case SshError::SshNotFound: return "SshNotFound";
    case SshError::Unknown: return "Unknown";
    }
    return "Unknown";
}

SshExitInfo mapSshExit(uint32_t code, std::string_view tail) {
    SshExitInfo r; r.exitCode = code;
    if (code == 0) { r.error = SshError::None; return r; }
    if (code != 255) { r.error = SshError::RemoteExit; return r; }
    std::string t = stripTerminalEscapes(tail);
    std::string lt = lower(t);
    // 顺序：更具体的在前（主机密钥变化的报告里也含 "Host key verification failed"）
    struct Rule { const char *needle; SshError e; };
    static const Rule rules[] = {
        {"remote host identification has changed", SshError::HostKeyChanged},
        {"host key verification failed", SshError::HostKeyUnknown},
        {"permission denied", SshError::AuthFailed},
        {"too many authentication failures", SshError::AuthFailed},
        {"no supported authentication methods", SshError::AuthFailed},
        {"bad configuration option", SshError::ClientIncompatible},
        {"unsupported option", SshError::ClientIncompatible},
        {"unknown configuration option", SshError::ClientIncompatible},
        {"connection refused", SshError::ConnectionRefused},
        {"timed out", SshError::Timeout},
        {"could not resolve hostname", SshError::HostNotFound},
        {"no such host", SshError::HostNotFound},
        {"network is unreachable", SshError::NetworkUnreachable},
        {"no route to host", SshError::NetworkUnreachable},
        {"can't open user config file", SshError::ConfigError},
        {"bad owner or permissions", SshError::ConfigError},
        {"terminating, ", SshError::ConfigError},
        {"connection closed by", SshError::ConnectionClosed},
        {"connection reset", SshError::ConnectionClosed},
        {"broken pipe", SshError::ConnectionClosed},
    };
    for (auto &ru : rules) {
        size_t p = lt.rfind(ru.needle);
        if (p == std::string::npos) continue;
        size_t b = t.rfind('\n', p); b = b == std::string::npos ? 0 : b + 1;
        size_t e = t.find('\n', p); if (e == std::string::npos) e = t.size();
        r.error = ru.e; r.detail = t.substr(b, e - b);
        return r;
    }
    r.error = SshError::Unknown;
    return r;
}

}  // namespace it2w
