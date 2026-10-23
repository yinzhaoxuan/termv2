// SPDX-License-Identifier: GPL-2.0-only
// 输入编码：text / paste / key / mouse（设计 14.1 / 14.3，T06 ABI）。
#include "encode.h"
#include <new>
#include <string>
#include <string_view>

namespace itcore_encode {

constexpr size_t kMaxPaste = 32u * 1024u;

struct Buf {
    itcore_buffer abi{};
    std::string bytes;
};

itcore_buffer *make_buf(std::string s) {
    auto *b = new (std::nothrow) Buf;
    if (!b) return nullptr;
    b->bytes = std::move(s);
    b->abi.abi_version = ITCORE_ABI_VERSION;
    b->abi.struct_size = sizeof(itcore_buffer);
    b->abi.data = reinterpret_cast<uint8_t *>(b->bytes.data());
    b->abi.length = b->bytes.size();
    return &b->abi;
}

void release_buf(itcore_buffer *p) {
    if (!p) return;
    // abi 是 Buf 的首成员
    delete reinterpret_cast<Buf *>(p);
}

bool mod(uint32_t m, uint32_t bit) { return (m & bit) != 0; }

int mod_param(uint32_t modifiers) {
    int m = 1;
    if (mod(modifiers, ITCORE_MOD_SHIFT)) m += 1;
    if (mod(modifiers, ITCORE_MOD_ALT)) m += 2;
    if (mod(modifiers, ITCORE_MOD_CTRL)) m += 4;
    if (mod(modifiers, ITCORE_MOD_WIN)) m += 8;
    return m;
}

std::string csi_tilde(int code, uint32_t modifiers) {
    int m = mod_param(modifiers);
    if (m == 1) return "\x1b[" + std::to_string(code) + "~";
    return "\x1b[" + std::to_string(code) + ";" + std::to_string(m) + "~";
}

std::string csi_letter(char letter, uint32_t modifiers, bool app_cursor) {
    int m = mod_param(modifiers);
    if (m == 1) {
        if (app_cursor) return std::string("\x1bO") + letter;
        return std::string("\x1b[") + letter;
    }
    return std::string("\x1b[1;") + std::to_string(m) + letter;
}

std::string ctrl_letter(uint32_t unicode, uint32_t modifiers) {
    char ch = 0;
    bool ok = true;
    if (unicode >= 'a' && unicode <= 'z') ch = static_cast<char>(unicode - 'a' + 1);
    else if (unicode >= 'A' && unicode <= 'Z') ch = static_cast<char>(unicode - 'A' + 1);
    else if (unicode == ' ') ch = 0;
    else if (unicode == '?') ch = 0x7f;
    else ok = false;
    if (!ok) return {};
    std::string out;
    if (mod(modifiers, ITCORE_MOD_ALT)) out.push_back('\x1b');
    out.push_back(ch);
    return out;
}

std::string utf8_cp(uint32_t cp) {
    std::string out;
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string encode_key_impl(const itcore::ScreenModes &modes, const itcore_key_event *ev) {
    const uint32_t vk = ev->vk;
    const uint32_t mods = ev->modifiers;
    const uint32_t uni = ev->unicode;

    auto arrow = [&](char letter) { return csi_letter(letter, mods, modes.app_cursor); };

    switch (vk) {
    case ITCORE_VK_UP: return arrow('A');
    case ITCORE_VK_DOWN: return arrow('B');
    case ITCORE_VK_RIGHT: return arrow('C');
    case ITCORE_VK_LEFT: return arrow('D');
    case ITCORE_VK_HOME: return modes.app_cursor ? csi_letter('H', mods, true) : csi_tilde(1, mods);
    case ITCORE_VK_END: return modes.app_cursor ? csi_letter('F', mods, true) : csi_tilde(4, mods);
    case ITCORE_VK_INSERT: return csi_tilde(2, mods);
    case ITCORE_VK_DELETE: return csi_tilde(3, mods);
    case ITCORE_VK_PRIOR: return csi_tilde(5, mods);
    case ITCORE_VK_NEXT: return csi_tilde(6, mods);
    case ITCORE_VK_BACK:
        return mod(mods, ITCORE_MOD_ALT) ? std::string("\x1b\x7f") : std::string("\x7f");
    case ITCORE_VK_TAB:
        if (mod(mods, ITCORE_MOD_SHIFT)) return "\x1b[Z";
        if (mod(mods, ITCORE_MOD_ALT)) return "\x1b\t";
        return "\t";
    case ITCORE_VK_RETURN:
        return mod(mods, ITCORE_MOD_ALT) ? std::string("\x1b\r") : std::string("\r");
    case ITCORE_VK_ESCAPE: return "\x1b";
    default: break;
    }

    if (vk >= ITCORE_VK_F1 && vk <= ITCORE_VK_F12) {
        static const int codes[] = {11,12,13,14,15,17,18,19,20,21,23,24};
        return csi_tilde(codes[vk - ITCORE_VK_F1], mods);
    }

    if (modes.app_keypad && vk >= ITCORE_VK_NUMPAD0 && vk <= ITCORE_VK_DIVIDE &&
        !mod(mods, ITCORE_MOD_CTRL | ITCORE_MOD_ALT | ITCORE_MOD_SHIFT)) {
        if (vk >= ITCORE_VK_NUMPAD0 && vk <= ITCORE_VK_NUMPAD9)
            return std::string("\x1bO") + char('p' + (vk - ITCORE_VK_NUMPAD0));
        if (vk == ITCORE_VK_MULTIPLY) return "\x1bOj";
        if (vk == ITCORE_VK_ADD) return "\x1bOk";
        if (vk == ITCORE_VK_SUBTRACT) return "\x1bOm";
        if (vk == ITCORE_VK_DECIMAL) return "\x1bOn";
        if (vk == ITCORE_VK_DIVIDE) return "\x1bOo";
    }

    if (uni) {
        if (mod(mods, ITCORE_MOD_CTRL)) {
            auto s = ctrl_letter(uni, mods);
            if (!s.empty()) return s;
        }
        std::string out;
        if (mod(mods, ITCORE_MOD_ALT) && uni < 0x80 && !mod(mods, ITCORE_MOD_CTRL))
            out.push_back('\x1b');
        out += utf8_cp(uni);
        return out;
    }
    return {};
}

bool mouse_active(uint32_t mode) {
    return (mode & (ITCORE_MOUSE_MODE_X10 | ITCORE_MOUSE_MODE_NORMAL |
                    ITCORE_MOUSE_MODE_BUTTON | ITCORE_MOUSE_MODE_ANY)) != 0;
}

std::string encode_mouse_impl(uint32_t mode, const itcore_mouse_event *ev) {
    if (!mouse_active(mode)) return {};
    const bool sgr = (mode & ITCORE_MOUSE_MODE_SGR) != 0;
    const bool any = (mode & ITCORE_MOUSE_MODE_ANY) != 0;
    const bool button_motion = (mode & ITCORE_MOUSE_MODE_BUTTON) != 0;

    uint8_t action = ev->action;
    uint8_t button = ev->button;

    if (action == ITCORE_MOUSE_MOVE) {
        if (any) {
            if (button > 2) button = ITCORE_MBUTTON_NONE;
        } else if (button_motion) {
            if (button > 2) return {};
        } else {
            return {};
        }
    }
    if (action == ITCORE_MOUSE_RELEASE && (mode & ITCORE_MOUSE_MODE_X10)) return {};

    int cb = 0;
    if (action == ITCORE_MOUSE_WHEEL || button == ITCORE_MBUTTON_WHEEL_UP ||
        button == ITCORE_MBUTTON_WHEEL_DOWN) {
        cb = (ev->wheel_delta > 0 || button == ITCORE_MBUTTON_WHEEL_UP) ? 64 : 65;
    } else if (action == ITCORE_MOUSE_RELEASE && sgr) {
        cb = static_cast<int>(button <= 2 ? button : 0);
    } else if (action == ITCORE_MOUSE_RELEASE) {
        cb = 3;
    } else if (button <= 2) {
        cb = button;
    } else if (button == ITCORE_MBUTTON_NONE) {
        cb = 3;
    } else {
        cb = button;
    }
    if (action == ITCORE_MOUSE_MOVE) cb |= 32;
    if (mod(ev->modifiers, ITCORE_MOD_SHIFT)) cb |= 4;
    if (mod(ev->modifiers, ITCORE_MOD_ALT)) cb |= 8;
    if (mod(ev->modifiers, ITCORE_MOD_CTRL)) cb |= 16;

    int col = static_cast<int>(ev->col) + 1;
    int row = static_cast<int>(ev->row) + 1;
    if (col < 1) col = 1;
    if (row < 1) row = 1;

    if (sgr) {
        char final = (action == ITCORE_MOUSE_RELEASE) ? 'm' : 'M';
        return "\x1b[<" + std::to_string(cb) + ";" + std::to_string(col) + ";" +
               std::to_string(row) + final;
    }
    if (col > 223) col = 223;
    if (row > 223) row = 223;
    std::string out = "\x1b[M";
    out.push_back(static_cast<char>(32 + cb));
    out.push_back(static_cast<char>(32 + col));
    out.push_back(static_cast<char>(32 + row));
    return out;
}

int encode_text(itcore::Screen *screen, const char *utf8, size_t len, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!screen || !out) return -1;
    if (!utf8 && len) return -1;
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) if (utf8[i]) s.push_back(utf8[i]);
    *out = make_buf(std::move(s));
    return *out ? 0 : -1;
}

