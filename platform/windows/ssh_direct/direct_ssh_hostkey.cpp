// SPDX-License-Identifier: GPL-3.0-or-later
#include "direct_ssh_internal.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace it2w {
using namespace direct_ssh;
namespace {
std::optional<DirectSha1> hmacSha1(std::string_view salt, std::string_view host) {
    DirectSha1 digest{};
    unsigned length = 0;
    if (!HMAC(EVP_sha1(), salt.data(), static_cast<int>(salt.size()),
              reinterpret_cast<const unsigned char *>(host.data()), host.size(),
              digest.data(), &length) || length != digest.size()) return std::nullopt;
    return digest;
}
void lowerAscii(std::string &s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
}
} // namespace

void DirectSshTransport::Impl::prepareHostKey() {
    setPhase(DirectStage::KnownHosts, "known_hosts");
    // Same string OpenSSH uses: alias verbatim when set, otherwise put_host_port(host).
    hostLower = directHostKeyLookup(target.hostName, target.port, target.hostKeyAlias);
    std::ifstream file(std::filesystem::path(widen(target.appKnownHostsFile)), std::ios::binary);
    if (!file) fail(DirectFailureKind::KnownHostsOpenFailed);
    // Same 16 MiB limit as the dependency probe; never load an unbounded file.
    std::string text;
    char buffer[64 * 1024];
    while (file) {
        checkpoint();
        file.read(buffer, sizeof buffer);
        if (text.size() + static_cast<size_t>(file.gcount()) > 16u * 1024 * 1024)
            fail(DirectFailureKind::KnownHostsParseFailed, 0, "known_hosts exceeds 16 MiB");
        text.append(buffer, static_cast<size_t>(file.gcount()));
    }
    if (!file.eof()) fail(DirectFailureKind::KnownHostsOpenFailed, 0, "known_hosts read failed");
    const auto types = knownHostTypesFor(text, target.hostName, target.port, hmacSha1,
                                          target.hostKeyAlias);
    if (types.error) fail(DirectFailureKind::KnownHostsParseFailed, 0, types.error->reason);
    knownTypes = types.types;
    knownHosts = libssh2_knownhost_init(session);
    if (!knownHosts) fail(DirectFailureKind::Other, LIBSSH2_ERROR_ALLOC);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        checkpoint();
        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#') continue;
        // The portable parser has already rejected markers. Normalize only the
        // host field, since libssh2's plain-name comparison is case-sensitive.
        if (line[start] != '|') {
            auto end = line.find_first_of(" \t\r", start);
            auto hosts = line.substr(start, end - start);
            lowerAscii(hosts);
            line.replace(start, hosts.size(), hosts);
        }
        int rc = libssh2_knownhost_readline(knownHosts, line.data(), line.size(),
                                           LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        if (rc) fail(DirectFailureKind::KnownHostsParseFailed, rc);
    }
    const char **algorithms = nullptr;
    int count = libssh2_session_supported_algs(session, LIBSSH2_METHOD_HOSTKEY, &algorithms);
    if (count < 0) fail(DirectFailureKind::NoHostKeyAlgorithm, count);
    std::vector<std::string> supported;
    for (int i = 0; i < count; ++i) supported.emplace_back(algorithms[i]);
    libssh2_free(session, const_cast<char **>(algorithms));
    const auto pref = hostKeyPref(knownTypes, supported);
    if (pref.empty()) fail(DirectFailureKind::NoHostKeyAlgorithm);
    int rc = libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, pref.c_str());
    if (rc) fail(DirectFailureKind::NoHostKeyAlgorithm, rc);
}

void DirectSshTransport::Impl::verifyHostKey() {
    setPhase(DirectStage::HostKey, "hostkey");
    size_t length = 0;
    int type = 0, algorithm = 0;
    const char *key = libssh2_session_hostkey(session, &length, &type);
    if (!key) fail(DirectFailureKind::NoHostKeyAlgorithm);
    DirectHostKeyType keyType = DirectHostKeyType::Unknown;
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: algorithm = LIBSSH2_KNOWNHOST_KEY_SSHRSA; keyType = DirectHostKeyType::Rsa; break;
    case LIBSSH2_HOSTKEY_TYPE_DSS: algorithm = LIBSSH2_KNOWNHOST_KEY_SSHDSS; keyType = DirectHostKeyType::Dss; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; keyType = DirectHostKeyType::Ecdsa256; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; keyType = DirectHostKeyType::Ecdsa384; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: algorithm = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; keyType = DirectHostKeyType::Ecdsa521; break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: algorithm = LIBSSH2_KNOWNHOST_KEY_ED25519; keyType = DirectHostKeyType::Ed25519; break;
    default: fail(DirectFailureKind::NoHostKeyAlgorithm, type);
    }
    const auto name = directHostKeyTypeName(keyType);
    if (!name) fail(DirectFailureKind::NoHostKeyAlgorithm, type, "不支持的主机密钥类型");
    unsigned char digest[EVP_MAX_MD_SIZE], encoded[4 * EVP_MAX_MD_SIZE + 1]{};
    unsigned digestLength = 0;
    if (!EVP_Digest(key, length, digest, &digestLength, EVP_sha256(), nullptr))
        fail(DirectFailureKind::Other, 0, "hostkey SHA256 failed");
    EVP_EncodeBlock(encoded, digest, static_cast<int>(digestLength));
    std::string fingerprint(reinterpret_cast<char *>(encoded));
    while (!fingerprint.empty() && fingerprint.back() == '=') fingerprint.pop_back();
    fingerprint = "SHA256:" + fingerprint;
    std::string detail = std::string(*name) + " " + fingerprint;
    diag("ssh.direct.hostkey", 0, 0, detail);
    // hostLower is already the known_hosts name (alias, or [host]:port). Do not
    // bracket it again: OpenSSH does not put_host_port a HostKeyAlias.
    // Explicitly type the *presented* raw key, never a preferred/known type.
    int rc = libssh2_knownhost_check(knownHosts, hostLower.c_str(), key, length,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | algorithm, nullptr);
    if (rc == LIBSSH2_KNOWNHOST_CHECK_MATCH) return;
    if (rc == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
        fail(DirectFailureKind::HostKeyChanged, 0, detail);
    if (rc == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        if (!knownTypes.empty()) {
            detail += "; known_hosts types=";
            for (const auto &t : knownTypes) detail += t + " ";
            detail += "; server presented=" + std::string(*name);
        }
        fail(DirectFailureKind::HostKeyUnknown, 0, detail, std::nullopt,
             presentHostKey(hostLower, keyType, {key, length}, fingerprint));
    }
    fail(DirectFailureKind::KnownHostsParseFailed, rc, detail);
}

} // namespace it2w
