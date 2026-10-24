// ABI-01 / PARSE-01 部分：feed_result 语义、事件释放、EOF、超大 OSC。
#include "itcore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static itcore_core *make() {
    itcore_options o{ITCORE_ABI_VERSION, sizeof(itcore_options), 80, 24, 1000, 0};
    itcore_core *c = nullptr;
    CHECK(itcore_create(&o, &c) == 0 && c);
    return c;
}
struct Ev { itcore_event_kind k; std::string d; };
static std::vector<Ev> drain(itcore_core *c) {
    std::vector<Ev> v;
    while (itcore_event *e = itcore_next_event(c)) {
        size_t n; const uint8_t *p = itcore_event_data(e, &n);
        v.push_back({itcore_event_type(e), std::string(reinterpret_cast<const char *>(p), n)});
        itcore_event_release(e);
    }
    return v;
}
static itcore_feed_result feed(itcore_core *c, const std::string &s) {
    return itcore_feed(c, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

static void test_basic_events() {
    itcore_core *c = make();
    auto r = feed(c, "hi\x07\x1b]2;ti\x01tle\x07\x1b[5n\x1b]52;c;aGk=\x07");
    CHECK(r.status == ITCORE_OK && r.accepted == 30);
    auto v = drain(c);
    bool bell = false, title = false, reply = false, clip = false; int inval = 0;
    for (auto &e : v) {
        bell |= e.k == ITCORE_EV_BELL;
        title |= e.k == ITCORE_EV_TITLE && e.d == "title";
        reply |= e.k == ITCORE_EV_REPLY_BYTES && e.d == "\x1b[0n";
        clip |= e.k == ITCORE_EV_CLIPBOARD_REQUEST && e.d == "c";
        inval += e.k == ITCORE_EV_RENDER_INVALIDATED;
    }
    CHECK(bell && title && reply && clip);
    CHECK(inval == 1);  // 合并
    CHECK(itcore_feed(c, nullptr, 0).status == ITCORE_OK);
    itcore_destroy(c);
}

static void test_title_cap() {
    itcore_core *c = make();
    std::string t; for (int i = 0; i < 300; ++i) t += "中";
    feed(c, "\x1b]0;" + t + "\x07");
    auto v = drain(c);
    // OSC 0 同时设置窗口标题与图标标题（ITCORE_EV_ICON_TITLE），两者都截断到 256 字符
    CHECK(v.size() == 2 && v[0].k == ITCORE_EV_TITLE && v[0].d.size() == 256 * 3);
    CHECK(v.size() == 2 && v[1].k == ITCORE_EV_ICON_TITLE && v[1].d.size() == 256 * 3);
    feed(c, "\x1b]2;" + t + "\x07");
    v = drain(c);
    CHECK(v.size() == 1 && v[0].k == ITCORE_EV_TITLE && v[0].d.size() == 256 * 3);
    itcore_destroy(c);
}

static void test_need_drain() {
    itcore_core *c = make();
    std::string bells(5000, '\x07');
    size_t off = 0; int drains = 0; size_t total = 0;
    while (off < bells.size()) {
        auto r = itcore_feed(c, reinterpret_cast<const uint8_t *>(bells.data()) + off, bells.size() - off);
        CHECK(r.status != ITCORE_FATAL);
        if (r.accepted == 0) CHECK(r.status == ITCORE_NEED_DRAIN);
        off += r.accepted;
        if (r.status == ITCORE_NEED_DRAIN) { ++drains; CHECK(r.accepted > 0 || drains > 0); }
        total += drain(c).size();
        if (drains > 100) break;
    }
    CHECK(drains >= 4);
    CHECK(total == 5000);  // 无丢失、无重复
    // 不排空时 accepted=0 必须伴随 NEED_DRAIN
    for (int i = 0; i < 2; ++i) feed(c, std::string(2000, '\x07'));
    auto r = feed(c, "\x07");
    CHECK(r.accepted == 0 && r.status == ITCORE_NEED_DRAIN);
    itcore_destroy(c);  // 未取出的事件由 destroy 释放（ASan/valgrind 检查）
}

static void test_finish() {
    itcore_core *c = make();
    feed(c, "a\xe4\xb8");
    drain(c);
    CHECK(itcore_finish_input(c) == 0);
    auto v = drain(c);
    CHECK(v.size() == 1 && v[0].k == ITCORE_EV_RENDER_INVALIDATED);  // U+FFFD 进入屏幕
    CHECK(itcore_incomplete_count(c) == 1);
    CHECK(itcore_finish_input(c) == 0);
    CHECK(drain(c).empty() && itcore_incomplete_count(c) == 1);  // 重复调用无副作用
    auto r = feed(c, "x");
    CHECK(r.status == ITCORE_FATAL && r.accepted == 0);
    itcore_destroy(c);

    c = make();
    feed(c, "\x1b[12;");
    itcore_finish_input(c);
    CHECK(itcore_incomplete_count(c) == 1);
    itcore_destroy(c);
}

static void test_bad_args() {
    itcore_core *c = nullptr;
    itcore_options o{ITCORE_ABI_VERSION + 1, sizeof(itcore_options), 80, 24, 0, 0};
    CHECK(itcore_create(&o, &c) != 0 && !c);
    CHECK(itcore_feed(nullptr, nullptr, 0).status == ITCORE_FATAL);
}

// B4：OSC 52 —— 默认全关；写需 allow_write、读另需 allow_read；解码后大小上限
static Ev only(itcore_core *c, const std::string &s) {
    feed(c, s);
    Ev r{ITCORE_EV_BELL, "<none>"}; int n = 0;
    for (auto &e : drain(c)) if (e.k != ITCORE_EV_RENDER_INVALIDATED) { r = e; ++n; }
    CHECK(n == 1);
    return r;
}
static void test_osc52() {
    itcore_core *c = make();
    itcore_buffer *b = nullptr;
    auto isReq = [](const Ev &e, const char *sel) { return e.k == ITCORE_EV_CLIPBOARD_REQUEST && e.d == sel; };
    // 默认：写、读都拒绝；读应答编码也拒绝
    CHECK(isReq(only(c, "\x1b]52;c;aGk=\x07"), "c"));
    CHECK(isReq(only(c, "\x1b]52;c;?\x07"), "c"));
    CHECK(itcore_encode_clipboard_reply(c, nullptr, "x", 1, &b) == 1 && !b);
    // 只开写
    itcore_clipboard_policy p{sizeof(p), 1, 0, 8};
    CHECK(itcore_set_clipboard_policy(c, &p) == 0);
    Ev w = only(c, "\x1b]52;c;aGk=\x07");
    CHECK(w.k == ITCORE_EV_CLIPBOARD_WRITE && w.d == "hi");
    w = only(c, "\x1b]52;p;5Lit\x1b\\");  // ST 结尾、UTF-8
    CHECK(w.k == ITCORE_EV_CLIPBOARD_WRITE && w.d == "\xe4\xb8\xad");
    CHECK(only(c, "\x1b]52;c;aGk\x07").d == "hi");  // 无填充也接受
    auto none = [&](const std::string &in) {
        feed(c, in); int n = 0;
        for (auto &e : drain(c)) if (e.k != ITCORE_EV_RENDER_INVALIDATED) ++n;
        return n == 0;
    };
    CHECK(none("\x1b]52;c;\x07"));                   // 空 Pd：不动剪贴板，不产生事件
    CHECK(none("\x1b]52;;\x1b\\"));
    // base64 空白（折行）忽略
    CHECK(only(c, "\x1b]52;c;aG\r\nk=\x07").d == "hi");
    CHECK(only(c, "\x1b]52;c; aGk= \x07").d == "hi");
    CHECK(isReq(only(c, "\x1b]52;c;aGkx\nY\x07"), "c"));  // 有效字符 5 个：多出 1 个不成字节
    // 清洗：控制字符去掉、非法 UTF-8 → U+FFFD、C1 去掉
    CHECK(only(c, "\x1b]52;c;YRtbbQAKYg==\x07").d == "a[m\nb");                    // "a\x1b[m\0\nb"：ESC、NUL 去掉，\n 保留
    CHECK(only(c, "\x1b]52;c;/w==\x07").d == "\xEF\xBF\xBD");                       // 0xFF
    CHECK(only(c, "\x1b]52;c;wpt4\x07").d == "x");                                   // U+009B（CSI）+ x
    CHECK(only(c, "\x1b]52;c;wK8=\x07").d == "\xEF\xBF\xBD\xEF\xBF\xBD");             // 过长编码 C0 AF
    CHECK(isReq(only(c, "\x1b]52;c;7aCA\x07"), "c"));                                  // 代理 U+D800 → 3 个 U+FFFD = 9 字节 > 8
    CHECK(isReq(only(c, "\x1b]52;c;/////w==\x07"), "c"));                           // 4 个 0xFF → 12 字节 > 8
    // Pc 校验：非法字符/过长拒绝（负载为空）；空 Pc → s0
    CHECK(isReq(only(c, "\x1b]52;x;aGk=\x07"), ""));
    CHECK(isReq(only(c, "\x1b]52;c!;aGk=\x07"), ""));
    CHECK(isReq(only(c, "\x1b]52;ccccccccc;aGk=\x07"), ""));
    CHECK(only(c, "\x1b]52;s0;aGk=\x07").k == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(only(c, "\x1b]52;;aGk=\x07").k == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(isReq(only(c, "\x1b]52;c;MTIzNDU2Nzg=\x07"), "c") == false);  // 恰 8 字节：允许
    CHECK(isReq(only(c, "\x1b]52;c;MTIzNDU2Nzg5\x07"), "c"));          // 9 字节：超上限
    CHECK(isReq(only(c, "\x1b]52;c;" + std::string(4096, 'A') + "\x07"), "c"));  // 远超：不解码直接拒
    CHECK(isReq(only(c, "\x1b]52;c;a$k=\x07"), "c"));   // 非法字符
    CHECK(isReq(only(c, "\x1b]52;c;aG=k\x07"), "c"));   // 填充后有数据
    CHECK(isReq(only(c, "\x1b]52;c\x07"), "c"));        // 缺数据段
    // OSC52-02：Pc 里的 q、0-7、重复字符都合法（原样规范化），8 不合法
    CHECK(only(c, "\x1b]52;q;aGk=\x07").k == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(only(c, "\x1b]52;01234567;aGk=\x07").k == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(only(c, "\x1b]52;cc;aGk=\x07").k == ITCORE_EV_CLIPBOARD_WRITE);
    CHECK(isReq(only(c, "\x1b]52;8;aGk=\x07"), ""));
    CHECK(isReq(only(c, "\x1b]52;cq;?\x07"), "cq"));  // 读未开：拒绝时回规范化的 Pc
    // 超出 U+10FFFF 的 F4 90 与非法首字节 F5：逐字节 U+FFFD
    CHECK(only(c, "\x1b]52;c;9JA=\x07").d == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(only(c, "\x1b]52;c;9Q==\x07").d == "\xEF\xBF\xBD");
    // 截断的多字节序列（E4 B8 少一字节；末尾单个 E4）
    CHECK(only(c, "\x1b]52;c;5Lg=\x07").d == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(only(c, "\x1b]52;c;YeQ=\x07").d == "a\xEF\xBF\xBD");
    // 清洗后为空的 Pd：只有 NUL / 只有控制字符，不产生事件（同空 Pd）
    CHECK(none("\x1b]52;c;AA==\x07"));
    CHECK(none("\x1b]52;c;ARs=\x07"));
    CHECK(isReq(only(c, "\x1b]52;c;?\x07"), "c"));      // 写开 ≠ 读开
    CHECK(itcore_encode_clipboard_reply(c, nullptr, "x", 1, &b) == 1);
    // 再开读
    p = {sizeof(p), 1, 1, 8};
    CHECK(itcore_set_clipboard_policy(c, &p) == 0);
    Ev r = only(c, "\x1b]52;c;?\x07");
    CHECK(r.k == ITCORE_EV_CLIPBOARD_READ && r.d == "c");
    CHECK(itcore_encode_clipboard_reply(c, nullptr, "hi", 2, &b) == 0 && b);
    CHECK(std::string(reinterpret_cast<char *>(b->data), b->length) == "\x1b]52;c;aGk=\x1b\\");
    itcore_buffer_release(b); b = nullptr;
    CHECK(itcore_encode_clipboard_reply(c, nullptr, "", 0, &b) == 0 && std::string(reinterpret_cast<char *>(b->data), b->length) == "\x1b]52;c;\x1b\\");
    itcore_buffer_release(b); b = nullptr;
    CHECK(itcore_encode_clipboard_reply(c, nullptr, "123456789", 9, &b) == -2 && !b);  // 读也受上限
    // 应答结尾与请求一致：BEL ↔ BEL，ST ↔ ST
    auto replyFor = [&](const std::string &req) {
        feed(c, req);
        std::string r; int reads = 0;
        while (itcore_event *e = itcore_next_event(c)) {
            if (itcore_event_type(e) == ITCORE_EV_CLIPBOARD_READ) {
                ++reads;
                itcore_buffer *bb = nullptr;
                CHECK(itcore_encode_clipboard_reply(c, e, "hi", 2, &bb) == 0 && bb);
                if (bb) { r.assign(reinterpret_cast<char *>(bb->data), bb->length); itcore_buffer_release(bb); }
            } else if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) {
                itcore_buffer *bb = nullptr;
                CHECK(itcore_encode_clipboard_reply(c, e, "hi", 2, &bb) == -1 && !bb);  // 非 READ 事件
            }
            itcore_event_release(e);
        }
        CHECK(reads == 1);
        return r;
    };
    CHECK(replyFor("\x1b]52;c;?\x07") == "\x1b]52;c;aGk=\x07");
    CHECK(replyFor("\x1b]52;c;?\x1b\\") == "\x1b]52;c;aGk=\x1b\\");
    CHECK(replyFor("\x1b]52;c;?\x07\x1b[5n") == "\x1b]52;c;aGk=\x07");  // 与 DSR 同批，非 READ 事件被拒
    CHECK(replyFor("\x1b]52;p;?\x07") == "\x1b]52;p;aGk=\x07");           // Pc 回显
    CHECK(replyFor("\x1b]52;;?\x07") == "\x1b]52;s0;aGk=\x07");           // 空 Pc → s0
    CHECK(only(c, "\x1b]52;q;?\x07").d == "q");
    CHECK(isReq(only(c, "\x1b]52;c\x07"), "c"));  // 读写都开：没有第二个分号仍按拒绝
    // 只开读不开写
    p = {sizeof(p), 0, 1, 0};
    CHECK(itcore_set_clipboard_policy(c, &p) == 0);
    CHECK(isReq(only(c, "\x1b]52;c;aGk=\x07"), "c"));
    CHECK(only(c, "\x1b]52;c;?\x07").k == ITCORE_EV_CLIPBOARD_READ);
    // NULL 恢复全关；struct_size 过小拒绝
    CHECK(itcore_set_clipboard_policy(c, nullptr) == 0);
    CHECK(isReq(only(c, "\x1b]52;c;?\x07"), "c"));
    itcore_clipboard_policy bad{4, 1, 1, 0};
    CHECK(itcore_set_clipboard_policy(c, &bad) == -1);
    // max_bytes 超过 OSC 负载能承载的量：钳到 768 KiB（读应答上限随之）
    itcore_clipboard_policy big{sizeof(big), 0, 1, 0xFFFFFFFFu};
    CHECK(itcore_set_clipboard_policy(c, &big) == 0);
    std::string huge(768u * 1024u + 1, 'x');
    CHECK(itcore_encode_clipboard_reply(c, nullptr, huge.data(), huge.size(), &b) == -2 && !b);
    CHECK(itcore_encode_clipboard_reply(c, nullptr, huge.data(), huge.size() - 1, &b) == 0 && b);
    itcore_buffer_release(b); b = nullptr;
    // OSC52-02：负载超过 1 MiB 被解析器截断时，不能把前半截写进剪贴板
    itcore_clipboard_policy bigw{sizeof(bigw), 1, 0, 768u * 1024u};
    CHECK(itcore_set_clipboard_policy(c, &bigw) == 0);
    CHECK(isReq(only(c, "\x1b]52;c;" + std::string((1u << 20) + 4, 'Q') + "\x07"), "c"));
    CHECK(only(c, "\x1b]52;c;aGk=\x07").d == "hi");  // 截断不影响后续
    CHECK(itcore_set_clipboard_policy(c, nullptr) == 0);
    CHECK(isReq(only(c, "\x1b]52;c;aGk=\x07"), "c"));
    itcore_destroy(c);
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "osc52") == 0) {
        test_osc52();
        if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
        std::printf("osc52 ok\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "big_osc") == 0) {
        // 1.5 MiB OSC：截断但可恢复，后续 BEL 正常
        itcore_core *c = make();
        std::string s = "\x1b]2;" + std::string(1536 * 1024, 'x') + "\x07\x07";
        auto r = feed(c, s);
        CHECK(r.status == ITCORE_OK && r.accepted == s.size());
        auto v = drain(c);
        CHECK(v.size() == 2 && v[0].k == ITCORE_EV_TITLE && v[0].d.size() == 256 && v[1].k == ITCORE_EV_BELL);
        itcore_destroy(c);
        // 超过 1 MiB + 1 MiB 丢弃预算：FATAL，之后 feed 失败
        c = make();
        std::string big = "\x1b]2;" + std::string((2u << 20) + 16, 'y');
        size_t off = 0; itcore_status st = ITCORE_OK;
        while (off < big.size() && st != ITCORE_FATAL) {  // 64 KiB 分块，模拟管道读
            size_t n = std::min<size_t>(65536, big.size() - off);
            auto rr = itcore_feed(c, reinterpret_cast<const uint8_t *>(big.data()) + off, n);
            off += rr.accepted; st = rr.status;
        }
        CHECK(st == ITCORE_FATAL);
        CHECK(off == 2 + (2u << 20) + 1);  // ESC ] + 1 MiB 负载 + 1 MiB 丢弃预算 + 1
        CHECK(feed(c, "a").status == ITCORE_FATAL);
        CHECK(itcore_finish_input(c) == 0);
        itcore_destroy(c);
    } else {
        test_basic_events(); test_title_cap(); test_need_drain(); test_finish(); test_bad_args();
    }
    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
