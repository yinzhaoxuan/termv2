// SPDX-License-Identifier: GPL-3.0-or-later
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 非 WIN32 构建：提供可链接的空实现，便于 Linux CI 编译门闸。
#include "renderer.h"
#include "font_family.h"
#include "cell_style.h"
#include "frame_policy.h"
#include "cursor_geometry.h"
#include "text_runs.h"
#include "ime_layout.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string_view>

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
#  include <dwrite_2.h>
#  include <dxgi1_2.h>
#  include <d3d11.h>
#  pragma comment(lib, "d2d1")
#  pragma comment(lib, "dwrite")
#  pragma comment(lib, "d3d11")
#  pragma comment(lib, "dxgi")
#endif

namespace itren {

#ifdef _WIN32
// DS-18 §9：文本计时用的单调时钟（纳秒）
static uint64_t qpc_now_ns() {
    static const uint64_t freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart ? f.QuadPart : 1); }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart) / freq * 1'000'000'000ull + uint64_t(c.QuadPart) % freq * 1'000'000'000ull / freq;
}
#endif

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
    // ---- DS-18 字形运行路径 ----
    IDWriteFactory2 *dwrite2 = nullptr;           // 系统回退与彩色字形；取不到时只用旧路径
    IDWriteTextAnalyzer *analyzer = nullptr;
    IDWriteFontFallback *fallback = nullptr;
    IDWriteFontFace *primary[4] = {};             // 回退无字体可用时的主字体面（按样式）
    ID2D1SolidColorBrush *brush_layer = nullptr;  // 彩色字形的固定色层
    bool glyph_path = false;                      // 可用且未被 ITERM2WIN_TEXT_PATH=drawtext 关闭
    bool ligatures = false;
    float em_px = 0.f;                            // 主字号（像素）= pt × 96/72 × dpi_scale（DS-18 §3）
    ShapeCache shape_cache;
    PlanCounters plan_counters;
    RenderStatsWindow stats;
    TextTimer timer{qpc_now_ns};
    IsolateState last_iso;
    IsolatedPlan iso_plan;
    // 字体、DPI、连字开关、设备变化：塑形结果全部作废，下一个统计窗口标冷
    void reset_shaping() { shape_cache.clear(); iso_plan.invalidate(); last_iso = {}; stats.markCold(); }
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

