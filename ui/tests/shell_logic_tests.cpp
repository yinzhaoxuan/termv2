#include "uicfg/shell_logic.h"
#include "uicfg/settings.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace uicfg;
int main() {
  auto c = parseChord("Ctrl+Shift+T");
  CHECK(c && c->ctrl && c->shift && !c->alt && c->key == "T");
  CHECK(parseChord("ctrl+plus")->key == "Plus");
  CHECK(parseChord("Shift+PgUp")->key == "PageUp");
  CHECK(parseChord("Alt+Arrow")->key == "Arrow");
  CHECK(parseChord("F12")->key == "F12");
  CHECK(!parseChord("Hyper+T") && !parseChord("") && !parseChord("Ctrl+F99"));
  // 所有默认快捷键都能解析
  auto s = defaultSettings();
  for (auto& [a, k] : s.keybindings) { if (!parseChord(k)) std::printf("unparsed %s=%s\n", a.c_str(), k.c_str()); CHECK(parseChord(k)); }
  CHECK(matchKey(s.keybindings, {true, true, false, "T"}).action == "newTab");
  CHECK(matchKey(s.keybindings, {true, false, false, "Tab"}).action == "nextTab");
  CHECK(matchKey(s.keybindings, {true, true, false, "Tab"}).action == "prevTab");
  auto f = matchKey(s.keybindings, {false, false, true, "Left"});
  CHECK(f.action == "focusPane" && f.direction == Direction::Left);
  CHECK(matchKey(s.keybindings, {false, true, false, "PageUp"}).action == "scrollPageUp");
  CHECK(matchKey(s.keybindings, {true, false, false, "T"}).action.empty());  // 交给终端
  CHECK(matchKey(s.keybindings, {false, false, true, "T"}).action.empty());
  // 粘贴换行
  CHECK(normalizePasteText("a\r\nb\nc\rd") == "a\rb\rc\rd");
  CHECK(normalizePasteText(std::string("x\0y", 3)) == "x");
  // 链接
  auto l = linkPrompt("https://example.com/a");
  CHECK(l.canOpen && l.url == "https://example.com/a" && l.message.find("https://example.com/a") != std::string::npos);
  CHECK(!linkPrompt("file:///C:/x").canOpen && !linkPrompt("javascript:alert(1)").canOpen && !linkPrompt("http://a b").canOpen);
  // 状态覆盖层
  uishell::SessionStatus st{uishell::SessionState::Failed, 0, false, "", "认证失败"};
  auto o = overlayFor(st);
  CHECK(o.visible && o.showReconnect && o.text.find("认证失败") != std::string::npos);
  st.state = uishell::SessionState::Running; CHECK(!overlayFor(st).visible);
  CHECK(stateLabel(uishell::SessionState::Reconnecting) == "正在重连");
  CHECK(pasteRejectedMessage(40 * 1024, 32 * 1024).find("32") != std::string::npos);
  CHECK(vtForKey("Up", false, false, false) == "\x1b[A");
  CHECK(vtForKey("Left", true, false, false) == "\x1b[1;5D");
  CHECK(vtForKey("PageUp", false, false, false) == "\x1b[5~");
  CHECK(vtForKey("Delete", false, true, false) == "\x1b[3;2~");
  CHECK(vtForKey("F1", false, false, false) == "\x1bOP" && vtForKey("F5", false, false, false) == "\x1b[15~");
  CHECK(vtForKey("Tab", false, true, false) == "\x1b[Z" && vtForKey("Tab", false, false, false).empty());
  CHECK(vtForKey("T", false, false, false).empty());
  CHECK(vtForChar("\b", false) == "\x7f" && vtForChar("x", true) == "\x1bx");
  if (!fails) std::printf("shell_logic_tests OK\n");
  return fails ? 1 : 0;
}
