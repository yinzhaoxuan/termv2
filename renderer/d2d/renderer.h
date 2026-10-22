#pragma once
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 字体回退不改变列数：字形 advance 不参与定位，簇强制锚在 cell_rect（14.2）。
#include "iswap_chain_host.h"
#include "grid_metrics.h"
#include "itcore.h"
#include <cstdint>
#include <memory>
#include <string>

namespace itren {

struct Preedit {
    std::string utf8;       // 本地预编辑，不进快照
    uint16_t anchor_col = 0, anchor_row = 0;
};

class D2DRenderer {
public:
    explicit D2DRenderer(ISwapChainHost *host);
    ~D2DRenderer();

    // 设备/交换链/图集；失败返回 false（设备丢失时上层重调）
    bool init(const GridMetrics &metrics, const wchar_t *font_family, float font_size_pt);
    // 客户区尺寸变化：ResizeBuffers + 重建 D2D target（保留设备与字体）
    bool resize_surface();
    void shutdown();

    // 只读快照；generation 不匹配时保持旧布局、多出区域填背景
    void set_snapshot(const itcore_snapshot *snap);
    void set_expected_generation(uint64_t gen) { expected_gen_ = gen; }
    void set_preedit(const Preedit &p) { preedit_ = p; }
    void clear_preedit() { preedit_ = {}; }

    // 脏行重绘 + Present；无变化返回 false（跳过 Present）
    bool present_frame();

    // IME 候选窗 / TSF NotifyLayoutChanged
    GridMetrics::Rect caret_rect_px() const;

    GridMetrics &metrics() { return metrics_; }
    const GridMetrics &metrics() const { return metrics_; }

    // 设备丢失标志（DXGI_ERROR_DEVICE_REMOVED/RESET）
    bool device_lost() const { return device_lost_; }

private:
    struct Impl;
    ISwapChainHost *host_ = nullptr;
    GridMetrics metrics_{};
    const itcore_snapshot *snap_ = nullptr;  // 非拥有
    uint64_t expected_gen_ = 0;
    uint64_t last_drawn_revision_ = 0;
    Preedit preedit_{};
    bool device_lost_ = false;
    std::unique_ptr<Impl> impl_;
};

}  // namespace itren