// 字体族名归一到 DirectWrite：系统字体集合里有就原样用；否则按 GDI 名（ChooseFont 的 lfFaceName，
// 如 "Cascadia Mono SemiBold"，GDI 把字重拆成单独的族）经 IDWriteGdiInterop 映射回 DirectWrite 族名
// （"Cascadia Mono"）。都找不到返回 false：CreateTextFormat 对不存在的族名照样成功，会悄悄退回系统字体。
bool resolve_family(IDWriteFactory *dw, const std::wstring &in, std::wstring *out) {
    if (!dw || in.empty()) return false;
    bool ok = false;
    IDWriteFontCollection *col = nullptr;
    if (SUCCEEDED(dw->GetSystemFontCollection(&col, FALSE)) && col) {
        UINT32 idx = 0; BOOL exists = FALSE;
        if (SUCCEEDED(col->FindFamilyName(in.c_str(), &idx, &exists)) && exists) { *out = in; ok = true; }
        col->Release();
    }
    if (ok || in.size() >= LF_FACESIZE) return ok;
    IDWriteGdiInterop *gdi = nullptr;
    if (FAILED(dw->GetGdiInterop(&gdi)) || !gdi) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, in.c_str(), LF_FACESIZE - 1);
    IDWriteFont *font = nullptr; IDWriteFontFamily *fam = nullptr; IDWriteLocalizedStrings *names = nullptr;
    if (SUCCEEDED(gdi->CreateFontFromLOGFONT(&lf, &font)) && font && SUCCEEDED(font->GetFontFamily(&fam)) && fam &&
        SUCCEEDED(fam->GetFamilyNames(&names)) && names && names->GetCount() > 0) {
        UINT32 i = 0; BOOL has = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &i, &has)) || !has) i = 0;
        UINT32 len = 0;
        if (SUCCEEDED(names->GetStringLength(i, &len))) {
            std::wstring n(len + 1, L'\0');
            if (SUCCEEDED(names->GetString(i, n.data(), len + 1))) { n.resize(len); *out = n; ok = !n.empty(); }
        }
    }
    if (names) names->Release();
    if (fam) fam->Release();
    if (font) font->Release();
    gdi->Release();
    return ok;
}

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
    // DS-18：字号只在这里乘一次 DPI；主字体面供回退无字体时使用；旧的塑形结果作废
    I.em_px = size_pt * (96.f / 72.f) * (scale > 0 ? scale : 1.f);
    for (auto *&f : I.primary) if (f) { f->Release(); f = nullptr; }
    IDWriteFontCollection *col = nullptr;
    if (SUCCEEDED(I.dwrite->GetSystemFontCollection(&col, FALSE)) && col) {
        UINT32 idx = 0; BOOL exists = FALSE;
        IDWriteFontFamily *fam = nullptr;
        if (SUCCEEDED(col->FindFamilyName(family.c_str(), &idx, &exists)) && exists && SUCCEEDED(col->GetFontFamily(idx, &fam)) && fam) {
            for (int i = 0; i < 4; ++i) {
                IDWriteFont *font = nullptr;
                if (SUCCEEDED(fam->GetFirstMatchingFont((i & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                        DWRITE_FONT_STRETCH_NORMAL,
                                                        (i & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)) && font) {
                    font->CreateFontFace(&I.primary[i]);
                    font->Release();
                }
            }
            fam->Release();
        }
        col->Release();
    }
    I.reset_shaping();
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
    // 启动时 settings 里的族名也归一；找不到则退回内置等宽默认（Consolas → Courier New），
    // 不再拿不存在的族名建 TextFormat（度量会退回估算）。全都找不到才原样交给 DirectWrite 回退（FONT-02）
    {
        bool fell_back = false;
        std::wstring rf = pick_font_family(impl_->family,
            [&](const std::wstring &n, std::wstring *o) { return resolve_family(impl_->dwrite, n, o); }, &fell_back);
        if (rf.empty()) std::fprintf(stderr, "[renderer] 字体族与内置默认都找不到，交给 DirectWrite 回退\n");
        else {
            if (fell_back) std::fprintf(stderr, "[renderer] 字体族找不到，退回 %ls\n", rf.c_str());
            impl_->family = rf;
        }
    }
    if (!create_formats(*impl_, impl_->family, impl_->size_pt)) return false;
    {   // DS-18：新路径要 IDWriteFactory2（系统回退、彩色字形）和文本分析器；缺任何一个只用旧路径
        auto &I = *impl_;
        const char *path = std::getenv("ITERM2WIN_TEXT_PATH");
        const bool forceDrawText = path && std::string_view(path) == "drawtext";
        I.dwrite->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void **>(&I.dwrite2));
        if (I.dwrite2) I.dwrite2->GetSystemFontFallback(&I.fallback);
        I.dwrite->CreateTextAnalyzer(&I.analyzer);
        I.glyph_path = !forceDrawText && I.dwrite2 && I.fallback && I.analyzer;
        if (!I.glyph_path)
            std::fprintf(stderr, "[renderer] 文本走逐格 DrawText（%s）\n", forceDrawText ? "ITERM2WIN_TEXT_PATH=drawtext" : "缺 IDWriteFactory2 / 回退 / 分析器");
    }
    device_kind_ = kind;
    impl_->saved_metrics = metrics_;
    impl_->dirty.invalidate(); impl_->layout.reset(); impl_->last_vis = -1;

    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &impl_->brush_bg);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &impl_->brush_fg);
    impl_->d2d->CreateSolidColorBrush(rgb_f(impl_->theme.cursor), &impl_->brush_caret);
    impl_->d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &impl_->brush_layer);
    return true;
}