int encode_paste(itcore::Screen *screen, const char *utf8, size_t len, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!screen || !out) return -1;
    if (len > kMaxPaste) return -2;
    std::string body;
    body.reserve(len);
    if (utf8) for (size_t i = 0; i < len; ++i) if (utf8[i]) body.push_back(utf8[i]);
    static const std::string_view marks[] = {"\x1b[200~", "\x1b[201~", "\xc2\x9b" "200~", "\xc2\x9b" "201~"};
    for (bool changed = true; changed;) {
        changed = false;
        for (auto m : marks)
            for (size_t p; (p = body.find(m)) != std::string::npos;) { body.erase(p, m.size()); changed = true; }
    }
    std::string all;
    if (screen->modes().bracketed_paste) {
        all.reserve(body.size() + 16);
        all = "\x1b[200~";
        all += body;
        all += "\x1b[201~";
    } else {
        all = std::move(body);
    }
    *out = make_buf(std::move(all));
    return *out ? 0 : -1;
}

int encode_key(itcore::Screen *screen, const itcore_key_event *ev, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!screen || !ev || !out) return -1;
    if (ev->struct_size && ev->struct_size < sizeof(itcore_key_event)) return -1;
    auto s = encode_key_impl(screen->modes(), ev);
    if (s.empty()) return 1;
    *out = make_buf(std::move(s));
    return *out ? 0 : -1;
}

int encode_mouse(itcore::Screen *screen, const itcore_mouse_event *ev, itcore_buffer **out) {
    if (out) *out = nullptr;
    if (!screen || !ev || !out) return -1;
    if (ev->struct_size && ev->struct_size < sizeof(itcore_mouse_event)) return -1;
    if (!mouse_active(screen->modes().mouse_mode)) return 1;
    auto s = encode_mouse_impl(screen->modes().mouse_mode, ev);
    if (s.empty()) return 1;
    *out = make_buf(std::move(s));
    return *out ? 0 : -1;
}

}  // namespace itcore_encode
