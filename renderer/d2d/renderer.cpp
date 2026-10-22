// SPDX-License-Identifier: GPL-2.0-only
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 非 WIN32 构建：提供可链接的空实现，便于 Linux CI 编译门闸。
#include "renderer.h"
#include <vector>
#include <cstring>

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
    IDWriteTextFormat *format = nullptr;
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
uint32_t theme_fg = 0xFFD4D4D4, theme_bg = 0xFF1E1E1E;

D2D1_COLOR_F color_of(uint32_t c, bool fg) {
    uint8_t tt = (c >> 24) & 0xFF;
    uint32_t rgb = c & 0xFFFFFF;
    if (tt == 0) rgb = (fg ? theme_fg : theme_bg) & 0xFFFFFF;
    // 真彩 / 调色板简化：直接用 RR GG BB
    float r = ((rgb >> 16) & 0xFF) / 255.f;
    float g = ((rgb >> 8) & 0xFF) / 255.f;
    float b = (rgb & 0xFF) / 255.f;
    return D2D1::ColorF(r, g, b, 1.f);
}
}  // namespace

bool D2DRenderer::init(const GridMetrics &metrics, const wchar_t *font_family, float font_size_pt) {
    metrics_ = metrics;
    if (font_family) impl_->family = font_family;
    impl_->size_pt = font_size_pt;
    device_lost_ = false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 2, D3D11_SDK_VERSION, &impl_->d3d, &got, &impl_->ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, 2, D3D11_SDK_VERSION, &impl_->d3d, &got, &impl_->ctx);
    }
    if (FAILED(hr)) return false;

    IDXGIDevice *dxgi_dev = nullptr;
    impl_->d3d->QueryInterface(&dxgi_dev);
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl_->d2d_factory);
    if (FAILED(hr)) { if (dxgi_dev) dxgi_dev->Release(); return false; }
    hr = impl_->d2d_factory->CreateDevice(dxgi_dev, &impl_->d2d_device);
    if (dxgi_dev) dxgi_dev->Release();
    if (FAILED(hr)) return false;
    hr = impl_->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->d2d);
    if (FAILED(hr)) return false;

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
    impl_->swap = host_->create_swap_chain(factory, impl_->d3d, desc);
    if (factory) factory->Release();
    if (!impl_->swap) return false;

    IDXGISurface *surf = nullptr;
    impl_->swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    hr = impl_->d2d->CreateBitmapFromDxgiSurface(surf, &bp, &impl_->target);
    if (surf) surf->Release();
    if (FAILED(hr)) return false;
    impl_->d2d->SetTarget(impl_->target);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(&impl_->dwrite));
    if (FAILED(hr)) return false;
    hr = impl_->dwrite->CreateTextFormat(impl_->family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         impl_->size_pt * (96.f / 72.f), L"en-us", &impl_->format);
    if (FAILED(hr)) return false;
    impl_->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &impl_->brush_bg);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &impl_->brush_fg);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.f), &impl_->brush_caret);
    return true;
}

