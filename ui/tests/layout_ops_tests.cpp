#include <cstdio>
#include "uicfg/layout_ops.h"
using namespace uicfg;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  Tab t; t.title = "Ubuntu"; t.root = LayoutNode::pane("a", "conn-1"); t.activePaneId = "a";
  // 左右分屏
  CHECK(splitPane(t, "a", SplitAxis::Columns, "b") == std::string("b"));
  CHECK(t.activePaneId == "b");
  CHECK(t.root.kind == LayoutNode::Kind::Split);
  CHECK(t.root.second->connectionId == "conn-1");  // 同连接，独立 Session
  CHECK(!splitPane(t, "a", SplitAxis::Rows, "b"));  // 重复 id 拒绝
  // b 上下分
  CHECK(splitPane(t, "b", SplitAxis::Rows, "c"));
  Rect area{0, 0, 1000, 600};
  auto rects = computeRects(t.root, area);
  CHECK(rects.size() == 3);

  // 焦点移动：a 在左，b 右上，c 右下
  CHECK(neighborPane(t.root, area, "a", Direction::Right) == std::string("b"));
  CHECK(neighborPane(t.root, area, "b", Direction::Down) == std::string("c"));
  CHECK(neighborPane(t.root, area, "c", Direction::Up) == std::string("b"));
  CHECK(neighborPane(t.root, area, "c", Direction::Left) == std::string("a"));
  CHECK(!neighborPane(t.root, area, "a", Direction::Left));

  // 分隔条：cell 10×20 px → 左侧至少 200px；右侧至少 200px
  double r = clampRatio(t.root, area, 0.05, 10, 20);
  CHECK(r >= 0.2 - 1e-9);
  r = clampRatio(t.root, area, 0.99, 10, 20);
  CHECK(r <= 0.8 + 1e-9);
  CHECK(clampRatio(t.root, area, 0.5, 10, 20) == 0.5);
  // 窗口太小：保留原比例
  CHECK(clampRatio(t.root, {0, 0, 300, 600}, 0.1, 10, 20) == t.root.ratio);
  // 右侧是上下分，纵向需要 2×5 行
  double rr = clampRatio(*t.root.second, {500, 0, 500, 600}, 0.01, 10, 20);
  CHECK(rr >= 100.0 / 600 - 1e-9);

  // 关闭 c：b 上提
  CHECK(closePane(t, "c"));
  CHECK(computeRects(t.root, area).size() == 2);
  CHECK(t.activePaneId == "a" || t.activePaneId == "b");
  CHECK(closePane(t, "b"));
  CHECK(t.root.kind == LayoutNode::Kind::Pane && t.root.paneId == "a");
  CHECK(t.activePaneId == "a");
  CHECK(!closePane(t, "a"));  // 最后一个由调用方关标签

  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
