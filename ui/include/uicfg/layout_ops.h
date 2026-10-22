#pragma once
// 第 12.3 / 12.4：与外壳无关的布局操作（分屏、关闭、焦点移动、分隔条限制）
#include <optional>
#include <string>
#include <vector>
#include "uicfg/workspace.h"

namespace uicfg {

struct Rect { double x = 0, y = 0, w = 0, h = 0; };
struct PaneRect { std::string paneId; Rect rect; };

enum class Direction { Left, Right, Up, Down };

// 把 activePane 一分为二，新 pane 使用同一连接（启动独立 Session），返回新 paneId
std::optional<std::string> splitPane(Tab& tab, const std::string& paneId, SplitAxis axis,
                                     const std::string& newPaneId);

// 关闭 pane：兄弟节点上提；最后一个 pane 返回 false（调用方关闭标签）
bool closePane(Tab& tab, const std::string& paneId);

// 计算每个 pane 的像素矩形
std::vector<PaneRect> computeRects(const LayoutNode& root, Rect area);

// Alt+方向键：从 from 出发找该方向上最近且重叠的 pane
std::optional<std::string> neighborPane(const LayoutNode& root, Rect area,
                                        const std::string& from, Direction dir);

// 拖动分隔条：限制 ratio 使两侧至少 minCols×minRows 网格（cell 像素尺寸给定）；
// 窗口太小时保留原比例不改树（第 13.3）
double clampRatio(const LayoutNode& split, Rect area, double desired,
                  double cellW, double cellH, int minCols = 20, int minRows = 5);

}  // namespace uicfg
