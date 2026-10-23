// SPDX-License-Identifier: GPL-2.0-only
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 非 WIN32 构建：提供可链接的空实现，便于 Linux CI 编译门闸。
#include "renderer.h"
#include "cell_style.h"
#include "frame_policy.h"
#include "cursor_geometry.h"
#include "glyph_cache.h"
#include "ime_layout.h"
#include <unordered_map>
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
    mutable FontMetricsDip fm_cache; mutable bool fm_valid = false;  // font_metrics 缓存（create_formats 时失效）
    float fmt_scale = 0.f; int fmt_cell_h = 0; float fmt_baseline = 0.f;  // TextFormat 当前对应的 DPI/行高/基线
    std::vector<uint64_t> row_rev_cache;
    std::unordered_map<std::wstring, float> advance[4];  // 字形 advance 缓存（按样式），字体变化时清空
    int last_reverse = -1;
    bool last_blink_on = true;
    BlinkRows blink_rows;                         // 含 SGR 5 格的行（相位翻转只重画这些行）
    bool last_cursor_on = true; int last_cursor_style = -1; bool last_focused = true;
#endif
};

D2DRenderer::D2DRenderer(ISwapChainHost *host) : host_(host), impl_(std::make_unique<Impl>()) {}
D2DRenderer::~D2DRenderer() { shutdown(); }

#ifdef _WIN32
static bool row_has_text(const itcore_snapshot *s, uint16_t row, uint16_t col) {
    itcore_wrap w; uint64_t rr;
    const itcore_cell *r = itcore_snapshot_row(s, row, &w, &rr);
    return r && col < itcore_snapshot_columns(s) && r[col].grapheme_id && !(r[col].flags & ITCORE_CELL_CONTINUATION);
}
#endif
// 预编辑 → ime_layout：簇与列宽来自核心当前屏幕（fill_preedit_clusters），与上屏后网格内宽度一致
void fill_preedit_clusters(Preedit &p, const itcore_core *c) {
    size_t n = itcore_text_clusters(c, p.utf8.data(), p.utf8.size(), nullptr, nullptr, 0);
    p.widths.assign(n, 1); p.byte_lens.assign(n, 0);
    if (n) itcore_text_clusters(c, p.utf8.data(), p.utf8.size(), p.widths.data(), p.byte_lens.data(), n);
}
static std::vector<uint32_t> codepoint_lens(const std::string &u) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < u.size();) {
        size_t j = i + 1;
        while (j < u.size() && (static_cast<unsigned char>(u[j]) & 0xC0) == 0x80) ++j;
        out.push_back(uint32_t(j - i)); i = j;
    }
    return out;
}
std::vector<uint8_t> preedit_widths(const Preedit &p) {
    if (!p.widths.empty()) return p.widths;
    return std::vector<uint8_t>(codepoint_lens(p.utf8).size(), 1);
}
std::vector<std::string> preedit_clusters(const Preedit &p) {
    const std::string &u = p.utf8;
    std::vector<uint32_t> bl = p.byte_lens.empty() ? codepoint_lens(u) : p.byte_lens;
    std::vector<std::string> out; size_t off = 0;
    for (uint32_t b : bl) { if (off >= u.size()) break; out.emplace_back(u, off, std::min<size_t>(b, u.size() - off)); off += b; }
    return out;
}
ImeLayout preedit_layout(const GridMetrics &m, const Preedit &p, uint16_t cols, uint16_t rows) {
    auto widths = preedit_widths(p);
    std::vector<ClauseAttr> attrs;
    for (uint8_t a : p.attrs) attrs.push_back(ClauseAttr(a > 2 ? 0 : a));
    size_t cur = p.cursor == SIZE_MAX ? widths.size() : std::min(p.cursor, widths.size());
    return ime_layout(m, p.anchor_row, p.anchor_col, widths, attrs, cur, cols, rows);
}

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