void D2DRenderer::shutdown() {
    device_kind_ = DeviceKind::None;
#ifdef _WIN32
    auto rel = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
    if (impl_) {
        if (impl_->d2d) impl_->d2d->SetTarget(nullptr);
        rel(impl_->brush_caret); rel(impl_->brush_fg); rel(impl_->brush_bg); rel(impl_->brush_layer);
        for (auto *&f : impl_->formats) rel(f);
        for (auto *&f : impl_->primary) rel(f);
        rel(impl_->fallback); rel(impl_->analyzer); rel(impl_->dwrite2);
        impl_->glyph_path = false;
        impl_->reset_shaping();  // 塑形结果里的字体面各自持有引用，清缓存即释放
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

void D2DRenderer::set_search_highlight(SearchHighlight hl) {
    search_dirty_ = SearchHighlight::dirty_rows(search_hl_, hl);
    search_hl_ = std::move(hl);
}
void D2DRenderer::clear_search_highlight() {
    SearchHighlight empty = search_hl_;
    empty.clear();  // 保留尺寸，dirty_rows 才能标出旧命中行
    search_dirty_ = SearchHighlight::dirty_rows(search_hl_, empty);
    search_hl_ = std::move(empty);
}

bool D2DRenderer::set_font(const wchar_t *font_family, float font_size_pt) {
    std::wstring fam = font_family && *font_family ? std::wstring(font_family) : impl_->family;
    if (!impl_->dwrite) { impl_->family = fam; impl_->size_pt = font_size_pt; return true; }  // init 前/失败：下次 init 生效
    std::wstring rf;
    if (!resolve_family(impl_->dwrite, fam, &rf)) return false;  // 族名不存在：保持旧字体，backend 走回退
    if (!create_formats(*impl_, rf, font_size_pt)) return false;
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
    if (!init(m, fam.c_str(), pt)) return false;
    pace_.reset();  // 新交换链：重新开始节流计时
    return true;
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

// ---- DS-18 §4.2：文本分析的源与接收器（栈上对象，引用计数恒为 1）----
class AnalysisSource final : public IDWriteTextAnalysisSource {
public:
    AnalysisSource(const wchar_t *text, UINT32 len) : text_(text), len_(len) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, WCHAR const **text, UINT32 *len) override {
        if (pos >= len_) { *text = nullptr; *len = 0; } else { *text = text_ + pos; *len = len_ - pos; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, WCHAR const **text, UINT32 *len) override {
        if (pos == 0 || pos > len_) { *text = nullptr; *len = 0; } else { *text = text_; *len = pos; }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;  // 不做 bidi 重排（DS-18 §4.2）
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32 *len, WCHAR const **locale) override {
        *len = pos < len_ ? len_ - pos : 0; *locale = L"en-us"; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32 *len, IDWriteNumberSubstitution **sub) override {
        *len = pos < len_ ? len_ - pos : 0; *sub = nullptr; return S_OK;
    }
private:
    const wchar_t *text_;
    UINT32 len_;
};

class AnalysisSink final : public IDWriteTextAnalysisSink {
public:
    explicit AnalysisSink(UINT32 len) : levels(len, 0) {}
    std::vector<ScriptRange> scripts;
    std::vector<uint8_t> levels;  // 每个 UTF-16 单元的解析层级；只用来判断 RTL 格，不重排
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSink)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 pos, UINT32 len, DWRITE_SCRIPT_ANALYSIS const *sa) override {
        scripts.push_back({{pos, len}, sa->script, uint8_t(sa->shapes)});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, DWRITE_LINE_BREAKPOINT const *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 pos, UINT32 len, UINT8, UINT8 resolved) override {
        for (UINT32 i = pos; i < pos + len && i < levels.size(); ++i) levels[i] = resolved;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution *) override { return S_OK; }
};

std::u16string utf8_to_u16(const char *s, size_t n) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, int(n), nullptr, 0);
    std::u16string out(size_t(len > 0 ? len : 0), u'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s, int(n), reinterpret_cast<wchar_t *>(out.data()), len);
    return out;
}
const wchar_t *wide(const std::u16string &s) { return reinterpret_cast<const wchar_t *>(s.data()); }

// 列范围 × 一行的像素矩形（列数可超过 255）
D2D1_RECT_F cols_rect(const GridMetrics &m, uint16_t col0, uint32_t ncols, uint16_t row) {
    const auto a = m.cell_rect(col0, row, 1);
    return D2D1::RectF(float(a.x), float(a.y), float(a.x) + float(ncols) * float(m.cell_w_px()), float(a.y + a.h));
}
}  // namespace

void D2DRenderer::set_ligatures(bool on) {
    if (impl_->ligatures == on) return;
    impl_->ligatures = on;
    impl_->reset_shaping();
    impl_->dirty.invalidate();
}

