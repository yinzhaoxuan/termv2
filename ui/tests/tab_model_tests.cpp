#include "uicfg/tab_model.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg;
int main() {
  int n = 0; TabBar tb([&] { return "p" + std::to_string(++n); });
  CHECK(tb.empty() && !tb.activeIndex() && !tb.activeTab());
  tb.next(); tb.prev();  // 空时不崩
  Connection c; c.id = "c1"; c.name = "Ubuntu";
  CHECK(tb.openConnection(c) == 0);
  CHECK(tb.openConnection(c) == 1);  // 双击同一连接：总是新标签
  CHECK(tb.count() == 2 && tb.tabs()[0].activePaneId != tb.tabs()[1].activePaneId);
  CHECK(tb.tabs()[1].root.connectionId == "c1");
  tb.newTab("", "PowerShell");  // 下标 2
  CHECK(*tb.activeIndex() == 2);
  tb.next(); CHECK(*tb.activeIndex() == 0);  // 回绕
  tb.prev(); CHECK(*tb.activeIndex() == 2);
  tb.activate(0); tb.newTab("", "X");  // 插在当前之后
  CHECK(*tb.activeIndex() == 1 && tb.tabs()[1].title == "X" && tb.count() == 4);
  CHECK(tb.move(1, 3) && *tb.activeIndex() == 3 && tb.tabs()[3].title == "X");
  tb.activate(2); CHECK(tb.move(0, 3) && *tb.activeIndex() == 1);
  tb.activate(1); CHECK(tb.move(3, 0) && *tb.activeIndex() == 2);
  CHECK(!tb.move(0, 9));
  std::string t = tb.activeTab()->title;
  tb.activate(0); CHECK(tb.closeTab(0) && *tb.activeIndex() == 0);
  tb.activate(2); CHECK(tb.closeTab(2) && *tb.activeIndex() == 1);  // 最后一个 → 前一个
  tb.activate(1); CHECK(tb.closeTab(0) && *tb.activeIndex() == 0);  // 左侧关闭，活动跟随
  CHECK(!tb.closeTab(5));
  auto pid = tb.activeTab()->activePaneId;
  CHECK(tb.titleFor(0, [&](const std::string& p) -> std::optional<std::string> { if (p == pid) return std::string("vim"); return std::nullopt; }) == "vim");
  CHECK(tb.titleFor(0, [](const std::string&) -> std::optional<std::string> { return std::nullopt; }) == tb.tabs()[0].title);
  CHECK(tb.closeTab(0) && tb.empty());
  (void)t;
  if (!fails) std::printf("tab_model_tests OK\n");
  return fails ? 1 : 0;
}
