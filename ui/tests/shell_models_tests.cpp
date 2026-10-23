#include "uicfg/shell_models.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg;
int main() {
  // 分隔条：左右分，右侧再上下分
  LayoutNode root = LayoutNode::split(SplitAxis::Columns, 0.5, LayoutNode::pane("a", ""),
      LayoutNode::split(SplitAxis::Rows, 0.5, LayoutNode::pane("b", ""), LayoutNode::pane("c", "")));
  Rect area{0, 0, 1000, 600};
  auto ds = computeDividers(root, area, 6);
  CHECK(ds.size() == 2);
  CHECK(ds[0].path.empty() && ds[0].bar.x == 497);
  CHECK(ds[1].path == std::vector<int>{1} && ds[1].axis == SplitAxis::Rows && ds[1].bar.y == 297 && ds[1].bar.x == 500);
  CHECK(hitDivider(ds, 499, 100) == size_t(0));
  CHECK(hitDivider(ds, 700, 299) == size_t(1));
  CHECK(!hitDivider(ds, 100, 100));
  CHECK(!hitDivider(ds, 200, 299));  // 左侧没有横分隔条
  CHECK(nodeAtPath(root, {1}) && nodeAtPath(root, {1})->axis == SplitAxis::Rows);
  CHECK(nodeAtPath(root, {1, 0})->paneId == "b" && !nodeAtPath(root, {0, 0}));
  // 拖动：cell 8x16，最小 20 列 = 160px，右侧子树（上下分）最小宽 160
  CHECK(dragRatio(root, area, 300, 0, 8, 16) == 0.3);
  CHECK(dragRatio(root, area, 50, 0, 8, 16) == 0.16);
  CHECK(dragRatio(root, area, 990, 0, 8, 16) == 0.84);
  // 上下：最小 5 行 = 80px，区域高 600
  auto* sub = nodeAtPath(root, {1});
  CHECK(dragRatio(*sub, ds[1].splitArea, 0, 10, 8, 16) == 80.0 / 600);
  // 太小：保持原比例
  CHECK(dragRatio(root, {0, 0, 200, 600}, 50, 0, 8, 16) == 0.5);
  // 标签放置
  std::vector<std::pair<double, double>> sp{{0, 100}, {100, 180}, {180, 300}};
  CHECK(tabDropIndex(sp, -5) == size_t(0) && tabDropIndex(sp, 150) == size_t(1) && tabDropIndex(sp, 999) == size_t(2));
  CHECK(!tabDropIndex({}, 1));
  CHECK(!isDragStart(3, -3, 4) && isDragStart(5, 0, 4));
  // 滚动条
  auto sb = scrollBarFor(0, 100, 24);
  CHECK(sb.enabled && sb.max == 123 && sb.page == 24 && sb.pos == 100);
  CHECK(scrollBarFor(30, 100, 24).pos == 70);
  CHECK(!scrollBarFor(0, 0, 24).enabled);
  CHECK(scrollLinesForThumb(70, 0, 100) == -30);   // 向上进入历史
  CHECK(scrollLinesForThumb(100, 30, 100) == 30);  // 回到底部
  CHECK(scrollLinesForThumb(-50, 0, 100) == -100);
  // 树文案
  TreeGroup g{"g1", "生产", {{"c1", "web", "10.0.0.1", ConnectionKind::Ssh, 2}, {"c2", "db", "", ConnectionKind::Ssh, 0}}};
  CHECK(runningCount(g) == 2 && groupLabel(g) == "生产 (2 运行中)");
  CHECK(groupLabel(TreeGroup{"", "x", {}}) == "未分组");
  CHECK(itemLabel(g.items[0]) == "● web · 10.0.0.1 (2)" && itemLabel(g.items[1]) == "db");
  // 字号
  CHECK(fontSizeFor("fontBigger", 12) == 13.0 && fontSizeFor("fontSmaller", 12.5) == 12.0);
  CHECK(fontSizeFor("fontBigger", 72) == 72.0 && fontSizeFor("fontSmaller", 6) == 6.0);
  CHECK(fontSizeFor("fontReset", 30) == 12.0 && !fontSizeFor("copy", 12));
  // 按键路由
  CHECK(routeKey("Up", true, false, false, false, false) == KeyRoute::PassToIme);
  CHECK(routeKey("Up", false, true, false, false, false) == KeyRoute::PassToIme);
  CHECK(routeKey("Up", false, false, false, false, false) == KeyRoute::SendKey);
  CHECK(routeKey("F12", false, false, true, false, false) == KeyRoute::SendKey);
  CHECK(routeKey("Q", false, false, true, true, true) == KeyRoute::Text);     // AltGr+Q = @
  CHECK(routeKey("Q", false, false, false, true, false) == KeyRoute::SendKey); // Alt+Q
  CHECK(routeKey("Q", false, false, false, false, false) == KeyRoute::Text);
  CHECK(routeKey("", false, false, false, false, false) == KeyRoute::Text);
  CHECK(isSpecialKey("Numpad5") && routeKey("Char", false, false, false, true, false) == KeyRoute::SendKey);
  CHECK(!isSpecialKey("F25") && isSpecialKey("F1") && !isSpecialKey("Q"));
  CHECK(keyMods(true, true, true) == 7 && keyMods(false, true, false) == 1);
  // 旧 attempt 面板
  auto l0 = attemptPanelLayout(600, false, 1);
  CHECK(l0.panelH == 0 && l0.termY == 0 && l0.termH == 600);
  auto l1 = attemptPanelLayout(600, true, 1);
  CHECK(l1.panelH == 200 && l1.sepH == 20 && l1.termY == 220 && l1.termH == 380);
  auto l2 = attemptPanelLayout(100, true, 1);
  CHECK(l2.panelH == 35 && l2.termH == 45);
  CHECK(attemptHeader(3) == "上一次 attempt #3（只读）");
  if (fails) { std::printf("%d failures\n", fails); return 1; }
  std::printf("shell_models ok\n"); return 0;
}
