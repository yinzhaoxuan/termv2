#include "uicfg/layout_ops.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace uicfg {
namespace {

LayoutNode* findPane(LayoutNode& n, const std::string& id, LayoutNode** parent, LayoutNode* p = nullptr) {
  if (n.kind == LayoutNode::Kind::Pane) {
    if (n.paneId == id) { if (parent) *parent = p; return &n; }
    return nullptr;
  }
  if (auto* r = findPane(*n.first, id, parent, &n)) return r;
  return findPane(*n.second, id, parent, &n);
}

void collect(const LayoutNode& n, Rect a, std::vector<PaneRect>& out) {
  if (n.kind == LayoutNode::Kind::Pane) { out.push_back({n.paneId, a}); return; }
  if (n.axis == SplitAxis::Columns) {
    double w1 = a.w * n.ratio;
    collect(*n.first, {a.x, a.y, w1, a.h}, out);
    collect(*n.second, {a.x + w1, a.y, a.w - w1, a.h}, out);
  } else {
    double h1 = a.h * n.ratio;
    collect(*n.first, {a.x, a.y, a.w, h1}, out);
    collect(*n.second, {a.x, a.y + h1, a.w, a.h - h1}, out);
  }
}

// 子树在某轴方向上能接受的最小像素（递归考虑嵌套分屏）
double minExtent(const LayoutNode& n, bool horizontal, double cellW, double cellH, int minCols, int minRows) {
  if (n.kind == LayoutNode::Kind::Pane) return horizontal ? minCols * cellW : minRows * cellH;
  double a = minExtent(*n.first, horizontal, cellW, cellH, minCols, minRows);
  double b = minExtent(*n.second, horizontal, cellW, cellH, minCols, minRows);
  bool sameAxis = (n.axis == SplitAxis::Columns) == horizontal;
  return sameAxis ? a + b : std::max(a, b);
}

}  // namespace

std::optional<std::string> splitPane(Tab& tab, const std::string& paneId, SplitAxis axis,
                                     const std::string& newPaneId) {
  LayoutNode* parent = nullptr;
  LayoutNode* target = findPane(tab.root, paneId, &parent);
  if (!target) return std::nullopt;
  std::vector<std::string> ids;
  for (auto& r : computeRects(tab.root, {0, 0, 1, 1})) ids.push_back(r.paneId);
  if (std::find(ids.begin(), ids.end(), newPaneId) != ids.end()) return std::nullopt;

  LayoutNode oldPane = std::move(*target);
  LayoutNode fresh = LayoutNode::pane(newPaneId, oldPane.connectionId);  // 同连接、独立 Session
  *target = LayoutNode::split(axis, 0.5, std::move(oldPane), std::move(fresh));
  tab.activePaneId = newPaneId;
  return newPaneId;
}

bool closePane(Tab& tab, const std::string& paneId) {
  if (tab.root.kind == LayoutNode::Kind::Pane) return false;  // 最后一个
  LayoutNode* parent = nullptr;
  if (!findPane(tab.root, paneId, &parent) || !parent) return false;
  bool isFirst = parent->first->kind == LayoutNode::Kind::Pane && parent->first->paneId == paneId;
  LayoutNode sibling = std::move(isFirst ? *parent->second : *parent->first);
  *parent = std::move(sibling);
  if (tab.activePaneId == paneId) {
    auto rects = computeRects(tab.root, {0, 0, 1, 1});
    tab.activePaneId = rects.empty() ? "" : rects.front().paneId;
  }
  return true;
}

std::vector<PaneRect> computeRects(const LayoutNode& root, Rect area) {
  std::vector<PaneRect> out;
  collect(root, area, out);
  return out;
}

std::optional<std::string> neighborPane(const LayoutNode& root, Rect area,
                                        const std::string& from, Direction dir) {
  auto rects = computeRects(root, area);
  auto it = std::find_if(rects.begin(), rects.end(), [&](auto& r) { return r.paneId == from; });
  if (it == rects.end()) return std::nullopt;
  const Rect s = it->rect;
  const double eps = 1e-6;
  std::optional<std::string> best;
  double bestDist = std::numeric_limits<double>::max();
  for (const auto& r : rects) {
    if (r.paneId == from) continue;
    const Rect& c = r.rect;
    bool overlapV = c.y < s.y + s.h - eps && c.y + c.h > s.y + eps;
    bool overlapH = c.x < s.x + s.w - eps && c.x + c.w > s.x + eps;
    double d = -1;
    switch (dir) {
      case Direction::Left:  if (overlapV && c.x + c.w <= s.x + eps) d = s.x - (c.x + c.w); break;
      case Direction::Right: if (overlapV && c.x >= s.x + s.w - eps) d = c.x - (s.x + s.w); break;
      case Direction::Up:    if (overlapH && c.y + c.h <= s.y + eps) d = s.y - (c.y + c.h); break;
      case Direction::Down:  if (overlapH && c.y >= s.y + s.h - eps) d = c.y - (s.y + s.h); break;
    }
    if (d < 0) continue;
    // 距离相同则取与源中心更近者
    double tie = (dir == Direction::Left || dir == Direction::Right)
                     ? std::abs((c.y + c.h / 2) - (s.y + s.h / 2))
                     : std::abs((c.x + c.w / 2) - (s.x + s.w / 2));
    double score = d * 1e6 + tie;
    if (score < bestDist) { bestDist = score; best = r.paneId; }
  }
  return best;
}

double clampRatio(const LayoutNode& split, Rect area, double desired,
                  double cellW, double cellH, int minCols, int minRows) {
  if (split.kind != LayoutNode::Kind::Split) return desired;
  bool horizontal = split.axis == SplitAxis::Columns;
  double total = horizontal ? area.w : area.h;
  double minA = minExtent(*split.first, horizontal, cellW, cellH, minCols, minRows);
  double minB = minExtent(*split.second, horizontal, cellW, cellH, minCols, minRows);
  if (total <= 0 || minA + minB > total) return split.ratio;  // 窗口过小：不改保存布局
  double lo = minA / total, hi = 1.0 - minB / total;
  return std::clamp(desired, lo, hi);
}

}  // namespace uicfg