void D2DRenderer::shutdown() {
#ifdef _WIN32
    auto rel = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
    if (impl_) {
        if (impl_->d2d) impl_->d2d->SetTarget(nullptr);
        rel(impl_->brush_caret); rel(impl_->brush_fg); rel(impl_->brush_bg);
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

bool D2DRenderer::present_frame() {
    if (!impl_ || !impl_->d2d || !impl_->swap || !snap_) return false;
    uint64_t rev = itcore_snapshot_revision(snap_);
    uint64_t gen = itcore_snapshot_generation(snap_);
    // generation 未对齐：仍画旧内容逻辑由上层决定；这里若 gen 不匹配只填背景扩展区
    bool gen_ok = (expected_gen_ == 0 || gen == expected_gen_);

    uint16_t cols = itcore_snapshot_columns(snap_);
    uint16_t rows = itcore_snapshot_rows(snap_);
    if (impl_->row_rev_cache.size() != rows) impl_->row_rev_cache.assign(rows, 0);

    bool dirty = (rev != last_drawn_revision_) || !preedit_.utf8.empty();
    if (!dirty) {
        for (uint16_t y = 0; y < rows; ++y) {
            itcore_wrap w; uint64_t rr;
            itcore_snapshot_row(snap_, y, &w, &rr);
            if (rr != impl_->row_rev_cache[y]) { dirty = true; break; }
        }
    }
    if (!dirty) return false;

    impl_->d2d->BeginDraw();
    impl_->d2d->Clear(color_of(0, false));

    for (uint16_t y = 0; y < rows; ++y) {
        itcore_wrap wrap; uint64_t rr;
        const itcore_cell *cells = itcore_snapshot_row(snap_, y, &wrap, &rr);
        if (!cells) continue;
        impl_->row_rev_cache[y] = rr;

        // 1) 背景段合并
        uint16_t x = 0;
        while (x < cols) {
            if (cells[x].flags & ITCORE_CELL_CONTINUATION) { ++x; continue; }
            uint32_t bg = cells[x].bg;
            uint16_t x1 = x;
            uint8_t w = cells[x].width ? cells[x].width : 1;
            x1 = static_cast<uint16_t>(x + w);
            while (x1 < cols) {
                if (cells[x1].flags & ITCORE_CELL_CONTINUATION) { ++x1; continue; }
                if (cells[x1].bg != bg) break;
                uint8_t ww = cells[x1].width ? cells[x1].width : 1;
                x1 = static_cast<uint16_t>(x1 + ww);
            }
            auto rc = metrics_.cell_rect(x, y, uint8_t(x1 > x ? x1 - x : 1));
            impl_->brush_bg->SetColor(color_of(bg, false));
            impl_->d2d->FillRectangle(D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h)),
                                      impl_->brush_bg);
            x = x1;
        }

        // 2) 文本：每字素簇强制锚在 cell_rect；不使用字形 advance 推进列
        for (uint16_t col = 0; col < cols; ++col) {
            if (cells[col].flags & ITCORE_CELL_CONTINUATION) continue;
            if (!cells[col].grapheme_id) continue;
            size_t glen = 0;
            const char *g = itcore_snapshot_grapheme(snap_, cells[col].grapheme_id, &glen);
            if (!g || !glen) continue;
            uint8_t cw = cells[col].width ? cells[col].width : 1;
            auto rc = metrics_.cell_rect(col, y, cw);
            // UTF-8 → UTF-16
            int wlen = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
            std::wstring ws(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, g, int(glen), ws.data(), wlen);
            uint32_t fg = cells[col].fg;
            if (cells[col].flags & ITCORE_CELL_INVERSE) {
                impl_->brush_fg->SetColor(color_of(cells[col].bg, true));
            } else {
                impl_->brush_fg->SetColor(color_of(fg, true));
            }
            // 裁剪到格，居中偏左；回退字体不改变列起点
            D2D1_RECT_F clip = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
            impl_->d2d->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            impl_->d2d->DrawTextW(ws.c_str(), UINT32(ws.size()), impl_->format, clip, impl_->brush_fg,
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            impl_->d2d->PopAxisAlignedClip();
        }
    }

    // 预编辑覆盖层
    if (!preedit_.utf8.empty()) {
        auto rc = metrics_.cell_rect(preedit_.anchor_col, preedit_.anchor_row, 1);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, preedit_.utf8.data(), int(preedit_.utf8.size()), nullptr, 0);
        std::wstring ws(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, preedit_.utf8.data(), int(preedit_.utf8.size()), ws.data(), wlen);
        D2D1_RECT_F r = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w * 8), float(rc.y + rc.h));
        impl_->brush_fg->SetColor(D2D1::ColorF(D2D1::ColorF::Yellow));
        impl_->d2d->DrawTextW(ws.c_str(), UINT32(ws.size()), impl_->format, r, impl_->brush_fg);
        // 下划线
        impl_->d2d->DrawLine(D2D1::Point2F(r.left, r.bottom - 1), D2D1::Point2F(r.right, r.bottom - 1),
                             impl_->brush_fg, 1.f);
    }

    // 光标
    uint16_t cc, cr; int vis = 0;
    itcore_snapshot_cursor(snap_, &cc, &cr, &vis);
    if (vis) {
        auto rc = metrics_.cell_rect(cc, cr, 1);
        impl_->d2d->FillRectangle(D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + 2), float(rc.y + rc.h)),
                                  impl_->brush_caret);
    }

    // generation 未对齐时的提示带（多出区域已由 Clear 填背景）
    (void)gen_ok;

    HRESULT hr = impl_->d2d->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        device_lost_ = true;
        return false;
    }
    hr = impl_->swap->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        device_lost_ = true;
        return false;
    }
    last_drawn_revision_ = rev;
    return true;
}

#else  // !_WIN32

bool D2DRenderer::init(const GridMetrics &metrics, const wchar_t *, float) {
    metrics_ = metrics;
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

#endif

}  // namespace itren
