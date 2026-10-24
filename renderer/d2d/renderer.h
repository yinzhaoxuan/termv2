#pragma once
// Direct2D/DirectWrite 网格渲染原型（DESIGN-T06）。
// 字体回退不改变列数：字形 advance 不参与定位，簇强制锚在 cell_rect（14.2）。
#include "iswap_chain_host.h"
#include "grid_metrics.h"
#include "itcore.h"
#include "ime_layout.h"
#include "search_highlight.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

namespace itren {

struct Preedit {
    std::string utf8;       // 本地预编辑，不进快照
    uint16_t anchor_col = 0, anchor_row = 0;
    std::vector<uint8_t> widths;   // 每个 cluster 的列宽（1/2），由 fill_preedit_clusters 按核心当前屏幕填
    std::vector<uint32_t> byte_lens;  // 每个 cluster 的 UTF-8 字节数（与 widths 同长）；空则按码点逐个切
    std::vector<uint8_t> attrs;    // ClauseAttr；可短于 widths
    size_t cursor = SIZE_MAX;      // 预编辑内光标（cluster 下标）；SIZE_MAX = 末尾
};

// 按核心 c 的当前屏幕（主/备用屏 VS16 规则不同）填 widths / byte_lens（itcore_text_clusters）
void fill_preedit_clusters(Preedit &p, const itcore_core *c);
// 预编辑各簇列宽与 UTF-8 切分；widths/byte_lens 为空时退回每码点一簇、宽 1
std::vector<uint8_t> preedit_widths(const Preedit &p);
std::vector<std::string> preedit_clusters(const Preedit &p);
// 预编辑几何（下划线 / 内部光标 / 候选窗锚点），平台无关
ImeLayout preedit_layout(const GridMetrics &m, const Preedit &p, uint16_t cols, uint16_t rows);

// 当前 D3D 设备类型（UI 状态栏显示）
enum class DeviceKind { None, Hardware, Warp, Stub };
const char *device_kind_name(DeviceKind k);

class D2DRenderer {
public:
    explicit D2DRenderer(ISwapChainHost *host);
    ~D2DRenderer();

    // 设备/交换链/图集；失败返回 false（设备丢失时上层重调）
    bool init(const GridMetrics &metrics, const wchar_t *font_family, float font_size_pt);
    // 客户区尺寸变化：ResizeBuffers + 重建 D2D target（保留设备与字体）
    bool resize_surface();
    // 运行时换字体：重建 4 个 TextFormat、清 advance 缓存、整屏重画。失败保持旧字体并返回 false
    bool set_font(const wchar_t *font_family, float font_size_pt);
    // 当前主字体度量（DIP）；无 DWrite 时按字号估算
    FontMetricsDip font_metrics() const;
    void shutdown();

    // 只读快照；generation 不匹配时保持旧布局、多出区域填背景
    void set_snapshot(const itcore_snapshot *snap);
    void set_expected_generation(uint64_t gen) { expected_gen_ = gen; }
    void set_preedit(const Preedit &p) { preedit_ = p; }
    void clear_preedit() { preedit_ = {}; }
    // 光标：失焦画空心框；输入后 530ms 内常亮（tick_ms 为单调毫秒，如 GetTickCount64）
    void set_focused(bool f) { focused_ = f; }
    void note_input(uint64_t tick_ms) { last_input_ms_ = tick_ms; }
    void set_clock(uint64_t tick_ms) { now_ms_ = tick_ms; }
    // 搜索高亮（草案 §2.6）：每帧由后端把 PaneSearch::ranges() 转成 SearchHighlight 后传入；
    // 与上一帧比出脏行，present_frame 只重画那些行。clear / RESTARTED 调 clear_search_highlight。
    void set_search_highlight(SearchHighlight hl);
    void clear_search_highlight();
    // 动画（光标闪烁 / SGR 5）需要的下一次重绘时间点；0 = 不需要
    bool wants_animation() const { return animating_; }

    // 脏行重绘 + Present；无变化返回 false（跳过 Present）
    bool present_frame();

    // IME 候选窗 / TSF NotifyLayoutChanged：有预编辑时为 composition_anchor（组字起点 / Target 子句），不随组字右移
    GridMetrics::Rect caret_rect_px() const;

    GridMetrics &metrics() { return metrics_; }
    const GridMetrics &metrics() const { return metrics_; }

    // 设备丢失标志（DXGI_ERROR_DEVICE_REMOVED/RESET）
    bool device_lost() const { return device_lost_; }
    // 设备丢失后重建（present_frame 内自动调用；宿主也可在 WM_DISPLAYCHANGE 等时机主动调用）
    bool recover_device();
    // init 成功后为 Hardware 或 WARP；未初始化/失败为 None；非 WIN32 为 Stub
    DeviceKind device_kind() const { return device_kind_; }

private:
    void shutdown_device();
    struct Impl;
    static bool create_formats(Impl &I, const std::wstring &family, float size_pt, float scale = 0.f, int cell_h_px = 0, float baseline_px = 0.f);
    ISwapChainHost *host_ = nullptr;
    GridMetrics metrics_{};
    const itcore_snapshot *snap_ = nullptr;  // 非拥有
    uint64_t expected_gen_ = 0;
    uint64_t last_drawn_revision_ = 0;
    Preedit preedit_{};
    SearchHighlight search_hl_;                 // 视口高亮表；无活动搜索时为空（any()==false）
    std::vector<uint16_t> search_dirty_;        // set/clear 时相对上一帧的脏行，present_frame 消费
    bool focused_ = true, animating_ = false;
    uint64_t last_input_ms_ = 0, now_ms_ = 0;
    bool device_lost_ = false;
    float stub_size_pt_ = 12.f;  // 非 WIN32 桩：font_metrics 估算用
    DeviceKind device_kind_ = DeviceKind::None;
    std::unique_ptr<Impl> impl_;
};

}  // namespace itren