// 4 个样式 TextFormat 全部建成才替换 Impl 里的旧格式（失败保持旧字体）
bool D2DRenderer::create_formats(Impl &I, const std::wstring &family, float size_pt, float scale, int cell_h_px, float baseline_px) {
    if (!I.dwrite) return false;
    IDWriteTextFormat *f[4] = {};
    for (int i = 0; i < 4; ++i) {
        HRESULT hr = I.dwrite->CreateTextFormat(family.c_str(), nullptr,
                                                (i & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                (i & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size_pt * (96.f / 72.f) * (scale > 0 ? scale : 1.f), L"en-us", &f[i]);
        if (FAILED(hr)) {
            for (auto *&x : f) if (x) { x->Release(); x = nullptr; }
            return fail_hr("CreateTextFormat", hr);
        }
        f[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // 画布按物理像素绘制：字号已乘 DPI；固定行高=格高、基线=round(ascent×scale)，回退字体不再把基线挤出格子（BUG-01）
        if (cell_h_px > 0) f[i]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, float(cell_h_px), baseline_px);
    }
    for (int i = 0; i < 4; ++i) { if (I.formats[i]) I.formats[i]->Release(); I.formats[i] = f[i]; }
    if (I.format) I.format->Release();
    I.format = f[0]; I.format->AddRef();
    for (auto &a : I.advance) a.clear();
    I.family = family; I.size_pt = size_pt;
    I.fm_valid = false;
    I.fmt_scale = scale; I.fmt_cell_h = cell_h_px; I.fmt_baseline = baseline_px;
    return true;
}

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
    const SwapConfig sc = host_->swap_config();  // BUG-02：宿主决定呈现模型
    desc.BufferCount = sc.buffer_count ? sc.buffer_count : 1;
    desc.SwapEffect = sc.effect == SwapEffect::Discard ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (sc.effect == SwapEffect::FlipSequential && desc.BufferCount < 2) desc.BufferCount = 2;
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
    if (!create_formats(*impl_, impl_->family, impl_->size_pt)) return false;
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

bool D2DRenderer::set_font(const wchar_t *font_family, float font_size_pt) {
    std::wstring fam = font_family && *font_family ? std::wstring(font_family) : impl_->family;
    if (!impl_->dwrite) { impl_->family = fam; impl_->size_pt = font_size_pt; return true; }  // init 前/失败：下次 init 生效
    if (!create_formats(*impl_, fam, font_size_pt)) return false;
    impl_->dirty.invalidate(); impl_->layout.reset(); impl_->last_vis = -1; impl_->blink_rows.clear();
    return true;
}

FontMetricsDip D2DRenderer::font_metrics() const {
    if (impl_->fm_valid) return impl_->fm_cache;
    FontMetricsDip est = estimate_font_metrics(impl_->size_pt);
    if (!impl_->dwrite || !impl_->format) return est;
    IDWriteFontCollection *col = nullptr;
    impl_->format->GetFontCollection(&col);
    if (!col) return est;
    UINT32 idx = 0; BOOL exists = FALSE;
    FontMetricsDip out = est;
    if (SUCCEEDED(col->FindFamilyName(impl_->family.c_str(), &idx, &exists)) && exists) {
        IDWriteFontFamily *fam = nullptr; IDWriteFont *font = nullptr; IDWriteFontFace *face = nullptr;
        if (SUCCEEDED(col->GetFontFamily(idx, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) &&
            SUCCEEDED(font->CreateFontFace(&face))) {
            DWRITE_FONT_METRICS fm{}; face->GetMetrics(&fm);
            float em = impl_->size_pt * (96.f / 72.f), k = fm.designUnitsPerEm ? em / fm.designUnitsPerEm : 0.f;
            UINT32 cp = 'M'; UINT16 gi = 0; DWRITE_GLYPH_METRICS gm{};
            if (k > 0 && SUCCEEDED(face->GetGlyphIndices(&cp, 1, &gi)) && SUCCEEDED(face->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE)))
                out = {gm.advanceWidth * k, fm.ascent * k, fm.descent * k, fm.lineGap * k, true};
        }
        if (face) face->Release();
        if (font) font->Release();
        if (fam) fam->Release();
    }
    col->Release();
    if (out.advance <= 0) out = est;
    impl_->fm_cache = out; impl_->fm_valid = true;
    return out;
}

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
    if (!preedit_.utf8.empty() && snap_) {
        uint16_t cols = itcore_snapshot_columns(snap_), rows = itcore_snapshot_rows(snap_);
        if (cols && rows) return preedit_layout(metrics_, preedit_, cols, rows).composition_anchor;
        col = preedit_.anchor_col; row = preedit_.anchor_row;
    }
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
    {  // DPI / 格高变化（含首帧、setFont 之后）：按实测度量重建 TextFormat 的字号与行距，整屏重画
        const float sc = metrics_.dpi_scale > 0 ? metrics_.dpi_scale : 1.f;
        const int ch = metrics_.cell_h_px();
        const float bl = GridMetrics::baseline_px(font_metrics(), sc);
        if (sc != I.fmt_scale || ch != I.fmt_cell_h || bl != I.fmt_baseline) {
            if (!create_formats(I, I.family, I.size_pt, sc, ch, bl)) return false;
            full = true;
        }
    }
    // DECSCNM 切换：默认色全部翻转 → 整屏重画
    const int rev = itcore_snapshot_reverse_screen(snap_) ? 1 : 0;
    if (rev != I.last_reverse) { full = true; I.last_reverse = rev; }
    // SGR 5：相位翻转时只把含闪烁格的行标脏（行级跟踪，BLINK-01）
    const uint64_t now = now_ms_;
    const bool blink_on = ((now / 1000) % 2) == 0;
    // 评审第 1 点：只画脏行
    std::vector<uint16_t> rows = blink_frame_rows(I.dirty, I.blink_rows, I.last_blink_on, snap_, L.rows, full, blink_on);

    uint16_t cc = 0, cr = 0; int vis = 0;
    itcore_snapshot_cursor(snap_, &cc, &cr, &vis);
    int cstyle_ps = 0; itcore_snapshot_cursor_style(snap_, &cstyle_ps);
    const CursorStyle cstyle = decscusr_to_style(cstyle_ps);
    const uint64_t since_input = now >= last_input_ms_ ? now - last_input_ms_ : 0;
    const bool cursor_on = cursor_visible(cstyle, focused_, since_input);
    bool overlay_changed = cc != I.last_cc || cr != I.last_cr || vis != I.last_vis || preedit_.utf8 != I.last_preedit ||
                           cursor_on != I.last_cursor_on || cstyle_ps != I.last_cursor_style || focused_ != I.last_focused;
    // 闪烁行表在下面重画时更新；先按旧表 + 将要重画的行估计，画完再按新表定 animating_
    animating_ = (vis && cstyle.blink && focused_) || I.blink_rows.any();
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
        itcore_wrap wrap; uint64_t rr;
        const itcore_cell *cells = y < L.draw_rows ? itcore_snapshot_row(snap_, y, &wrap, &rr) : nullptr;
        blink_note_row(I.blink_rows, y, cells, L.draw_cols);
        if (!cells) continue;
        const uint16_t cols = L.draw_cols;
        // 背景段合并（用 resolve_cell 的 bg，INVERSE 已互换）
        uint16_t x = 0;
        while (x < cols) {
            if (cells[x].flags & ITCORE_CELL_CONTINUATION) { ++x; continue; }
            uint32_t bg = resolve_cell(I.theme, cells[x], rev != 0).bg;
            uint16_t x1 = uint16_t(x + (cells[x].width ? cells[x].width : 1));
            while (x1 < cols) {
                if (cells[x1].flags & ITCORE_CELL_CONTINUATION) { ++x1; continue; }
                if (resolve_cell(I.theme, cells[x1], rev != 0).bg != bg) break;
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
            auto rcol = resolve_cell(I.theme, c, rev != 0);
            const bool text_on = text_visible(c, now);
            auto st = text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
            uint8_t cw = c.width ? c.width : 1;
            auto rc = metrics_.cell_rect(col, y, cw);
            D2D1_RECT_F clip = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
            if (c.grapheme_id && text_on) {
                size_t glen = 0;
                const char *g = itcore_snapshot_grapheme(snap_, c.grapheme_id, &glen);
                if (g && glen) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
                    std::wstring ws(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, g, int(glen), ws.data(), wlen);
                    I.brush_fg->SetColor(rgb_f(rcol.fg));
                    // fit_glyph：advance 超出格宽（回退字体/宽字形）时只缩不放、水平居中；advance 按样式缓存
                    const int fi = st.format_index();
                    float adv = 0.f;
                    auto it = I.advance[fi].find(ws);
                    if (it != I.advance[fi].end()) adv = it->second;
                    else {
                        IDWriteTextLayout *tl = nullptr;
                        if (SUCCEEDED(I.dwrite->CreateTextLayout(ws.c_str(), UINT32(ws.size()), I.formats[fi], 1e4f, 1e4f, &tl)) && tl) {
                            DWRITE_TEXT_METRICS tm{};
                            if (SUCCEEDED(tl->GetMetrics(&tm))) adv = tm.widthIncludingTrailingWhitespace;
                            tl->Release();
                        }
                        if (I.advance[fi].size() > 4096) I.advance[fi].clear();
                        I.advance[fi].emplace(ws, adv);
                    }
                    GlyphFit gf = fit_glyph(adv, cw, float(metrics_.cell_w_px()));
                    I.d2d->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                    D2D1_MATRIX_3X2_F old; I.d2d->GetTransform(&old);
                    if (gf.scale < 1.f) {
                        I.d2d->SetTransform(D2D1::Matrix3x2F::Scale(gf.scale, 1.f, D2D1::Point2F(clip.left, clip.top)) *
                                            D2D1::Matrix3x2F::Translation(gf.x_offset, 0.f) * old);
                    }
                    D2D1_RECT_F box = clip;
                    if (gf.scale < 1.f) box.right = clip.left + adv + 1.f;
                    I.d2d->DrawTextW(ws.c_str(), UINT32(ws.size()), I.formats[fi], box, I.brush_fg,
                                     D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                    I.d2d->SetTransform(old);
                    I.d2d->PopAxisAlignedClip();
                }
            }
            if (underline_visible(c, now)) {  // 闪烁隐藏相位不画下划线（BLINK-02）；位置/粗细按度量与 DPI
                auto ur = underline_rect(metrics_, col, y, cw);
                I.brush_fg->SetColor(rgb_f(rcol.underline));
                I.d2d->FillRectangle(D2D1::RectF(float(ur.x), float(ur.y), float(ur.x + ur.w), float(ur.y + ur.h)), I.brush_fg);
            }
            if ((c.flags & ITREN_CELL_STRIKE) && text_on) {
                auto sr = strike_rect(metrics_, col, y, cw);
                I.brush_fg->SetColor(rgb_f(rcol.fg));
                I.d2d->FillRectangle(D2D1::RectF(float(sr.x), float(sr.y), float(sr.x + sr.w), float(sr.y + sr.h)), I.brush_fg);
            }
        }
    }
    HRESULT hr = I.d2d->EndDraw();
    if (is_lost(hr)) { device_lost_ = true; return false; }
    animating_ = (vis && cstyle.blink && focused_) || I.blink_rows.any();  // 闪烁字符全部消失后停止定时刷新

    // 后缓冲：贴画布 + 预编辑 + 光标（覆盖层每帧重画，不污染画布）
    I.d2d->SetTarget(I.target);
    I.d2d->BeginDraw();
    I.d2d->DrawImage(I.canvas);
    if (!preedit_.utf8.empty()) {
        // ime_layout：按 cluster 宽度逐格排布（满行折行），子句下划线 + 预编辑内光标
        auto IL = preedit_layout(metrics_, preedit_, L.cols, L.rows);
        // 按核心字素簇切分（与 widths 对齐）
        std::vector<std::wstring> cl;
        for (auto &u8 : preedit_clusters(preedit_)) {
            int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), nullptr, 0);
            std::wstring w(n, 0);
            MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), w.data(), n);
            cl.push_back(std::move(w));
        }
        I.brush_fg->SetColor(D2D1::ColorF(D2D1::ColorF::Yellow));
        for (size_t i = 0; i < IL.cells.size() && i < cl.size(); ++i) {
            const auto &pc = IL.cells[i];
            if (pc.row >= L.rows) break;
            auto rc = metrics_.cell_rect(pc.col, pc.row, pc.width);
            D2D1_RECT_F r = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
            I.brush_bg->SetColor(rgb_f(theme_bg));
            I.d2d->FillRectangle(r, I.brush_bg);
            I.d2d->DrawTextW(cl[i].c_str(), UINT32(cl[i].size()), I.formats[0], r, I.brush_fg,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        for (auto &u : IL.underlines)
            I.d2d->FillRectangle(D2D1::RectF(float(u.x), float(u.y), float(u.x + u.w), float(u.y + u.h)), I.brush_fg);
        auto &k = IL.caret;
        I.d2d->FillRectangle(D2D1::RectF(float(k.x), float(k.y), float(k.x + k.w), float(k.y + k.h)), I.brush_caret);
    } else if (vis && cursor_on && cc < L.cols && cr < L.rows) {
        // DECSCUSR 形状；宽字符首格覆盖 2 格；失焦的块光标画空心框
        // 落在宽字符尾格时先吸附到首格再判宽（CUR-01）
        const CursorCell ccell = cursor_cell(snap_, cr, cc);
        const uint16_t dc = ccell.col;
        const bool wide = ccell.wide;
        itcore_wrap w; uint64_t rr;
        for (auto &q : cursor_rects(metrics_, cr, dc, cstyle, focused_, wide, metrics_.dpi_scale))
            I.d2d->FillRectangle(D2D1::RectF(float(q.x), float(q.y), float(q.x + q.w), float(q.y + q.h)), I.brush_caret);
        if (cursor_inverts_glyph(cstyle, focused_) && row_has_text(snap_, cr, dc)) {
            const itcore_cell *row = itcore_snapshot_row(snap_, cr, &w, &rr);
            const itcore_cell &c = row[dc];
            size_t glen = 0;
            const char *g = c.grapheme_id ? itcore_snapshot_grapheme(snap_, c.grapheme_id, &glen) : nullptr;
            if (g && glen && text_visible(c, now)) {
                int wl = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
                std::wstring gs(wl, 0);
                MultiByteToWideChar(CP_UTF8, 0, g, int(glen), gs.data(), wl);
                auto rc = metrics_.cell_rect(dc, cr, wide ? 2 : 1);
                D2D1_RECT_F r = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
                auto st = text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
                I.brush_fg->SetColor(rgb_f(resolve_cell(I.theme, c, rev != 0).bg));
                I.d2d->DrawTextW(gs.c_str(), UINT32(gs.size()), I.formats[st.format_index()], r, I.brush_fg,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
    }
    hr = I.d2d->EndDraw();
    if (is_lost(hr)) { device_lost_ = true; return false; }
    hr = I.swap->Present(1, 0);
    if (is_lost(hr)) { device_lost_ = true; return false; }
    I.last_cc = cc; I.last_cr = cr; I.last_vis = vis; I.last_preedit = preedit_.utf8;
    I.last_cursor_on = cursor_on; I.last_cursor_style = cstyle_ps; I.last_focused = focused_;
    last_drawn_revision_ = itcore_snapshot_revision(snap_);
    return true;
}

#else  // !_WIN32

bool D2DRenderer::init(const GridMetrics &metrics, const wchar_t *, float font_size_pt) {
    metrics_ = metrics;
    stub_size_pt_ = font_size_pt;
    device_kind_ = DeviceKind::Stub;
    return true;  // 编译门闸：逻辑路径可测 caret_rect
}
void D2DRenderer::shutdown() { snap_ = nullptr; }
void D2DRenderer::set_snapshot(const itcore_snapshot *snap) { snap_ = snap; }
bool D2DRenderer::set_font(const wchar_t *, float font_size_pt) { stub_size_pt_ = font_size_pt; return true; }
FontMetricsDip D2DRenderer::font_metrics() const { return estimate_font_metrics(stub_size_pt_); }
bool D2DRenderer::resize_surface() { return true; }
GridMetrics::Rect D2DRenderer::caret_rect_px() const {
    uint16_t col = 0, row = 0; int vis = 0;
    if (snap_) itcore_snapshot_cursor(snap_, &col, &row, &vis);
    if (!preedit_.utf8.empty() && snap_) {
        uint16_t cols = itcore_snapshot_columns(snap_), rows = itcore_snapshot_rows(snap_);
        if (cols && rows) return preedit_layout(metrics_, preedit_, cols, rows).composition_anchor;
        col = preedit_.anchor_col; row = preedit_.anchor_row;
    }
    return metrics_.cell_rect(col, row, 1);
}
bool D2DRenderer::present_frame() { return false; }
bool D2DRenderer::recover_device() { device_lost_ = false; return true; }
void D2DRenderer::shutdown_device() {}

#endif

}  // namespace itren
