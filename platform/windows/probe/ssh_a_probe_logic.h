// Probe-only protocol/CLI logic. No product transport or terminal parser involved.
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ssh_a_probe {
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Cancelled {};
struct Grid {
    int cols = 80, rows = 24;
    bool operator==(const Grid &) const = default;
};
inline Grid clamp(Grid g) { return {std::max(20, g.cols), std::max(5, g.rows)}; }
struct SizeRequest { Grid grid; uint64_t generation = 0; };
// desired can change while an immutable in-flight request is retried on EAGAIN.
struct Sizes {
    SizeRequest desired, sent;
    bool closing = false;
    explicit Sizes(Grid g) : desired{clamp(g), 1}, sent{{}, 0} {}
    void resize(Grid g, uint64_t gen) {
        if (!closing && gen > desired.generation) desired = {clamp(g), gen};
    }
};

// R18-1 exact remote printf supplied in probe-brief's follow-up (2026-11-01).
// Keep command escapes and the independent literal byte fixture separate.
inline constexpr char kR18Command[] =
    "printf 'M1\\033P1000p\\033\\\\M2\\033P1000pXYZM3\\033Pq#0;2;0;0;0#0~~@@vv@@~~\\033\\\\M4\\033]52;c;?\\007M5\\033]52;c;aGk=\\007M6'";
inline constexpr unsigned char kR18Bytes[] = {
    'M','1',0x1b,'P','1','0','0','0','p',0x1b,'\\',
    'M','2',0x1b,'P','1','0','0','0','p','X','Y','Z',
    'M','3',0x1b,'P','q','#','0',';','2',';','0',';','0',';','0',
    '#','0','~','~','@','@','v','v','@','@','~','~',0x1b,'\\',
    'M','4',0x1b,']','5','2',';','c',';','?',0x07,
    'M','5',0x1b,']','5','2',';','c',';','a','G','k','=',0x07,'M','6'
};
static_assert(sizeof kR18Bytes == 79);
inline std::string expectedBytes() {
    return {reinterpret_cast<const char *>(kR18Bytes), sizeof kR18Bytes};
}
inline int number(std::string_view text, int low, int high, const char *option) {
    int n = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc{} || end != text.data() + text.size() || n < low || n > high)
        throw Failure(std::string(option) + ": integer out of range");
    return n;
}
inline bool sizeMatches(std::string_view text, Grid expected) {
    // stty prints rows then columns. Permit whitespace, never arbitrary suffixes.
    auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::vector<int> parts;
    while (!text.empty()) {
        while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
        if (text.empty()) break;
        int n = 0;
        auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
        if (ec != std::errc{} || p == text.data() || (p != text.data() + text.size() && !whitespace(*p))) return false;
        parts.push_back(n);
        if (parts.size() > 2) return false;
        text.remove_prefix(static_cast<size_t>(p - text.data()));
    }
    return parts.size() == 2 && parts[0] == expected.rows && parts[1] == expected.cols;
}
inline std::string compareBytes(std::string_view actual) {
    const auto expected = expectedBytes();
    size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
    if (i == expected.size() && actual.size() == expected.size()) return {};
    return "79-byte mismatch at offset " + std::to_string(i) + ", received=" + std::to_string(actual.size());
}
// A frame includes the command status outside its byte payload. Marker strings
// are emitted as octal escapes, so terminal command echo cannot imitate them.
struct Frame {
    std::string begin, end;
    std::string payload(const std::string &buffer) const {
        auto a = buffer.find(begin);
        if (a == std::string::npos) throw Failure("missing frame start");
        a += begin.size();
        auto b = buffer.find(end, a);
        if (b == std::string::npos) throw Failure("missing frame end");
        auto status = b + end.size();
        auto line = buffer.find('\n', status);
        if (line == std::string::npos || buffer.substr(status, line - status) != "0")
            throw Failure("remote command failed (frame status must be 0)");
        return buffer.substr(a, b - a);
    }
    bool complete(const std::string &buffer) const {
        auto a = buffer.find(begin);
        if (a == std::string::npos) return false;
        auto b = buffer.find(end, a + begin.size());
        return b != std::string::npos && buffer.find('\n', b + end.size()) != std::string::npos;
    }
};
inline std::string octal(std::string_view text) {
    std::string result;
    for (unsigned char c : text) {
        result += '\\'; result += char('0' + ((c >> 6) & 7));
        result += char('0' + ((c >> 3) & 7)); result += char('0' + (c & 7));
    }
    return result;
}
} // namespace ssh_a_probe
