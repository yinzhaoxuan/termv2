#include "uicfg/reconnect.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg;
int main() {
  PaneAttempts a(8);
  CHECK(a.attemptId() == 1 && !a.oldAttempt() && a.separator().empty());
  CHECK(a.reconnect("first") == 2);
  CHECK(a.oldAttempt()->attemptId == 1 && a.oldAttempt()->text == "first" && !a.separator().empty());
  CHECK(a.reconnect("second-output-long") == 3);
  CHECK(a.oldAttempt()->attemptId == 2 && a.oldAttempt()->text == "put-long");  // 只留一个，按预算截尾
  PaneAttempts u(2);
  u.reconnect("\xe4\xb8\xad" "a");  // "中a"，预算 2 字节落在续字节上，跳过不切断 UTF-8
  CHECK(u.oldAttempt()->text == "a");
  a.dropOldAttempt(); CHECK(!a.oldAttempt() && a.attemptId() == 3);
  if (!fails) std::printf("reconnect_tests OK\n");
  return fails ? 1 : 0;
}
