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
  if (!r.init(m, nullptr, 12.f)) {
    // 硬件与 WARP 都失败（HRESULT 已打到 stderr）才 SKIP
    std::puts("SKIP: D2D init failed on hardware and WARP");
    return 77;
  }
  std::printf("device=%s\n", itren::device_kind_name(r.device_kind()));
#ifdef _WIN32
  assert(r.device_kind() == itren::DeviceKind::Hardware || r.device_kind() == itren::DeviceKind::Warp);
#endif
  const char *txt = "AB";
  auto *s = itmock_snapshot_from_utf8(txt, 2, 40, 10);
  r.set_snapshot(s);
  auto rc = r.caret_rect_px();
  assert(rc.w == m.cell_w_px() && rc.h == m.cell_h_px());
  itren::Preedit pe; pe.utf8 = "测"; pe.anchor_col = 3; pe.anchor_row = 1;
  r.set_preedit(pe);
  rc = r.caret_rect_px();
  // IME-01：候选窗锚点 = composition_anchor（组字起点第 3 列），不随“测”右移到第 5 列
  assert(rc.x == m.cell_rect(3, 1, 1).x);
  assert(rc.y == m.cell_rect(3, 1, 1).y);
  pe.utf8 = "测试"; r.set_preedit(pe);
  rc = r.caret_rect_px(); assert(rc.x == m.cell_rect(3, 1, 1).x);
  // 预编辑宽度按 itcore_text_clusters（此处链接 mock）
  auto pw = itren::preedit_widths(pe); assert(pw.size() == 2 && pw[0] == 2 && pw[1] == 2);
  itcore_snapshot_release(s);
  r.shutdown();
  std::puts("ok");
  return 0;
}
