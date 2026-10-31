#include "ssh_a_probe_logic.h"
#include <cstdio>
#include <cstdlib>

using namespace ssh_a_probe;
static void check(bool ok, const char *why) {
    if (!ok) { std::fprintf(stderr, "FAIL self-test: %s\n", why); std::exit(1); }
}
int main() {
    check(clamp({0, 0}) == Grid{20, 5}, "initial size clamp");
    Sizes sizes({80, 24});
    sizes.resize({100, 30}, 2);
    const auto inflight = sizes.desired;
    sizes.resize({120, 40}, 4);
    sizes.resize({90, 20}, 3);
    check(inflight.grid == Grid{100, 30}, "immutable in-flight request");
    check(sizes.desired.grid == Grid{120, 40}, "latest Starting resize / old generation");
    sizes.sent = inflight;
    check(sizes.sent.generation < sizes.desired.generation, "post-pty window-change needed");
    sizes.closing = true;
    sizes.resize({180, 60}, 5);
    check(sizes.desired.grid == Grid{120, 40}, "resize rejected after close");
    check(sizeMatches("24 80\r\n", {80, 24}), "stty row/column order");
    check(!sizeMatches("80 24", {80, 24}) && !sizeMatches("24 80 junk", {80, 24}), "strict size parser");
    const auto bytes = expectedBytes();
    check(compareBytes(bytes).empty(), "79-byte fixture");
#ifndef _WIN32
    // Execute precisely the supplied printf on the native POSIX host as an
    // independent check that the literal fixture and remote command agree.
    auto *pipe = popen(kR18Command, "r");
    check(pipe != nullptr, "open R18 printf");
    std::string printed;
    char block[128];
    while (auto count = std::fread(block, 1, sizeof block, pipe)) printed.append(block, count);
    check(pclose(pipe) == 0 && compareBytes(printed).empty(), "exact R18 printf emits 79 bytes");
#endif
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto corrupt = bytes; corrupt[i] ^= 1;
        check(!compareBytes(corrupt).empty(), "detect every corrupt byte");
        check(!compareBytes(bytes.substr(0, i)).empty(), "detect every truncation");
    }
    check(!compareBytes(bytes + "x").empty(), "detect extra byte");
    Frame frame{"\036begin\037", "\036end\037"};
    const auto encoded = "command echo " + octal(frame.begin) + "\r\n";
    const auto wire = encoded + frame.begin + bytes + frame.end + "0\n";
    for (size_t split = 0; split <= wire.size(); ++split) {
        std::string receive = wire.substr(0, split);
        check(frame.complete(receive) == (split == wire.size()), "no prematurely complete frame");
        receive += wire.substr(split);
        check(frame.complete(receive) && frame.payload(receive) == bytes, "all frame splits");
    }
    bool rejected = false;
    try { frame.payload(frame.begin + bytes + frame.end + "255\n"); } catch (const Failure &) { rejected = true; }
    check(rejected, "remote status 255 preserved as failure, not ssh.exe semantics");
    rejected = false;
    try { number("22junk", 1, 65535, "port"); } catch (const Failure &) { rejected = true; }
    check(rejected, "strict CLI number");
    std::puts("PASS self-test: size generations, framing/splits, exact 79 bytes, CLI; Windows gates NOT RUN");
}
