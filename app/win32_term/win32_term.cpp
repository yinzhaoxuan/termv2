// T06 最小 Win32 终端实验窗：itcore + ConPtyTransport + itrender_d2d 单窗串联。
// 仅 WIN32 构建。键盘/IME → encode → enqueue；输出 tryPop → feed；定时快照绘制；
// resize：先 itcore_resize(gen) 再 transport.resize(gen)；关闭按规格 close+wait。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include "hwnd_swap_host.h"
#include "renderer.h"
#include "grid_metrics.h"
#include "itcore.h"
#include "conpty_transport.h"

#include <windows.h>
#include <imm.h>
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#  define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace it2w;
using namespace iterm_app;

namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16;
constexpr UINT WM_TERM_OUTPUT = WM_APP + 1;
constexpr UINT WM_TERM_EXITED = WM_APP + 2;

struct TermApp {
    HWND hwnd = nullptr;
    std::unique_ptr<HwndSwapChainHost> host;
    std::unique_ptr<itren::D2DRenderer> renderer;
    itcore_core *core = nullptr;
    std::unique_ptr<ConPtyTransport> transport;
    itren::GridMetrics metrics{};
    uint16_t cols = 80, rows = 24;
    uint64_t generation = 0;
    itcore_snapshot *snap = nullptr;
    bool skip_next_wm_char = false;
    bool closing = false;
    bool started = false;
};

TermApp *g_app = nullptr;

uint32_t mods_from_keys() {
    uint32_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ITCORE_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ITCORE_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ITCORE_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= ITCORE_MOD_WIN;
    return m;
}

void release_snap(TermApp *a) {
    if (a->snap) {
        itcore_snapshot_release(a->snap);
        a->snap = nullptr;
        if (a->renderer) a->renderer->set_snapshot(nullptr);
    }
}

void drain_events(TermApp *a) {
    if (!a->core || !a->transport) return;
    while (itcore_event *e = itcore_next_event(a->core)) {
        if (itcore_event_type(e) == ITCORE_EV_REPLY_BYTES) {
            size_t n = 0;
            const uint8_t *p = itcore_event_data(e, &n);
            if (p && n) a->transport->enqueueReply(std::string_view(reinterpret_cast<const char *>(p), n));
        }
        itcore_event_release(e);
    }
}

void drain_output(TermApp *a) {
    if (!a->transport || !a->core) return;
    while (auto chunk = a->transport->tryPopOutput()) {
        const auto &b = chunk->bytes;
        size_t off = 0;
        while (off < b.size()) {
            auto fr = itcore_feed(a->core, reinterpret_cast<const uint8_t *>(b.data() + off), b.size() - off);
            off += fr.accepted ? fr.accepted : b.size() - off;
            drain_events(a);
            if (fr.status == ITCORE_FATAL) break;
        }
    }
    drain_events(a);
}

void present(TermApp *a) {
    if (!a->renderer || !a->core) return;
    release_snap(a);
    a->snap = itcore_acquire_snapshot(a->core, 0, a->rows);
    if (!a->snap) return;
    a->renderer->set_expected_generation(a->generation);
    a->renderer->set_snapshot(a->snap);
    if (a->renderer->device_lost()) {
        // 最小实验：设备丢失仅记日志，实机再做重建
        std::fprintf(stderr, "[win32_term] D3D device lost\n");
        return;
    }
    a->renderer->present_frame();
}

void apply_resize(TermApp *a, int client_w, int client_h) {
    if (!a->core || !a->renderer) return;
    a->metrics.dpi_scale = a->host ? a->host->dpi_scale() : 1.f;
    a->renderer->metrics() = a->metrics;
    uint16_t nc = a->cols, nr = a->rows;
    a->metrics.grid_size(client_w, client_h, nc, nr);
    if (nc == a->cols && nr == a->rows && a->started) {
        // 仅像素变化：仍要 ResizeBuffers
        a->renderer->resize_surface();
        return;
    }
    a->cols = nc;
    a->rows = nr;
    ++a->generation;
    // 规格：先 itcore_resize，再 transport.resize，同 generation
    itcore_resize(a->core, a->cols, a->rows, a->generation);
    if (a->transport && a->started) {
        a->transport->resize(GridSize{a->cols, a->rows}, a->generation);
    }
    a->renderer->set_expected_generation(a->generation);
    a->renderer->resize_surface();
    release_snap(a);
}

