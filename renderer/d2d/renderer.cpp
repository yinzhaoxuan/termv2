// SPDX-License-Identifier: GPL-2.0-only
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 非 WIN32 构建：提供可链接的空实现，便于 Linux CI 编译门闸。
#include "renderer.h"
#include "cell_style.h"
#include "frame_policy.h"
#include <vector>
#include <cstring>
#include <cstdio>

namespace itren {
const char *device_kind_name(DeviceKind k) {
    switch (k) { case DeviceKind::Hardware: return "hardware"; case DeviceKind::Warp: return "warp"; case DeviceKind::Stub: return "stub"; default: return "none"; }
}
}  // namespace itren

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <d2d1_1.h>
#  include <d2d1.h>
#  include <dwrite.h>
#  include <dxgi1_2.h>
#  include <d3d11.h>
#  pragma comment(lib, "d2d1")
#  pragma comment(lib, "dwrite")
#  pragma comment(lib, "d3d11")
#  pragma comment(lib, "dxgi")
#endif

namespace itren {

struct D2DRenderer::Impl {
#ifdef _WIN32
    ID3D11Device *d3d = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain1 *swap = nullptr;
    ID2D1Factory1 *d2d_factory = nullptr;
    ID2D1Device *d2d_device = nullptr;
    ID2D1DeviceContext *d2d = nullptr;
    ID2D1Bitmap1 *target = nullptr;
    IDWriteFactory *dwrite = nullptr;
    IDWriteTextFormat *format = nullptr;          // = formats[0]
    IDWriteTextFormat *formats[4] = {};           // [bold|italic<<1]
    ID2D1Bitmap1 *canvas = nullptr;               // 持久网格画布：只重画脏行，每帧整体贴到后缓冲
    Theme theme;
    DirtyRows dirty;
    LayoutKeeper layout;
    uint16_t last_cc = 0xFFFF, last_cr = 0xFFFF; int last_vis = -1; std::string last_preedit;
    GridMetrics saved_metrics{};
    ID2D1SolidColorBrush *brush_bg = nullptr;
    ID2D1SolidColorBrush *brush_fg = nullptr;
    ID2D1SolidColorBrush *brush_caret = nullptr;
    std::wstring family = L"Consolas";
    float size_pt = 12.f;
    std::vector<uint64_t> row_rev_cache;
#endif
};

D2DRenderer::D2DRenderer(ISwapChainHost *host) : host_(host), impl_(std::make_unique<Impl>()) {}
D2DRenderer::~D2DRenderer() { shutdown(); }

#ifdef _WIN32
namespace {
D2D1_COLOR_F rgb_f(uint32_t rgb) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, 1.f);
}
void log_hr(const char *step, HRESULT hr) {
    std::fprintf(stderr, "[itren] %s 失败 HRESULT=0x%08lX\n", step, (unsigned long)hr);
}
bool fail_hr(const char *step, HRESULT hr) { log_hr(step, hr); return false; }
}  // namespace

