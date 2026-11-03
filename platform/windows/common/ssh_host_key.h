// SSH 主机密钥展示数据：跨平台共享。
#pragma once
#include <string>

namespace it2w {

struct PresentedHostKey {
    std::string knownHostsName, keyType, keyBase64, fingerprint;
};

} // namespace it2w