bool send_encoded(TermApp *a, itcore_buffer *buf) {
    if (!buf || !a->transport) {
        if (buf) itcore_buffer_release(buf);
        return false;
    }
    if (buf->data && buf->length) {
        auto r = a->transport->enqueueUserInput(
            std::string_view(reinterpret_cast<const char *>(buf->data), buf->length));
        if (r != EnqueueResult::Ok)
            std::fprintf(stderr, "[win32_term] enqueueUserInput %d\n", int(r));
    }
    itcore_buffer_release(buf);
    return true;
}

bool submit_utf8(TermApp *a, const char *utf8, size_t len) {
    if (!a->core || !utf8 || !len) return false;
    itcore_buffer *buf = nullptr;
    if (itcore_encode_text(a->core, utf8, len, &buf) != 0) return false;
    return send_encoded(a, buf);
}

bool submit_key(TermApp *a, uint32_t vk, uint32_t unicode, int is_repeat) {
    if (!a->core) return false;
    itcore_key_event ev{};
    ev.struct_size = sizeof(ev);
    ev.vk = vk;
    ev.modifiers = mods_from_keys();
    ev.is_repeat = is_repeat;
    ev.unicode = unicode;
    itcore_buffer *buf = nullptr;
    if (itcore_encode_key(a->core, &ev, &buf) != 0) return false;
    return send_encoded(a, buf);
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool start_session(TermApp *a) {
    itcore_options opt{};
    opt.abi_version = ITCORE_ABI_VERSION;
    opt.struct_size = sizeof(opt);
    opt.columns = a->cols;
    opt.rows = a->rows;
    opt.scrollback_lines = 5000;
    opt.memory_limit_bytes = 32ull << 20;
    if (itcore_create(&opt, &a->core) != 0 || !a->core) {
        std::fprintf(stderr, "[win32_term] itcore_create 失败\n");
        return false;
    }
    a->generation = 1;
    itcore_resize(a->core, a->cols, a->rows, a->generation);

    a->host = std::make_unique<HwndSwapChainHost>(a->hwnd);
    a->metrics = itren::GridMetrics::from_font(/*advance*/ 7.2f, /*ascent*/ 10.f, /*descent*/ 3.f,
                                               /*gap*/ 0.f, a->host->dpi_scale());
    a->renderer = std::make_unique<itren::D2DRenderer>(a->host.get());
    if (!a->renderer->init(a->metrics, L"Consolas", 12.f)) {
        std::fprintf(stderr, "[win32_term] D2DRenderer::init 失败\n");
        return false;
    }
    a->renderer->set_expected_generation(a->generation);

    TransportCallbacks cb;
    cb.onStateChanged = [](SessionId, AttemptId, TransportState s, std::optional<TransportFault> f) {
        std::fprintf(stderr, "[win32_term] transport state %d%s%d\n", int(s), f ? " fault=" : "",
                     f ? int(*f) : -1);
    };
    cb.onOutputAvailable = [](SessionId, AttemptId) {
        if (g_app && g_app->hwnd) PostMessageW(g_app->hwnd, WM_TERM_OUTPUT, 0, 0);
    };
    cb.onExited = [](SessionId, AttemptId, uint32_t code) {
        std::fprintf(stderr, "[win32_term] shell exited %u\n", code);
        if (g_app && g_app->hwnd) PostMessageW(g_app->hwnd, WM_TERM_EXITED, code, 0);
    };
    cb.onDiagnostic = [](SessionId, AttemptId, const DiagRecord &d) {
        std::fprintf(stderr, "[win32_term] diag %s %llums %s\n", d.stage.c_str(),
                     (unsigned long long)d.elapsedMs, d.detail.c_str());
    };
    a->transport = std::make_unique<ConPtyTransport>(cb);

    LaunchSpec spec;
    spec.executable = L"C:\\Windows\\System32\\cmd.exe";
    spec.initialSize = {a->cols, a->rows};
    if (wchar_t *env = GetEnvironmentStringsW()) {
        for (wchar_t *p = env; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            auto eq = kv.find(L'=', 1);
            if (eq != std::wstring::npos)
                spec.environment.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        FreeEnvironmentStringsW(env);
    }
    if (!a->transport->start(SessionId{1}, AttemptId{1}, spec)) {
        std::fprintf(stderr, "[win32_term] ConPTY start 失败\n");
        return false;
    }
    a->started = true;
    SetTimer(a->hwnd, kTimerId, kTimerMs, nullptr);
    return true;
}

void shutdown_session(TermApp *a) {
    if (a->closing) return;
    a->closing = true;
    KillTimer(a->hwnd, kTimerId);
    if (a->transport) {
        ULONGLONG t0 = GetTickCount64();
        a->transport->close(CloseReason::User);
        a->transport->waitClosed(3000);
        std::fprintf(stderr, "[win32_term] close 耗时 %llums\n",
                     (unsigned long long)(GetTickCount64() - t0));
        // 排空残留输出再销毁核心
        drain_output(a);
    }
    release_snap(a);
    if (a->renderer) {
        a->renderer->shutdown();
        a->renderer.reset();
    }
    a->host.reset();
    a->transport.reset();
    if (a->core) {
        itcore_finish_input(a->core);
        itcore_destroy(a->core);
        a->core = nullptr;
    }
    a->started = false;
}

bool is_char_key(WPARAM vk) {
    // 会走 WM_CHAR 的键，避免 KEYDOWN 重复编码
    if (vk == VK_BACK || vk == VK_TAB || vk == VK_RETURN || vk == VK_ESCAPE) return false;
    if (vk >= VK_PRIOR && vk <= VK_DOWN) return false;  // nav
    if (vk >= VK_F1 && vk <= VK_F24) return false;
    if (vk == VK_INSERT || vk == VK_DELETE) return false;
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) return false;
    // 可打印 / 组合键留给 WM_CHAR；Ctrl 组合仍可能只有 KEYDOWN
    if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        // Ctrl+字母等：encode_key 处理
        return false;
    }
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TermApp *a = g_app;
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        a = static_cast<TermApp *>(cs->lpCreateParams);
        g_app = a;
        a->hwnd = hwnd;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        // 先用默认格度量估行列，再 start
        a->metrics = itren::GridMetrics::from_font(7.2f, 10.f, 3.f, 0.f, 1.f);
        a->metrics.grid_size(rc.right - rc.left, rc.bottom - rc.top, a->cols, a->rows);
        if (!start_session(a)) {
            PostQuitMessage(1);
            return -1;
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerId && a && !a->closing) {
            drain_output(a);
            present(a);
        }
        return 0;
    case WM_TERM_OUTPUT:
        if (a && !a->closing) {
            drain_output(a);
            present(a);
        }
        return 0;
    case WM_TERM_EXITED:
        if (a && !a->closing) DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        if (a && a->started && wParam != SIZE_MINIMIZED) {
            apply_resize(a, LOWORD(lParam), HIWORD(lParam));
            present(a);
        }
        return 0;
    case WM_DPICHANGED: {
        if (!a) break;
        auto *r = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        apply_resize(a, rc.right - rc.left, rc.bottom - rc.top);
        present(a);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!a || a->closing) return 0;
        if (!is_char_key(wParam)) {
            submit_key(a, uint32_t(wParam), 0, (lParam & (1 << 30)) ? 1 : 0);
        }
        return 0;
    case WM_CHAR:
        if (!a || a->closing) return 0;
        if (a->skip_next_wm_char) {
            a->skip_next_wm_char = false;
            return 0;
        }
        if (wParam >= 0x20 || wParam == 0x09 /*tab via char rare*/ || wParam == 0x0D ||
            wParam == 0x08 || wParam == 0x1B) {
            // 控制字符走 encode_key 更稳；可打印走 text
            if (wParam < 0x20 || wParam == 0x7F) {
                submit_key(a, 0, uint32_t(wParam), 0);
            } else {
                wchar_t wc = wchar_t(wParam);
                auto u8 = wide_to_utf8(std::wstring_view(&wc, 1));
                if (!u8.empty()) submit_utf8(a, u8.data(), u8.size());
            }
        }
        return 0;
    case WM_IME_COMPOSITION:
        if (!a || !a->renderer) break;
        {
            HIMC imc = ImmGetContext(hwnd);
            if (!imc) break;
            if (lParam & GCS_RESULTSTR) {
                LONG nbytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
                if (nbytes > 0) {
                    std::wstring w(size_t(nbytes / sizeof(wchar_t)), L'\0');
                    ImmGetCompositionStringW(imc, GCS_RESULTSTR, w.data(), nbytes);
                    auto u8 = wide_to_utf8(w);
                    if (!u8.empty()) {
                        submit_utf8(a, u8.data(), u8.size());
                        a->skip_next_wm_char = true;
                    }
                }
                a->renderer->clear_preedit();
            } else if (lParam & GCS_COMPSTR) {
                LONG nbytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                itren::Preedit pe;
                if (nbytes > 0) {
                    std::wstring w(size_t(nbytes / sizeof(wchar_t)), L'\0');
                    ImmGetCompositionStringW(imc, GCS_COMPSTR, w.data(), nbytes);
                    pe.utf8 = wide_to_utf8(w);
                }
                if (a->snap) {
                    int vis = 0;
                    itcore_snapshot_cursor(a->snap, &pe.anchor_col, &pe.anchor_row, &vis);
                }
                a->renderer->set_preedit(pe);
                present(a);
            }
            ImmReleaseContext(hwnd, imc);
        }
        return 0;
    case WM_IME_ENDCOMPOSITION:
        if (a && a->renderer) a->renderer->clear_preedit();
        break;
    case WM_IME_REQUEST:
        // 候选窗位置：供 IMM 查询
        if (wParam == IMR_QUERYCHARPOSITION && a && a->renderer) {
            auto *q = reinterpret_cast<IMECHARPOSITION *>(lParam);
            auto r = a->renderer->caret_rect_px();
            POINT pt{r.x, r.y};
            ClientToScreen(hwnd, &pt);
            q->pt.x = pt.x;
            q->pt.y = pt.y;
            q->cLineHeight = r.h;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&rc), 2);
            q->rcDocument = rc;
            return 1;
        }
        break;
    case WM_SETFOCUS:
        if (a) {
            // 通知 IMM 候选窗跟随
            HIMC imc = ImmGetContext(hwnd);
            if (imc) {
                COMPOSITIONFORM cf{};
                cf.dwStyle = CFS_POINT;
                if (a->renderer) {
                    auto r = a->renderer->caret_rect_px();
                    cf.ptCurrentPos = {r.x, r.y + r.h};
                }
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(hwnd, imc);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (a && !a->closing) {
            drain_output(a);
            present(a);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (a) shutdown_session(a);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (a && !a->closing) shutdown_session(a);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    // Per-Monitor V2（若可用）
    using SetDpiAw = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
    if (auto raw = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) {
        auto fn = reinterpret_cast<SetDpiAw>(reinterpret_cast<void *>(raw));
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"iterm2_win32_term";
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "[win32_term] RegisterClassEx 失败\n");
        return 1;
    }

    TermApp app;
    g_app = &app;
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"iterm2-win T06 win32_term",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
        nullptr, nullptr, hi, &app);
    if (!hwnd) {
        std::fprintf(stderr, "[win32_term] CreateWindowEx 失败\n");
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app = nullptr;
    return int(msg.wParam);
}

