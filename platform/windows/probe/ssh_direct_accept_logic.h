// Portable consumer-side checks for the product API acceptance harness.
#pragma once
#include "ssh_a_probe_logic.h"
#include <limits>
#include <sstream>
#include <utility>

namespace ssh_direct_accept {
// The first commands run with the PRODUCT's echo/OPOST modes intact. Normalize
// only the frame status CRLF, never the payload (especially the binary fixture).
inline std::string framePayload(const ssh_a_probe::Frame &frame, std::string wire) {
    auto begin = wire.find(frame.begin);
    if (begin != std::string::npos) {
        auto end = wire.find(frame.end, begin + frame.begin.size());
        if (end != std::string::npos) {
            auto status = end + frame.end.size();
            auto line = wire.find('\n', status);
            if (line != std::string::npos && line > status && wire[line - 1] == '\r')
                wire.erase(line - 1, 1);
        }
    }
    return frame.payload(wire);
}

inline bool modesMatch(std::string text) {
    for (char &c : text) if (c == ';') c = ' ';
    std::istringstream stream(text);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(std::move(token));
    auto has = [&](const char *token) {
        return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
    };
    bool erase = false;
    for (size_t i = 0; i + 2 < tokens.size(); ++i)
        if (tokens[i] == "erase" && tokens[i + 1] == "=" && tokens[i + 2] == "^?") erase = true;
    return erase && has("iutf8") && has("echo") && has("TERM=xterm-256color");
}

// tryPopOutput() returns whole OutputChunks, with no size argument. Exercise
// random consumer read sizes by slicing those actual API chunks, retaining the
// unused suffix between reads/commands. No tap or direct queue access supplies data.
class OutputSlices {
    std::string pending_;
    size_t offset_ = 0;
public:
    bool empty() const { return offset_ == pending_.size(); }
    void put(std::string bytes) {
        if (!empty()) throw ssh_a_probe::Failure("unconsumed output slice");
        pending_ = std::move(bytes); offset_ = 0;
    }
    std::string pop(size_t maxBytes = std::numeric_limits<size_t>::max()) {
        if (!maxBytes) throw ssh_a_probe::Failure("zero-sized output read");
        auto result = pending_.substr(offset_, std::min(maxBytes, pending_.size() - offset_));
        offset_ += result.size();
        return result;
    }
};
} // namespace ssh_direct_accept