bool D2DRenderer::init(const GridMetrics &metrics, const wchar_t *font_family, float font_size_pt) {
    metrics_ = metrics;
    if (font_family) impl_->family = font_family;
    impl_->size_pt = font_size_pt;
    device_lost_ = false;
    device_kind_ = DeviceKind::None;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 2, D3D11_SDK_VERSION, &impl_->d3d, &got, &impl_->ctx);
    DeviceKind kind = DeviceKind::Hardware;
    if (FAILED(hr)) {
        log_hr("D3D11CreateDevice(HARDWARE)，改用 WARP", hr);
        kind = DeviceKind::Warp;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, 2, D3D11_SDK_VERSION, &impl_->d3d, &got, &impl_->ctx);
    }
    if (FAILED(hr)) return fail_hr("D3D11CreateDevice(WARP)", hr);
    if (kind == DeviceKind::Warp) std::fprintf(stderr, "[itren] 使用 WARP 软件渲染\n");

    IDXGIDevice *dxgi_dev = nullptr;
    impl_->d3d->QueryInterface(&dxgi_dev);
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl_->d2d_factory);
    if (FAILED(hr)) { if (dxgi_dev) dxgi_dev->Release(); return fail_hr("D2D1CreateFactory", hr); }
    hr = impl_->d2d_factory->CreateDevice(dxgi_dev, &impl_->d2d_device);
    if (dxgi_dev) dxgi_dev->Release();
    if (FAILED(hr)) return fail_hr("ID2D1Factory1::CreateDevice", hr);
    hr = impl_->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->d2d);
    if (FAILED(hr)) return fail_hr("CreateDeviceContext", hr);

    IDXGIFactory2 *factory = nullptr;
    {
        IDXGIDevice *d = nullptr; impl_->d3d->QueryInterface(&d);
        IDXGIAdapter *ad = nullptr; d->GetAdapter(&ad);
        ad->GetParent(IID_PPV_ARGS(&factory));
        ad->Release(); d->Release();
    }
    auto sz = host_->client_size_px();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = sz.w > 0 ? sz.w : 1;
    desc.Height = sz.h > 0 ? sz.h : 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (!factory) std::fprintf(stderr, "[itren] init：取 IDXGIFactory2 失败\n");
    impl_->swap = host_->create_swap_chain(factory, impl_->d3d, desc);
    if (factory) factory->Release();
    if (!impl_->swap) { std::fprintf(stderr, "[itren] init 失败：host->create_swap_chain 返回 nullptr\n"); return false; }

    IDXGISurface *surf = nullptr;
    impl_->swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    hr = impl_->d2d->CreateBitmapFromDxgiSurface(surf, &bp, &impl_->target);
    if (surf) surf->Release();
    if (FAILED(hr)) return fail_hr("CreateBitmapFromDxgiSurface", hr);
    impl_->d2d->SetTarget(impl_->target);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(&impl_->dwrite));
    if (FAILED(hr)) return fail_hr("DWriteCreateFactory", hr);
    hr = impl_->dwrite->CreateTextFormat(impl_->family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         impl_->size_pt * (96.f / 72.f), L"en-us", &impl_->format);
    if (FAILED(hr)) return fail_hr("CreateTextFormat(regular)", hr);
    impl_->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    impl_->formats[0] = impl_->format; impl_->format->AddRef();
    for (int i = 1; i < 4; ++i) {
        hr = impl_->dwrite->CreateTextFormat(impl_->family.c_str(), nullptr,
                                             (i & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                             (i & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, impl_->size_pt * (96.f / 72.f), L"en-us",
                                             &impl_->formats[i]);
        if (FAILED(hr)) return fail_hr("CreateTextFormat(bold/italic)", hr);
        impl_->formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    device_kind_ = kind;
    impl_->saved_metrics = metrics_;
    impl_->dirty.invalidate(); impl_->layout.reset(); impl_->last_vis = -1;

    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &impl_->brush_bg);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &impl_->brush_fg);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.f), &impl_->brush_caret);
    return true;
}

void D2DRenderer::shutdown() {
    device_kind_ = DeviceKind::None;
#ifdef _WIN32
    auto rel = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
    if (impl_) {
        if (impl_->d2d) impl_->d2d->SetTarget(nullptr);
        rel(impl_->brush_caret); rel(impl_->brush_fg); rel(impl_->brush_bg);
        for (auto *&f : impl_->formats) rel(f);
        rel(impl_->canvas);
        rel(impl_->format); rel(impl_->dwrite);
        rel(impl_->target); rel(impl_->d2d); rel(impl_->d2d_device); rel(impl_->d2d_factory);
        rel(impl_->swap); rel(impl_->ctx); rel(impl_->d3d);
        impl_->row_rev_cache.clear();
    }
#endif
    snap_ = nullptr;
}

void D2DRenderer::set_snapshot(const itcore_snapshot *snap) { snap_ = snap; }

bool D2DRenderer::resize_surface() {
#ifdef _WIN32
    if (!impl_ || !impl_->swap || !impl_->d2d || !host_) return false;
    auto sz = host_->client_size_px();
    UINT w = sz.w > 0 ? UINT(sz.w) : 1;
    UINT h = sz.h > 0 ? UINT(sz.h) : 1;
    impl_->d2d->SetTarget(nullptr);
    if (impl_->target) { impl_->target->Release(); impl_->target = nullptr; }
    if (impl_->canvas) { impl_->canvas->Release(); impl_->canvas = nullptr; }
    impl_->dirty.invalidate();
    HRESULT hr = impl_->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) device_lost_ = true;
        return false;
    }
    IDXGISurface *surf = nullptr;
    hr = impl_->swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    if (FAILED(hr)) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    hr = impl_->d2d->CreateBitmapFromDxgiSurface(surf, &bp, &impl_->target);
    surf->Release();
    if (FAILED(hr)) return false;
    impl_->d2d->SetTarget(impl_->target);
    last_drawn_revision_ = 0;  // 强制整帧重绘
    return true;
