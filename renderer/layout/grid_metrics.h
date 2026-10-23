/* 格坐标与像素换算（与平台无关，渲染、IME 候选窗、鼠标命中、resize 共用这一处）。
   列宽来自字体主字形的整数化 advance，与 DirectWrite 回退字体无关：字体回退不改变列数（14.2）。 */
#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace itren {
// 主字体度量（DIP）：DirectWrite 实测（D2DRenderer::font_metrics）或无设备时按字号估算
struct FontMetricsDip { float advance = 7.2f, ascent = 10.f, descent = 3.f, line_gap = 0.f; };
// 估算：12 pt 时为 7.2 / 10 / 3 / 0（与改动前的固定值一致），按字号线性缩放
inline FontMetricsDip estimate_font_metrics(float size_pt) {
  float k = (size_pt > 0 ? size_pt : 12.f) / 12.f;
  return {7.2f * k, 10.f * k, 3.f * k, 0.f};
}
struct GridMetrics {
  float dpi_scale = 1.0f;     // 96 DPI = 1.0
  float cell_w_dip = 0, cell_h_dip = 0;  // 设备无关像素
  float pad_x_dip = 4, pad_y_dip = 4;

  // 由主字体度量（DIP，em 尺寸下）求格尺寸，按物理像素取整避免累计漂移（DPI-01）
  static GridMetrics from_font(float advance_dip, float ascent_dip, float descent_dip, float line_gap_dip, float dpi_scale) {
    GridMetrics m; m.dpi_scale = dpi_scale;
    m.cell_w_dip = std::max(1.0f, std::round(advance_dip * dpi_scale)) / dpi_scale;
    m.cell_h_dip = std::max(1.0f, std::ceil((ascent_dip + descent_dip + line_gap_dip) * dpi_scale)) / dpi_scale;
    return m;
  }
  static GridMetrics from_font(const FontMetricsDip &f, float dpi_scale) {
    return from_font(f.advance, f.ascent, f.descent, f.line_gap, dpi_scale);
  }
  int cell_w_px() const { return int(std::lround(cell_w_dip * dpi_scale)); }
  int cell_h_px() const { return int(std::lround(cell_h_dip * dpi_scale)); }
  int pad_x_px() const { return int(std::lround(pad_x_dip * dpi_scale)); }
  int pad_y_px() const { return int(std::lround(pad_y_dip * dpi_scale)); }

  struct Rect { int x, y, w, h; };
  // 格 -> 客户区物理像素；width 为占格数（宽字符 2）
  Rect cell_rect(uint16_t col, uint16_t row, uint8_t width = 1) const {
    return {pad_x_px() + col * cell_w_px(), pad_y_px() + row * cell_h_px(), cell_w_px() * std::max<uint8_t>(width, 1), cell_h_px()};
  }
  // 像素 -> 格（鼠标命中），钳制到网格范围
  void hit_test(int px, int py, uint16_t cols, uint16_t rows, uint16_t &col, uint16_t &row) const {
    int c = (px - pad_x_px()) / std::max(1, cell_w_px());
    int r = (py - pad_y_px()) / std::max(1, cell_h_px());
    col = uint16_t(std::clamp(c, 0, cols - 1));
    row = uint16_t(std::clamp(r, 0, rows - 1));
  }
  // 客户区像素尺寸 -> 行列，最小 20x5（与 Windows 系统约定）
  void grid_size(int client_w_px, int client_h_px, uint16_t &cols, uint16_t &rows) const {
    int c = (client_w_px - 2 * pad_x_px()) / std::max(1, cell_w_px());
    int r = (client_h_px - 2 * pad_y_px()) / std::max(1, cell_h_px());
    cols = uint16_t(std::clamp(c, 20, 65535));
    rows = uint16_t(std::clamp(r, 5, 65535));
  }
};
}  // namespace itren
