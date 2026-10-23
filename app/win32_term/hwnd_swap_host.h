#pragma once
// HWND → DXGI 交换链宿主（DESIGN-T06 §1.1）。仅 WIN32。
#ifndef _WIN32
#error "hwnd_swap_host 仅用于 Windows"
#endif
#include "iswap_chain_host.h"
#include <windows.h>

namespace iterm_app {

class HwndSwapChainHost final : public itren::ISwapChainHost {
public:
    explicit HwndSwapChainHost(HWND hwnd) : hwnd_(hwnd) {}

    HWND hwnd() const { return hwnd_; }

    IDXGISwapChain1 *create_swap_chain(IDXGIFactory2 *factory, ID3D11Device *device,
                                       const DXGI_SWAP_CHAIN_DESC1 &desc) override {
        if (!factory || !device || !hwnd_) return nullptr;
        IDXGISwapChain1 *swap = nullptr;
        HRESULT hr = factory->CreateSwapChainForHwnd(device, hwnd_, &desc, nullptr, nullptr, &swap);
        if (FAILED(hr)) return nullptr;
        // 禁用 Alt+Enter 全屏切换，由我们自己管客户区
        IDXGIFactory *f = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f))) && f) {
            f->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
            f->Release();
        }
        return swap;
    }

    // 子窗口宿主：blt 模型，避免 DWM 把交换链叠到对话框上面（BUG-02）
    itren::SwapConfig swap_config() const override { return itren::kChildHwndSwapConfig; }

    itren::SizePx client_size_px() const override {
        RECT rc{};
        if (!hwnd_ || !GetClientRect(hwnd_, &rc)) return {1, 1};
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        return {w > 0 ? w : 1, h > 0 ? h : 1};
    }

    float dpi_scale() const override {
        UINT dpi = 96;
        if (hwnd_) {
            // Per-Monitor V2：GetDpiForWindow（Win10 1607+）
            using Fn = UINT(WINAPI *)(HWND);
            static Fn fn = nullptr;
            if (!fn) {
                if (auto raw = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))
                    fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(raw));
            }
            if (fn) dpi = fn(hwnd_);
        }
        return dpi / 96.f;
    }

private:
    HWND hwnd_ = nullptr;
};

}  // namespace iterm_app
