#include <cstdio>
#include "uicfg/launch.h"
using namespace uicfg;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  const EnvVars parent{{"z", "last"}, {"Path", "old"}, {"SystemRoot", "D:\\Windows"},
                   {"SystemDrive", "D:"}, {"WINDIR", "D:\\Windows"},
                   {"=C:", "C:\\项目"}, {"a", "first"}};
  LocalEnvironment env;
  CHECK(!composeEnvironment(env, parent));
  CHECK(!composeEnvironment(env, {}));
  env.unset = {"PATH", "Z"};
  CHECK((composeEnvironment(env, parent) == EnvVars{{"=C:", "C:\\项目"}, {"a", "first"},
         {"SystemDrive", "D:"}, {"SystemRoot", "D:\\Windows"}, {"WINDIR", "D:\\Windows"}}));
  env.set = {{"pAtH", "中文 新值"}, {"B", "second"}};
  CHECK((composeEnvironment(env, parent) == EnvVars{{"=C:", "C:\\项目"}, {"a", "first"}, {"B", "second"},
         {"pAtH", "中文 新值"}, {"SystemDrive", "D:"}, {"SystemRoot", "D:\\Windows"}, {"WINDIR", "D:\\Windows"}}));
  env = {}; env.inherit = false;
  CHECK((composeEnvironment(env, parent) == EnvVars{{"SystemDrive", "D:"}, {"SystemRoot", "D:\\Windows"}, {"WINDIR", "D:\\Windows"}}));
  env.unset = {"SYSTEMROOT", "systemdrive", "windir"};
  env.set = {{"sysTEMroot", "E:\\Windows"}, {"only", ""}};
  CHECK((composeEnvironment(env, parent) == EnvVars{{"only", ""}, {"sysTEMroot", "E:\\Windows"}}));
  env.set.clear();
  CHECK((composeEnvironment(env, parent) == EnvVars{{"SystemRoot", "D:\\Windows"}}));
  CHECK((composeEnvironment(env, {}) == EnvVars{{"SystemRoot", "C:\\Windows"}}));
  env = {}; env.unset = {"anything"};
  CHECK((composeEnvironment(env, {}) == EnvVars{{"SystemRoot", "C:\\Windows"}}));
  CHECK((composeEnvironment(env, {{"path", "old"}, {"PATH", "new"}}) == EnvVars{{"PATH", "new"}}));
  std::printf(fails ? "%d failed\n" : "all passed\n", fails);
  return fails != 0;
}
