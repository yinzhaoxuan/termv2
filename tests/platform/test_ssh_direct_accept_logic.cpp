#include "ssh_direct_accept_logic.h"
#include <cassert>
#include <cstdio>
#include <random>

int main() {
    using namespace ssh_a_probe;
    using namespace ssh_direct_accept;
    Frame frame{"\036DIRECT_BEGIN\037", "\036DIRECT_END\037"};
    const auto payload = expectedBytes() + "\r\n";
    const auto wire = "echo " + octal(frame.begin) + frame.begin + payload + frame.end + "0\r\n";
    std::mt19937 random(0x535348);
    // Vary both actual API chunk boundaries and independent consumer read sizes.
    for (size_t chunkSize : {size_t(1), size_t(7), size_t(79), wire.size()}) {
        OutputSlices output;
        std::string capture;
        for (size_t pos = 0; pos < wire.size(); pos += chunkSize) {
            output.put(wire.substr(pos, chunkSize));
            while (!output.empty()) capture += output.pop(1 + random() % 97);
        }
        assert(frame.complete(capture));
        assert(framePayload(frame, capture) == payload); // payload CRLF is untouched
        assert(compareBytes(framePayload(frame, frame.begin + expectedBytes() + frame.end + "0\n")).empty());
    }
    for (const auto &status : {"1\r\n", "255\n", "0junk\r\n"}) {
        bool rejected = false;
        try { framePayload(frame, frame.begin + payload + frame.end + status); }
        catch (const Failure &) { rejected = true; }
        assert(rejected);
    }
    const std::string modes = "speed 38400 baud; erase = ^?;\r\niutf8 echo echoe -echonl\r\nTERM=xterm-256color\r\n";
    assert(modesMatch(modes));
    for (const auto &pair : {std::pair{"iutf8", "-iutf8"}, {" echo ", " -echo "},
                             {"^?", "^H"}, {"TERM=xterm-256color", "TERM=xterm"}}) {
        auto changed = modes;
        changed.replace(changed.find(pair.first), std::string(pair.first).size(), pair.second);
        assert(!modesMatch(changed));
    }
    std::puts("PASS direct accept logic: product-mode frames, random consumer reads, exact mode tokens");
}
