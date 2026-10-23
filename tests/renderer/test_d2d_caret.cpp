#undef NDEBUG  // 真机包是 Release 构建，assert 不能被编译掉
#include "renderer.h"
#include "itcore_mock.h"
#include <cstdio>
#include <cassert>

struct NullHost : itren::ISwapChainHost {
#ifdef _WIN32
  IDXGISwapChain1 *create_swap_chain(IDXGIFactory2 *f, ID3D11Device *dev, const DXGI_SWAP_CHAIN_DESC1 &desc) override {
    // 无窗口：用 composition 交换链，真机（含 Hyper-V 基本显示适配器 + WARP）上 init 可完整走通
    IDXGISwapChain1 *sc = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d = desc;
    d.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(f->CreateSwapChainForComposition(dev, &d, nullptr, &sc))) return nullptr;
    return sc;
  }
#endif
  itren::SizePx client_size_px() const override { return {800, 600}; }
  float dpi_scale() const override { return 1.0f; }
};

int main() {
  NullHost host;
  itren::D2DRenderer r(&host);
  auto m = itren::GridMetrics::from_font(8, 10, 2, 0, 1.0f);
  assert(r.init(m, nullptr, 12.f));
  const char *txt = "AB";
  auto *s = itmock_snapshot_from_utf8(txt, 2, 40, 10);
  r.set_snapshot(s);
  auto rc = r.caret_rect_px();
  assert(rc.w == m.cell_w_px() && rc.h == m.cell_h_px());
  itren::Preedit pe; pe.utf8 = "测"; pe.anchor_col = 3; pe.anchor_row = 1;
  r.set_preedit(pe);
  rc = r.caret_rect_px();
  assert(rc.x == m.cell_rect(3, 1, 1).x);
  assert(rc.y == m.cell_rect(3, 1, 1).y);
  itcore_snapshot_release(s);
  r.shutdown();
  std::puts("ok");
  return 0;
}