#else
    return true;
#endif
}

GridMetrics::Rect D2DRenderer::caret_rect_px() const {
    uint16_t col = 0, row = 0; int vis = 0;
    if (snap_) itcore_snapshot_cursor(snap_, &col, &row, &vis);
    if (!preedit_.utf8.empty()) { col = preedit_.anchor_col; row = preedit_.anchor_row; }
    return metrics_.cell_rect(col, row, 1);
}

bool D2DRenderer::recover_device() {
    // 评审第 4 点：设备丢失后整套 D3D/D2D/DWrite 资源重建，交换链经 host->create_swap_chain 重新创建
    GridMetrics m = impl_->saved_metrics;
    std::wstring fam = impl_->family; float pt = impl_->size_pt;
    shutdown_device();
    return init(m, fam.c_str(), pt);
}

void D2DRenderer::shutdown_device() {
    const itcore_snapshot *keep = snap_;
    shutdown();
    snap_ = keep;
}

namespace {
bool is_lost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}
}  // namespace

bool D2DRenderer::present_frame() {
    if (device_lost_ && !recover_device()) return false;
    if (!impl_ || !impl_->d2d || !impl_->swap || !impl_->target || !snap_) return false;
    auto &I = *impl_;

    auto L = I.layout.update(itcore_snapshot_columns(snap_), itcore_snapshot_rows(snap_),
                             itcore_snapshot_generation(snap_), expected_gen_);
    if (!I.canvas) {
        D2D1_SIZE_U px = I.target->GetPixelSize();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(I.d2d->CreateBitmap(px, nullptr, 0, &bp, &I.canvas))) return false;
        I.dirty.invalidate();
    }
    bool full = L.layout_changed;
    if (full) I.dirty.invalidate();
    std::vector<uint16_t> rows = I.dirty.collect(snap_);  // 评审第 1 点：只画脏行

    uint16_t cc = 0, cr = 0; int vis = 0;
    itcore_snapshot_cursor(snap_, &cc, &cr, &vis);
    bool overlay_changed = cc != I.last_cc || cr != I.last_cr || vis != I.last_vis || preedit_.utf8 != I.last_preedit;
    if (!full && rows.empty() && !overlay_changed) return false;

    I.d2d->SetTarget(I.canvas);
    I.d2d->BeginDraw();
    const uint32_t theme_bg = I.theme.bg;
    if (full) I.d2d->Clear(rgb_f(theme_bg));  // 评审第 5 点：布局外区域一律背景色
    for (uint16_t y : rows) {
        if (y >= L.rows) continue;
        auto row_rc = metrics_.cell_rect(0, y, 1);
        // 行先整体填主题背景（含 draw_cols 之外到右边缘）
        D2D1_SIZE_F csz = I.canvas->GetSize();
        I.brush_bg->SetColor(rgb_f(theme_bg));
        I.d2d->FillRectangle(D2D1::RectF(0.f, float(row_rc.y), csz.width, float(row_rc.y + row_rc.h)), I.brush_bg);
        if (y >= L.draw_rows) continue;
        itcore_wrap wrap; uint64_t rr;
        const itcore_cell *cells = itcore_snapshot_row(snap_, y, &wrap, &rr);
        if (!cells) continue;
        const uint16_t cols = L.draw_cols;
        // 背景段合并（用 resolve_cell 的 bg，INVERSE 已互换）
        uint16_t x = 0;
        while (x < cols) {
            if (cells[x].flags & ITCORE_CELL_CONTINUATION) { ++x; continue; }
            uint32_t bg = resolve_cell(I.theme, cells[x]).bg;
            uint16_t x1 = uint16_t(x + (cells[x].width ? cells[x].width : 1));
            while (x1 < cols) {
                if (cells[x1].flags & ITCORE_CELL_CONTINUATION) { ++x1; continue; }
                if (resolve_cell(I.theme, cells[x1]).bg != bg) break;
                x1 = uint16_t(x1 + (cells[x1].width ? cells[x1].width : 1));
            }
            if (x1 > cols) x1 = cols;
            auto rc = metrics_.cell_rect(x, y, uint8_t(x1 > x ? (x1 - x > 255 ? 255 : x1 - x) : 1));
            if (x1 - x > 255) rc.w = metrics_.cell_w_px() * (x1 - x);
            I.brush_bg->SetColor(rgb_f(bg));
            I.d2d->FillRectangle(D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h)), I.brush_bg);
            x = x1;
        }
        // 文本 + 粗体/斜体/下划线（评审第 3 点）
        for (uint16_t col = 0; col < cols; ++col) {
            const itcore_cell &c = cells[col];
            if (c.flags & ITCORE_CELL_CONTINUATION) continue;
            auto rcol = resolve_cell(I.theme, c);
            auto st = text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
            uint8_t cw = c.width ? c.width : 1;
            auto rc = metrics_.cell_rect(col, y, cw);
            D2D1_RECT_F clip = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
            if (c.grapheme_id) {
                size_t glen = 0;
                const char *g = itcore_snapshot_grapheme(snap_, c.grapheme_id, &glen);
                if (g && glen) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
                    std::wstring ws(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, g, int(glen), ws.data(), wlen);
                    I.brush_fg->SetColor(rgb_f(rcol.fg));
                    I.d2d->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                    I.d2d->DrawTextW(ws.c_str(), UINT32(ws.size()), I.formats[st.format_index()], clip, I.brush_fg,
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                    I.d2d->PopAxisAlignedClip();
                }
            }
            if (st.underline) {
                float uy = clip.bottom - 1.5f;
                I.brush_fg->SetColor(rgb_f(rcol.underline));
                I.d2d->DrawLine(D2D1::Point2F(clip.left, uy), D2D1::Point2F(clip.right, uy), I.brush_fg, 1.f);
            }
        }
    }
    HRESULT hr = I.d2d->EndDraw();
    if (is_lost(hr)) { device_lost_ = true; return false; }

    // 后缓冲：贴画布 + 预编辑 + 光标（覆盖层每帧重画，不污染画布）
    I.d2d->SetTarget(I.target);
    I.d2d->BeginDraw();
    I.d2d->DrawImage(I.canvas);
    if (!preedit_.utf8.empty()) {
        auto rc = metrics_.cell_rect(preedit_.anchor_col, preedit_.anchor_row, 1);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, preedit_.utf8.data(), int(preedit_.utf8.size()), nullptr, 0);
        std::wstring ws(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, preedit_.utf8.data(), int(preedit_.utf8.size()), ws.data(), wlen);
        D2D1_RECT_F r = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w * 8), float(rc.y + rc.h));
        I.brush_bg->SetColor(rgb_f(theme_bg));
        I.d2d->FillRectangle(r, I.brush_bg);
        I.brush_fg->SetColor(D2D1::ColorF(D2D1::ColorF::Yellow));
        I.d2d->DrawTextW(ws.c_str(), UINT32(ws.size()), I.formats[0], r, I.brush_fg);
        I.d2d->DrawLine(D2D1::Point2F(r.left, r.bottom - 1), D2D1::Point2F(r.right, r.bottom - 1), I.brush_fg, 1.f);
    }
    if (vis && cc < L.cols && cr < L.rows) {
        auto rc = metrics_.cell_rect(cc, cr, 1);
        I.d2d->FillRectangle(D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + 2), float(rc.y + rc.h)), I.brush_caret);
    }
    hr = I.d2d->EndDraw();
    if (is_lost(hr)) { device_lost_ = true; return false; }
    hr = I.swap->Present(1, 0);
    if (is_lost(hr)) { device_lost_ = true; return false; }
    I.last_cc = cc; I.last_cr = cr; I.last_vis = vis; I.last_preedit = preedit_.utf8;
    last_drawn_revision_ = itcore_snapshot_revision(snap_);
    return true;
}

#else  // !_WIN32

bool D2DRenderer::init(const GridMetrics &metrics, const wchar_t *, float) {
    metrics_ = metrics;
    device_kind_ = DeviceKind::Stub;
    return true;  // 编译门闸：逻辑路径可测 caret_rect
}
void D2DRenderer::shutdown() { snap_ = nullptr; }
void D2DRenderer::set_snapshot(const itcore_snapshot *snap) { snap_ = snap; }
bool D2DRenderer::resize_surface() { return true; }
GridMetrics::Rect D2DRenderer::caret_rect_px() const {
    uint16_t col = 0, row = 0; int vis = 0;
    if (snap_) itcore_snapshot_cursor(snap_, &col, &row, &vis);
    if (!preedit_.utf8.empty()) { col = preedit_.anchor_col; row = preedit_.anchor_row; }
    return metrics_.cell_rect(col, row, 1);
}
bool D2DRenderer::present_frame() { return false; }
bool D2DRenderer::recover_device() { device_lost_ = false; return true; }
void D2DRenderer::shutdown_device() {}

#endif

}  // namespace itren
