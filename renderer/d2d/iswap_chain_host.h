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

// 交换链呈现模型（BUG-02）。Flip 模型挂在子窗口上时由 DWM 单独合成，在 Hyper-V 增强会话/RDP 下
// 会盖住其他顶层窗口（如模态对话框）；子窗口宿主改用 blt 模型 Discard + 1 缓冲。
// 渲染器每帧整张重画 canvas，blt 模型没有副作用。
enum class SwapEffect { FlipSequential, Discard };
struct SwapConfig {
    SwapEffect effect = SwapEffect::FlipSequential;
    unsigned buffer_count = 2;
};
inline constexpr SwapConfig kDefaultSwapConfig{SwapEffect::FlipSequential, 2};
inline constexpr SwapConfig kChildHwndSwapConfig{SwapEffect::Discard, 1};

class ISwapChainHost {
public:
    virtual ~ISwapChainHost() = default;
#ifdef _WIN32
    // 宿主创建交换链（HWND → CreateSwapChainForHwnd；WinUI → ForComposition）
    virtual IDXGISwapChain1 *create_swap_chain(IDXGIFactory2 *factory, ID3D11Device *device,
                                              const DXGI_SWAP_CHAIN_DESC1 &desc) = 0;
#endif
    virtual SwapConfig swap_config() const { return kDefaultSwapConfig; }
    virtual SizePx client_size_px() const = 0;
    virtual float dpi_scale() const = 0;  // 96dpi = 1.0
};

}  // namespace itren
