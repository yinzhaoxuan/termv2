// SPDX-License-Identifier: GPL-2.0-only
// itcore_test_tokens <向量文件> [--case NAME] [--check]
//   向量文件为 /workspace/qa/vectors/ 格式的 JSON（数组，每项含 case/name/hex/splits）：
//     对每个向量按 whole 切分输出规范化 Token（每行一个 JSON），向量之间以 "# case name" 注释行分隔；
//     --check：对所有切分方式（whole/bytewise/all_two_splits/random）比较输出，不一致则非零退出。
//   其他文件按原始字节一次性喂入并输出 Token。
#include "vt_parser.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
// ---- 极简 JSON 读取（仅用于测试向量） ----
struct J {
    enum K { Null, Num, Str, Arr, Obj, Bool } k = Null;
    double n = 0; std::string s; std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J *get(const std::string &key) const { for (auto &p : o) if (p.first == key) return &p.second; return nullptr; }
};
struct P {
    const std::string &s; size_t i = 0;
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
    [[noreturn]] void fail() { throw std::runtime_error("bad json at " + std::to_string(i)); }
    J val() {
        ws(); if (i >= s.size()) fail();
        J j; char c = s[i];
        if (c == '{') { j.k = J::Obj; ++i; ws(); if (s[i] == '}') { ++i; return j; }
            for (;;) { ws(); J key = val(); ws(); if (s[i++] != ':') fail(); j.o.emplace_back(key.s, val()); ws();
                if (s[i] == ',') { ++i; continue; } if (s[i++] == '}') return j; fail(); } }
        if (c == '[') { j.k = J::Arr; ++i; ws(); if (s[i] == ']') { ++i; return j; }
            for (;;) { j.a.push_back(val()); ws(); if (s[i] == ',') { ++i; continue; } if (s[i++] == ']') return j; fail(); } }
        if (c == '"') { j.k = J::Str; ++i;
            while (s[i] != '"') { if (s[i] == '\\') { ++i; char e = s[i++];
                    if (e == 'u') { unsigned cp = std::stoul(s.substr(i, 4), nullptr, 16); i += 4;
                        if (cp < 0x80) j.s += char(cp); else if (cp < 0x800) { j.s += char(0xc0 | cp >> 6); j.s += char(0x80 | (cp & 63)); }
                        else { j.s += char(0xe0 | cp >> 12); j.s += char(0x80 | ((cp >> 6) & 63)); j.s += char(0x80 | (cp & 63)); } }
                    else j.s += e == 'n' ? '\n' : e == 't' ? '\t' : e; }
                else j.s += s[i++]; }
            ++i; return j; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return j; }
        if (s.compare(i, 4, "true") == 0) { i += 4; j.k = J::Bool; j.n = 1; return j; }
        if (s.compare(i, 5, "false") == 0) { i += 5; j.k = J::Bool; return j; }
        size_t e; j.k = J::Num; j.n = std::stod(s.substr(i, 32), &e); i += e; return j;
    }
};

struct Collect : itcore::TokenSink {
    std::string out;
    void token(const itcore::Token &t) override { out += itcore::to_json(t); out += '\n'; }
};

std::string run(const std::string &bytes, const std::vector<std::pair<size_t, size_t>> &chunks) {
    itcore::Parser p; Collect c;
    for (auto [a, b] : chunks) p.feed(reinterpret_cast<const uint8_t *>(bytes.data()) + a, b - a, c);
    p.finish(c);
    return c.out;
}
std::vector<std::pair<size_t, size_t>> chunks_of(const J &arr) {
    std::vector<std::pair<size_t, size_t>> v;
    for (auto &r : arr.a) v.emplace_back(size_t(r.a.at(0).n), size_t(r.a.at(1).n));
    return v;
}
std::string unhex(const std::string &h) {
    std::string b; for (size_t i = 0; i + 1 < h.size(); i += 2) b += char(std::stoi(h.substr(i, 2), nullptr, 16)); return b;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: itcore_test_tokens <vector-file> [--case NAME] [--check]\n"); return 2; }
    std::string path = argv[1], only; bool check = false;
    for (int i = 2; i < argc; ++i) { std::string a = argv[i];
        if (a == "--check") check = true; else if (a == "--case" && i + 1 < argc) only = argv[++i]; }
    std::ifstream f(path, std::ios::binary); if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    std::stringstream ss; ss << f.rdbuf(); std::string data = ss.str();
    size_t first = data.find_first_not_of(" \t\r\n");
    bool isVec = path.size() > 5 && path.substr(path.size() - 5) == ".json" && first != std::string::npos && data[first] == '[';
    if (!isVec) { std::fputs(run(data, {{0, data.size()}}).c_str(), stdout); return 0; }

    J root; try { P p{data}; root = p.val(); } catch (std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
    int failures = 0, matched = 0;
    for (auto &v : root.a) {
        std::string name = v.get("name")->s; if (!only.empty() && name != only) continue;
        ++matched;
        std::string bytes = unhex(v.get("hex")->s);
        std::string ref = run(bytes, {{0, bytes.size()}});
        std::printf("# %s %s\n%s", v.get("case")->s.c_str(), name.c_str(), ref.c_str());
        if (!check) continue;
        const J *sp = v.get("splits"); int n = 0, bad = 0;
        for (auto &[kind, s] : sp->o) {
            std::vector<const J *> variants;
            if (kind == "all_two_splits") for (auto &x : s.a) variants.push_back(&x); else variants.push_back(&s);
            for (auto *x : variants) { ++n; std::string got = run(bytes, chunks_of(*x));
                if (got != ref) { if (!bad++) std::fprintf(stderr, "MISMATCH %s split=%s\n--- whole\n%s--- got\n%s", name.c_str(), kind.c_str(), ref.c_str(), got.c_str()); } }
        }
        std::fprintf(stderr, "%s %s: %d splits, %d mismatches\n", bad ? "FAIL" : "PASS", name.c_str(), n, bad);
        if (bad) ++failures;
    }
    if (!matched) { std::fprintf(stderr, "no vector matched\n"); return 2; }
    return failures ? 1 : 0;
}