// DS-18 §4.2–4.3：AnalyzeScript + AnalyzeBidi + MapCharacters → planItems → GetGlyphs / GetGlyphPlacements。
std::shared_ptr<const ShapedRun> D2DRenderer::shape_run(Impl &I, const TextRun &run) {
    const UINT32 len = UINT32(run.text.size());
    if (!len || !I.analyzer || !I.fallback) return nullptr;
    const wchar_t *text = wide(run.text);
    AnalysisSource src(text, len);
    AnalysisSink sink(len);
    if (FAILED(I.analyzer->AnalyzeScript(&src, 0, len, &sink))) return nullptr;
    if (FAILED(I.analyzer->AnalyzeBidi(&src, 0, len, &sink))) return nullptr;
    std::sort(sink.scripts.begin(), sink.scripts.end(),
              [](const ScriptRange &a, const ScriptRange &b) { return a.span.begin < b.span.begin; });

    // 回退：字体面去重（同一字体面、同一缩放共用一个下标）
    struct Face { std::shared_ptr<void> owner; IDWriteFontFace *raw; float scale; };
    std::vector<Face> faces;
    std::vector<FontRange> fonts;
    const int fi = run.format & 3;
    const DWRITE_FONT_WEIGHT weight = (fi & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const DWRITE_FONT_STYLE style = (fi & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    for (UINT32 pos = 0; pos < len;) {
        UINT32 mapped = 0; IDWriteFont *font = nullptr; FLOAT scale = 1.f;
        HRESULT hr = I.fallback->MapCharacters(&src, pos, len - pos, nullptr, I.family.c_str(), weight, style,
                                               DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale);
        if (FAILED(hr) || mapped == 0) { if (font) font->Release(); return nullptr; }
        IDWriteFontFace *face = nullptr;
        if (font) {
            hr = font->CreateFontFace(&face);
            font->Release();
            if (FAILED(hr) || !face) return nullptr;
        } else {  // 没有任何字体含这些字符：用主字体画（豆腐块），列不变
            face = I.primary[fi];
            if (!face) return nullptr;
            face->AddRef();
            scale = 1.f;
        }
        int id = -1;
        for (size_t i = 0; i < faces.size(); ++i)
            if (faces[i].raw == face && faces[i].scale == scale) { id = int(i); break; }
        if (id < 0) {
            faces.push_back({std::shared_ptr<void>(face, [](void *f) { static_cast<IDWriteFontFace *>(f)->Release(); }), face, scale});
            id = int(faces.size() - 1);
        } else {
            face->Release();
        }
        fonts.push_back({{pos, mapped}, uint16_t(id)});
        pos += mapped;
    }
    auto hasAll = [&](uint16_t f, uint16_t k) {
        const TextSpan sp = run.cellSpans[k];
        for (UINT32 u = sp.begin; u < sp.end();) {
            UINT32 cp = text[u], n = 1;
            if (cp >= 0xD800 && cp < 0xDC00 && u + 1 < sp.end()) { cp = 0x10000 + ((cp - 0xD800) << 10) + (UINT32(text[u + 1]) - 0xDC00); n = 2; }
            u += n;
            if (isZeroWidthFormat(cp)) continue;
            UINT16 gi = 0;
            if (FAILED(faces[f].raw->GetGlyphIndices(&cp, 1, &gi)) || gi == 0) return false;
        }
        return true;
    };
    const ItemPlan plan = planItems(run.cellSpans, sink.scripts, fonts, rtlCells(sink.levels, run.cellSpans), hasAll);

    auto out = std::make_shared<ShapedRun>();
    out->legacyCells = plan.legacyCells;
    // 连字关：只关可选连字（liga / calt / clig），script 必需的特性不受影响（DS-18 §4.3）
    DWRITE_FONT_FEATURE off[3] = {{DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 0},
                                  {DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES, 0},
                                  {DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES, 0}};
    DWRITE_TYPOGRAPHIC_FEATURES typo{off, 3};
    for (const ItemRange &ir : plan.items) {
        const Face &fc = faces[ir.font];
        const ScriptRange &sr = sink.scripts[ir.scriptRange];
        DWRITE_SCRIPT_ANALYSIS sa{sr.script, DWRITE_SCRIPT_SHAPES(sr.shapes)};
        const UINT32 n = ir.span.length;
        const wchar_t *t = text + ir.span.begin;
        const DWRITE_TYPOGRAPHIC_FEATURES *features[1] = {&typo};
        const UINT32 featureLens[1] = {n};
        const DWRITE_TYPOGRAPHIC_FEATURES **feat = I.ligatures ? nullptr : features;
        const UINT32 *featLen = I.ligatures ? nullptr : featureLens;
        const UINT32 nFeat = I.ligatures ? 0 : 1;
        std::vector<UINT16> cm(n), gi;
        std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> tp(n);
        std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> gp;
        UINT32 maxG = 3 * n / 2 + 16, actual = 0;
        const UINT32 cap = 8 * n + 64;
        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 4; ++attempt) {  // 缓冲不足时加倍，最多重试 3 次
            gi.resize(maxG); gp.resize(maxG);
            hr = I.analyzer->GetGlyphs(t, n, fc.raw, FALSE, FALSE, &sa, L"en-us", nullptr, feat, featLen, nFeat,
                                       maxG, cm.data(), tp.data(), gi.data(), gp.data(), &actual);
            if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || maxG >= cap) break;
            maxG = std::min(maxG * 2, cap);
        }
        if (FAILED(hr) || actual == 0) return nullptr;
        std::vector<FLOAT> adv(actual);
        std::vector<DWRITE_GLYPH_OFFSET> off2(actual);
        const float em = I.em_px * fc.scale;
        hr = I.analyzer->GetGlyphPlacements(t, cm.data(), tp.data(), n, gi.data(), gp.data(), actual, fc.raw, em,
                                            FALSE, FALSE, &sa, L"en-us", feat, featLen, nFeat, adv.data(), off2.data());
        if (FAILED(hr)) return nullptr;
        ShapedItem it;
        it.span = ir.span; it.face = fc.owner; it.emPx = em; it.script = sr.script; it.shapes = sr.shapes;
        it.glyphIndices.assign(gi.begin(), gi.begin() + actual);
        it.advancesPx.assign(adv.begin(), adv.end());
        it.offsetXPx.resize(actual); it.offsetYPx.resize(actual);
        for (UINT32 g = 0; g < actual; ++g) { it.offsetXPx[g] = off2[g].advanceOffset; it.offsetYPx[g] = off2[g].ascenderOffset; }
        it.clusterMap.assign(cm.begin(), cm.end());
        out->items.push_back(std::move(it));
    }
    return out;
}

