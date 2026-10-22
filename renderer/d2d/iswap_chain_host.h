#pragma once
// ISwapChainHost：渲染器与窗口解耦（DESIGN-T06 §1.1）。
// WIN32 专用；Linux 宿主可提供空实现以便头文件被包含（原型编译门闸）。
#include <cstdint>
#ifdef _WIN32
#  include <dxgi1_2.h>
#  include <d3d11.h>
#endif

namespace itren {

struct SizePx { int w = 0, h = 0; };

class ISwapChainHost {
public:
    virtual ~ISwapChainHost() = default;
#ifdef _WIN32
    // 宿主创建交换链（HWND → CreateSwapChainForHwnd；WinUI → ForComposition）
    virtual IDXGISwapChain1 *create_swap_chain(IDXGIFactory2 *factory, ID3D11Device *device,
                                              const DXGI_SWAP_CHAIN_DESC1 &desc) = 0;
#endif
    virtual SizePx client_size_px() const = 0;
    virtual float dpi_scale() const = 0;  // 96dpi = 1.0
};

}  // namespace itren