// 旧方式画一个字素（逐格 DrawTextW）：fit_glyph 只缩不放、居中；advance 按样式缓存。旧路径与降级格共用。
void D2DRenderer::draw_cell_text(Impl &I, const GridMetrics &m, const std::wstring &ws, int fi, GridMetrics::Rect rc,
                                 uint8_t cw, uint32_t color) {
    D2D1_RECT_F clip = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
    I.brush_fg->SetColor(rgb_f(color));
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
    GlyphFit gf = fit_glyph(adv, cw, float(m.cell_w_px()));
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

namespace {
// DS-18 §6：COLR v0 分层；paletteIndex 0xFFFF 用当前文本颜色（brush_fg），其他层用字体给的颜色。
// 非彩色字体，或 TranslateColorGlyphRun 返回 DWRITE_E_NOCOLOR / 失败：按普通字形画。
void draw_glyph_run(ID2D1DeviceContext *d2d, IDWriteFactory2 *f2, ID2D1SolidColorBrush *text, ID2D1SolidColorBrush *layer,
                    D2D1_POINT_2F origin, const DWRITE_GLYPH_RUN &gr) {
    bool colorFont = false;
    IDWriteFontFace2 *face2 = nullptr;
    if (f2 && SUCCEEDED(gr.fontFace->QueryInterface(__uuidof(IDWriteFontFace2), reinterpret_cast<void **>(&face2))) && face2) {
        colorFont = face2->IsColorFont();
        face2->Release();
    }
    if (colorFont) {
        IDWriteColorGlyphRunEnumerator *e = nullptr;
        if (SUCCEEDED(f2->TranslateColorGlyphRun(origin.x, origin.y, &gr, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &e)) && e) {
            BOOL more = FALSE;
            while (SUCCEEDED(e->MoveNext(&more)) && more) {
                DWRITE_COLOR_GLYPH_RUN const *cr = nullptr;
                if (FAILED(e->GetCurrentRun(&cr)) || !cr) break;
                ID2D1Brush *b = text;
                if (cr->paletteIndex != 0xFFFF && layer) {
                    layer->SetColor(D2D1::ColorF(cr->runColor.r, cr->runColor.g, cr->runColor.b, cr->runColor.a));
                    b = layer;
                }
                d2d->DrawGlyphRun(D2D1::Point2F(cr->baselineOriginX, cr->baselineOriginY), &cr->glyphRun, nullptr, b,
                                  DWRITE_MEASURING_MODE_NATURAL);
            }
            e->Release();
            return;
        }
        if (e) e->Release();
    }
    // ID2D1DeviceContext 的 5 参版本（带 glyphRunDescription）：避免基类 4 参重载被隐藏时编不过
    d2d->DrawGlyphRun(origin, &gr, nullptr, text, DWRITE_MEASURING_MODE_NATURAL);
}
}  // namespace

// DS-18 §5.4：按计划画一段。底层与块光标反色（§7）都走这里，只是颜色不同。
void D2DRenderer::draw_plan(Impl &I, const GridMetrics &m, uint16_t row, const TextRun &run, const RunPlan &plan,
                            uint32_t color) {
    const int fi = run.format & 3;
    auto legacy = [&](uint16_t k, ColRange clip) {
        if (k >= run.cellSpans.size()) return;
        const TextSpan sp = run.cellSpans[k];
        const std::wstring ws(wide(run.text) + sp.begin, sp.length);
        auto rc = m.cell_rect(clip.col0, row, 1);
        rc.w = m.cell_w_px() * clip.ncols;
        draw_cell_text(I, m, ws, fi, rc, uint8_t(clip.ncols > 255 ? 255 : clip.ncols), color);
    };
    if (plan.legacyWhole()) {  // 塑形失败：整段逐格旧方式
        for (size_t k = 0; k < run.cellCols.size(); ++k) {
            const uint16_t col = run.cellCols[k];
            const uint16_t next = k + 1 < run.cellCols.size() ? run.cellCols[k + 1] : uint16_t(run.cols.col0 + run.cols.ncols);
            legacy(uint16_t(k), {col, uint16_t(next - col)});
        }
        return;
    }
    const float baseline = float(m.cell_rect(0, row, 1).y) + I.fmt_baseline;
    std::vector<DWRITE_GLYPH_OFFSET> offs;
    for (const DrawPiece &p : plan.pieces) {
        if (p.kind == DrawPiece::LegacyCell) { legacy(p.cell, p.clip); continue; }
        const ShapedItem &it = plan.shaped->items[p.item];
        const UINT32 n = p.glyphEnd - p.glyphBegin;
        offs.resize(n);
        for (UINT32 i = 0; i < n; ++i) offs[i] = {it.offsetXPx[p.glyphBegin + i], it.offsetYPx[p.glyphBegin + i]};
        DWRITE_GLYPH_RUN gr{};
        gr.fontFace = static_cast<IDWriteFontFace *>(it.face.get());
        gr.fontEmSize = it.emPx;
        gr.glyphCount = n;
        gr.glyphIndices = it.glyphIndices.data() + p.glyphBegin;
        gr.glyphAdvances = p.advancesPx.data();
        gr.glyphOffsets = offs.data();
        gr.isSideways = FALSE;
        gr.bidiLevel = 0;
        if (!gr.fontFace) continue;
        I.brush_fg->SetColor(rgb_f(color));
        const D2D1_POINT_2F origin = D2D1::Point2F(float(m.pad_x_px()) + p.originXPx, baseline);
        I.d2d->PushAxisAlignedClip(cols_rect(m, p.clip.col0, p.clip.ncols, row), D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_MATRIX_3X2_F old; I.d2d->GetTransform(&old);
        if (p.scale != 1.f) I.d2d->SetTransform(D2D1::Matrix3x2F::Scale(p.scale, 1.f, origin) * old);
        draw_glyph_run(I.d2d, I.dwrite2, I.brush_fg, I.brush_layer, origin, gr);
        I.d2d->SetTransform(old);
        I.d2d->PopAxisAlignedClip();
    }
}

bool D2DRenderer::present_frame(PresentMode mode) {
    if (device_lost_ && !recover_device()) return false;
    if (!impl_ || !impl_->d2d || !impl_->swap || !impl_->target || !snap_) return false;
    const auto pace_action = pace_.decide(now_ms_, mode);
    if (pace_action == PresentPace::Action::Skip) return false;  // 距上次 Present <16ms：保留脏区
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
    itcore_colors colors{};
    itcore_snapshot_colors(snap_, &colors);
    const auto theme = themeFromColors(colors, I.theme.search);
    if (theme != I.theme) {
        I.theme = theme;
        I.dirty.invalidate();
        full = true;  // 背景边缘也必须清屏
        I.brush_caret->SetColor(rgb_f(I.theme.cursor));
    }
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
    // 搜索高亮变化：只标脏行（next/prev 换当前命中、滚动视口、clear/RESTARTED）
    for (uint16_t y : search_dirty_) I.dirty.mark(y);
    search_dirty_.clear();
    uint16_t cc = 0, cr = 0; int vis = 0;
    itcore_snapshot_cursor(snap_, &cc, &cr, &vis);
    int cstyle_ps = 0; itcore_snapshot_cursor_style(snap_, &cstyle_ps);
    const CursorStyle cstyle = decscusr_to_style(cstyle_ps);
    // DS-18 §7：块光标格单独成段。隔离只看 DECTCEM、块形状、预编辑，与闪烁相位和焦点无关；
    // 隔离状态变化时旧行、新行都要重新塑形
    IsolateState iso;
    if (I.glyph_path && vis && preedit_.utf8.empty() && cstyle.shape == CursorShape::Block && cc < L.cols && cr < L.rows) {
        const CursorCell head = cursor_cell(snap_, cr, cc);
        iso = {true, cr, {head.col, uint16_t(head.wide ? 2 : 1)}};
    }
    if (!(iso == I.last_iso)) {
        for (uint16_t y : isolateDirtyRows(I.last_iso, iso)) I.dirty.mark(y);
        I.last_iso = iso;
    }
    // 评审第 1 点：只画脏行
    std::vector<uint16_t> rows = blink_frame_rows(I.dirty, I.blink_rows, I.last_blink_on, snap_, L.rows, full, blink_on);

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
            uint32_t bg = resolve_cell(I.theme, cells[x], rev != 0, search_hl_.kind(y, x)).bg;
            uint16_t x1 = uint16_t(x + (cells[x].width ? cells[x].width : 1));
            while (x1 < cols) {
                if (cells[x1].flags & ITCORE_CELL_CONTINUATION) { ++x1; continue; }
                if (resolve_cell(I.theme, cells[x1], rev != 0, search_hl_.kind(y, x1)).bg != bg) break;
                x1 = uint16_t(x1 + (cells[x1].width ? cells[x1].width : 1));
            }
            if (x1 > cols) x1 = cols;
            auto rc = metrics_.cell_rect(x, y, uint8_t(x1 > x ? (x1 - x > 255 ? 255 : x1 - x) : 1));
            if (x1 - x > 255) rc.w = metrics_.cell_w_px() * (x1 - x);
            I.brush_bg->SetColor(rgb_f(bg));
            I.d2d->FillRectangle(D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h)), I.brush_bg);
            x = x1;
        }
        // 文本（DS-18）：新路径按段塑形、按格定位；旧路径（ITERM2WIN_TEXT_PATH=drawtext）逐格 DrawTextW。
        // 只有文本工作计入 render.stats（§9）；背景已画完，装饰在下面按格画，都不计时
        if (I.glyph_path) {
            I.timer.begin();
            std::vector<TextCellIn> tc(cols);
            std::vector<uint8_t> widths(cols, 0);
            for (uint16_t col = 0; col < cols; ++col) {
                const itcore_cell &c = cells[col];
                TextCellIn &t = tc[col];
                if (c.flags & ITCORE_CELL_CONTINUATION) { t.width = 0; continue; }
                const auto rcol = resolve_cell(I.theme, c, rev != 0, search_hl_.kind(y, col));
                t.width = c.width ? c.width : 1;
                widths[col] = t.width;
                t.format = uint8_t(text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE).format_index());
                t.fg = rcol.fg; t.bg = rcol.bg;
                t.visible = text_visible(c, now);
                size_t glen = 0;
                const char *g = c.grapheme_id ? itcore_snapshot_grapheme(snap_, c.grapheme_id, &glen) : nullptr;
                if (g && glen) t.text = utf8_to_u16(g, glen);
            }
            std::optional<ColRange> isoCols;
            if (iso.active && iso.row == y) isoCols = iso.cols;
            const float cellW = float(metrics_.cell_w_px());
            auto shaper = [&I](const TextRun &r) { return shape_run(I, r); };
            for (const TextRun &run : segmentRow(tc, isoCols)) {
                RunPlan plan = planRun(run, widths, cellW, I.ligatures, I.shape_cache, shaper, I.plan_counters);
                draw_plan(I, metrics_, y, run, plan, run.fg);
                if (isoCols && run.cols == *isoCols) {  // 覆盖层反色字形用同一份计划（§7）
                    I.iso_plan.state = iso; I.iso_plan.rowRevision = rr;
                    I.iso_plan.plan = std::move(plan); I.iso_plan.run = run;
                }
            }
            I.timer.end();
        }
        for (uint16_t col = 0; col < cols; ++col) {
            const itcore_cell &c = cells[col];
            if (c.flags & ITCORE_CELL_CONTINUATION) continue;
            auto rcol = resolve_cell(I.theme, c, rev != 0, search_hl_.kind(y, col));
            const bool text_on = text_visible(c, now);
            auto st = text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
            uint8_t cw = c.width ? c.width : 1;
            if (!I.glyph_path && c.grapheme_id && text_on) {
                I.timer.begin();
                size_t glen = 0;
                const char *g = itcore_snapshot_grapheme(snap_, c.grapheme_id, &glen);
                if (g && glen) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
                    std::wstring ws(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, g, int(glen), ws.data(), wlen);
                    draw_cell_text(I, metrics_, ws, st.format_index(), metrics_.cell_rect(col, y, cw), cw, rcol.fg);
                }
                I.timer.end();
            }
            if (underline_visible(c, now)) {  // 闪烁隐藏相位不画下划线（BLINK-02）；样式见 underline_style_rects
                I.brush_fg->SetColor(rgb_f(rcol.underline));
                for (auto &ur : underline_style_rects(metrics_, col, y, cw, cell_underline_style(c)))
                    I.d2d->FillRectangle(D2D1::RectF(float(ur.x), float(ur.y), float(ur.x + ur.w), float(ur.y + ur.h)), I.brush_fg);
            }
            if (overline_visible(c, now)) {  // SGR 53；颜色跟前景，闪烁相位与下划线相同
                auto ov = overline_rect(metrics_, col, y, cw);
                I.brush_fg->SetColor(rgb_f(rcol.fg));
                I.d2d->FillRectangle(D2D1::RectF(float(ov.x), float(ov.y), float(ov.x + ov.w), float(ov.y + ov.h)), I.brush_fg);
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
    {   // DS-18 §9：本帧文本 CPU 时间 → 统计窗口；窗口满时写一条 render.stats
        uint32_t drawn = 0;
        for (uint16_t y : rows) if (y < L.rows) ++drawn;
        const uint64_t textNs = I.timer.takeFrameNs();
        if (auto st = I.stats.add(qpc_now_ns(), drawn, L.rows, textNs, I.shape_cache.stats(), I.plan_counters); st && diag_sink_) {
            const RenderStatsContext ctx{I.glyph_path ? "glyphrun" : "drawtext", I.ligatures, metrics_.dpi_scale, L.cols, L.rows};
            diag_sink_(formatRenderStats(*st, ctx));
        }
    }
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
            // DS-18 §7：底层画这一格时保存的计划仍有效（隔离状态与行内容都没变）就直接用，只换成反色；
            // 不查缓存、不重新塑形。计划缺失（例如这一格是空白段）才退回下面的逐格 DrawTextW
            const bool planned = I.glyph_path && I.iso_plan.usableFor(I.last_iso, rr);
            if (planned && text_visible(c, now)) {
                draw_plan(I, metrics_, cr, I.iso_plan.run, *I.iso_plan.plan,
                          resolve_cell(I.theme, c, rev != 0, search_hl_.kind(cr, dc)).bg);
            } else if (g && glen && text_visible(c, now)) {
                int wl = MultiByteToWideChar(CP_UTF8, 0, g, int(glen), nullptr, 0);
                std::wstring gs(wl, 0);
                MultiByteToWideChar(CP_UTF8, 0, g, int(glen), gs.data(), wl);
                auto rc = metrics_.cell_rect(dc, cr, wide ? 2 : 1);
                D2D1_RECT_F r = D2D1::RectF(float(rc.x), float(rc.y), float(rc.x + rc.w), float(rc.y + rc.h));
                auto st = text_style(c.flags, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
                I.brush_fg->SetColor(rgb_f(resolve_cell(I.theme, c, rev != 0, search_hl_.kind(cr, dc)).bg));
                I.d2d->DrawTextW(gs.c_str(), UINT32(gs.size()), I.formats[st.format_index()], r, I.brush_fg,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
    }
    hr = I.d2d->EndDraw();
    if (is_lost(hr)) { device_lost_ = true; return false; }
    {
        const UINT sync = (pace_action == PresentPace::Action::PresentWait) ? 1u : 0u;
        const UINT flags = (pace_action == PresentPace::Action::PresentNoWait) ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        hr = I.swap->Present(sync, flags);
    }
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        I.dirty.invalidate();  // 未翻到前缓冲：整屏标脏，下次再出
        return false;
    }
    if (is_lost(hr)) { device_lost_ = true; return false; }
    pace_.note_presented(now_ms_);
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
void D2DRenderer::set_search_highlight(SearchHighlight hl) {
    search_dirty_ = SearchHighlight::dirty_rows(search_hl_, hl);
    search_hl_ = std::move(hl);
}
void D2DRenderer::clear_search_highlight() {
    SearchHighlight empty = search_hl_;
    empty.clear();
    search_dirty_ = SearchHighlight::dirty_rows(search_hl_, empty);
    search_hl_ = std::move(empty);
}
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
bool D2DRenderer::present_frame(PresentMode) { return false; }
void D2DRenderer::set_ligatures(bool) {}
bool D2DRenderer::recover_device() { device_lost_ = false; return true; }
void D2DRenderer::shutdown_device() {}

#endif

}  // namespace itren
