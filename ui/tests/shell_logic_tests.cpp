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
  {  // 粘贴预览：末尾换行触发确认，但不额外算空行
    CHECK(!pastePreview("", true).needsConfirm && pastePreview("", true).message.empty());
    CHECK(!pastePreview("单行", true).needsConfirm && pastePreview("单行", true).message.empty());
    auto trailing = pastePreview("a\r", true);
    CHECK(trailing.needsConfirm && trailing.message == "将粘贴 1 行、2 字节：\na");
    CHECK(pastePreview("a\rb", true).message == "将粘贴 2 行、3 字节：\na\nb");
    CHECK(pastePreview("a\r\r", true).message == "将粘贴 2 行、3 字节：\na\n");
    CHECK(pastePreview("\r", true).needsConfirm && pastePreview("\r", true).message == "将粘贴 1 行、1 字节：\n");
    CHECK(pastePreview("中\r文", true).message == "将粘贴 2 行、7 字节：\n中\n文");
    CHECK(!pastePreview("a\rb\r", false).needsConfirm && pastePreview("a\rb\r", false).message.empty());

    std::string lines, shown;
    for (int i = 1; i <= 12; ++i) {
      if (i > 1) lines += '\r';
      lines += std::to_string(i);
      if (i <= 10) shown += "\n" + std::to_string(i);
    }
    CHECK(pastePreview(lines, true).message == "将粘贴 12 行、26 字节：" + shown + "\n…（其余 2 行未显示）");
    CHECK(pastePreview(lines.substr(0, 20), true).message == "将粘贴 10 行、20 字节：" + shown);

    std::string cjk;
    for (int i = 0; i < 120; ++i) cjk += "中";
    CHECK(pastePreview(cjk + "文\r", true).message == "将粘贴 1 行、364 字节：\n" + cjk);
    CHECK(pastePreview(cjk + "\r", true).message == "将粘贴 1 行、361 字节：\n" + cjk);
    CHECK(pastePreview(std::string(121, 'x') + "\r", true).message == "将粘贴 1 行、122 字节：\n" + std::string(120, 'x'));

    std::string controls, escaped;
    for (char ch = 0; ch < 0x20; ++ch) {
      if (ch == '\r' || ch == '\n') continue;
      controls += ch; escaped += '^'; escaped += static_cast<char>(ch + '@');
    }
    controls += '\x7f'; controls += '\r';
    CHECK(pastePreview(controls, true).message == "将粘贴 1 行、32 字节：\n" + escaped + "^?");
    std::string tabs, carets;
    for (int i = 0; i < 60; ++i) { tabs += '\t'; carets += "^I"; }
    CHECK(pastePreview(tabs + "x\r", true).message == "将粘贴 1 行、62 字节：\n" + carets);
  }
  CHECK(!parseCommandArg({L"it2w_shell.exe"}));
  CHECK(*parseCommandArg({L"x", L"--command", L"t.cmd a b"}) == L"t.cmd a b");
  CHECK(*parseCommandArg({L"x", L"--command=dir /s"}) == L"dir /s");
  CHECK(parseCommandArg({L"x", L"--command"})->empty());
  // 链接
  auto l = linkPrompt("https://example.com/a");
  CHECK(l.canOpen && l.url == "https://example.com/a" && l.message.find("https://example.com/a") != std::string::npos);
  CHECK(!linkPrompt("file:///C:/x").canOpen && !linkPrompt("javascript:alert(1)").canOpen && !linkPrompt("http://a b").canOpen);
  // 状态覆盖层
  uishell::SessionStatus st{uishell::SessionState::Failed, 0, false, "", "认证失败"};
  auto o = paneOverlay(false, false, "", st);
  CHECK(o.visible && o.showReconnect && o.text == "会话失败：认证失败" && o.buttonLabel == "重连");
  st.errorText.clear();
  CHECK(paneOverlay(false, false, "", st).text == "会话失败：未知错误");
  st.state = uishell::SessionState::Exited; st.exitCode = 17;
  o = paneOverlay(false, false, "", st);
  CHECK(o.visible && o.showReconnect && o.text == "会话已退出（退出码 17）" && o.buttonLabel == "重连");
  auto placeholder = paneOverlay(false, true, "本地终端", st);
  CHECK(placeholder.visible && placeholder.showReconnect && placeholder.buttonLabel == "连接");
  CHECK(placeholder.text == "未连接：本地终端。点击「连接」或按 Enter 开始");
  CHECK(paneOverlay(false, true, "开发 SSH", st).text == "未连接：开发 SSH。点击「连接」或按 Enter 开始");
  {
    using S = uishell::SessionState;
    using R = uishell::SessionRecovery;
    uishell::SessionStatus recovery{S::Failed, 0, false, "", "VmNotRunning 启动并连接"};
    CHECK(paneOverlay(false, false, "", recovery).buttonLabel == "重连");  // 文案不决定恢复动作
    recovery.recovery.kind = R::StartVmAndConnect;
    recovery.errorText = "任意错误";
    CHECK(paneOverlay(false, false, "", recovery).buttonLabel == "启动并连接");
    recovery.recovery = {R::ChooseAddress, {{"nic", "网卡 10.0.0.1"}}};
    CHECK(paneOverlay(false, false, "", recovery).buttonLabel == "选择地址…");
    struct RecoveryCase { R::Kind kind; std::string label; };
    const RecoveryCase cases[] = {{R::None, "重连"}, {R::StartVmAndConnect, "启动并连接"}, {R::ChooseAddress, "选择地址…"}};
    for (auto state : {S::Starting, S::Running, S::Reconnecting, S::Exited, S::Failed}) {
      recovery.state = state;
      for (const auto& c : cases) {
        recovery.recovery.kind = c.kind;
        auto normal = paneOverlay(false, false, "", recovery);
        auto label = state == S::Failed ? c.label : "重连";
        CHECK(normal.buttonLabel == label);
        if (state == S::Failed || state == S::Exited) CHECK(normal.visible && normal.showReconnect);
        else CHECK(!normal.visible && !normal.showReconnect && normal.text.empty());
        auto pending = paneOverlay(false, true, "开发 SSH", recovery);
        CHECK(pending.visible && pending.showReconnect && pending.buttonLabel == "连接");
        CHECK(pending.text == "未连接：开发 SSH。点击「连接」或按 Enter 开始");
        for (bool notStarted : {false, true}) {  // 缺失引用优先于占位与会话状态
          auto missing = paneOverlay(true, notStarted, "开发 SSH", recovery);
          CHECK(missing.visible && !missing.showReconnect && missing.buttonLabel == label);
          CHECK(missing.text == "连接配置已删除（引用缺失）。请在左侧选择连接后新建会话。");
        }
      }
    }
  }
  CHECK(stateLabel(uishell::SessionState::Reconnecting) == "正在重连");
  {  // PERF-01：标题变化不刷树；仅进出运行集合才 refreshTree
    using S = uishell::SessionState;
    CHECK(!statusAffectsSessionTree(S::Running, S::Running));       // 同状态（OSC 标题等）
    CHECK(!statusAffectsSessionTree(S::Starting, S::Running));      // 仍在 snaps 内
    CHECK(!statusAffectsSessionTree(S::Running, S::Reconnecting));
    CHECK(statusAffectsSessionTree(S::Running, S::Exited));         // onSessionEnded
    CHECK(statusAffectsSessionTree(S::Reconnecting, S::Failed));
    CHECK(statusAffectsSessionTree(S::Exited, S::Starting));        // 重进（若有）
    CHECK(!statusAffectsSessionTree(S::Exited, S::Failed));         // 都已不在集合
  }
  CHECK(pasteRejectedMessage(40 * 1024, 32 * 1024).find("32") != std::string::npos);
  CHECK(vtForKey("Up", false, false, false) == "\x1b[A");
  CHECK(vtForKey("Left", true, false, false) == "\x1b[1;5D");
  CHECK(vtForKey("PageUp", false, false, false) == "\x1b[5~");
  CHECK(vtForKey("Delete", false, true, false) == "\x1b[3;2~");
  CHECK(vtForKey("F1", false, false, false) == "\x1bOP" && vtForKey("F5", false, false, false) == "\x1b[15~");
  CHECK(vtForKey("Tab", false, true, false) == "\x1b[Z" && vtForKey("Tab", false, false, false).empty());
  CHECK(vtForKey("T", false, false, false).empty());
  CHECK(vtForChar("\b", false) == "\x7f" && vtForChar("x", true) == "\x1bx");
  {  // 搜索条：状态文字
    using uishell::SearchResult;
    SearchResult r;
    CHECK(searchStatusText("", r) == "");
    CHECK(searchStatusText("foo", r) == "无结果");  // 默认实现（未接后端）
    r.found = true; r.index = 0; r.total = -1;
    CHECK(searchStatusText("foo", r) == "1/?");
    r.index = 2; r.total = 7;
    CHECK(searchStatusText("foo", r) == "3/7");
    SearchResult p; p.pending = true;
    CHECK(searchStatusText("foo", p) == "搜索中…");
    r.hasNewOutput = true;
    CHECK(searchStatusText("foo", r) == "有新输出，按 Enter 重新搜索");
    SearchResult rs; rs.restarted = true;
    CHECK(searchStatusText("foo", rs) == "内容已变化，按 Enter 重新搜索");
  }
  {  // 搜索条：状态机
    using K = SearchBarAction;
    SearchBarModel m;
    CHECK(m.onKey(SearchBarKey::Enter).kind == K::None);            // 空 needle：Enter 无动作
    auto a = m.onTextChanged("foo");
    CHECK(a.kind == K::Find && a.backward && a.restart);            // 输入变化：从头向上
    uishell::SearchResult r; r.found = true; r.index = 4; r.total = -1;
    CHECK(m.onResult(r).kind == K::None && m.statusText() == "5/?");
    a = m.onKey(SearchBarKey::Enter);
    CHECK(a.kind == K::Find && a.backward && !a.restart);           // Enter = 下一个（向上/更早）
    a = m.onKey(SearchBarKey::ShiftEnter);
    CHECK(a.kind == K::Find && !a.backward && !a.restart);          // Shift+Enter = 上一个
    r.hasNewOutput = true; m.onResult(r);
    CHECK(m.statusText() == "有新输出，按 Enter 重新搜索");
    a = m.onKey(SearchBarKey::Enter);
    CHECK(a.kind == K::Find && a.backward && a.restart);            // 有新输出：Enter 从头重搜
    a = m.onKey(SearchBarKey::ShiftEnter);
    CHECK(a.kind == K::Find && a.restart);
    uishell::SearchResult rs; rs.restarted = true;
    a = m.onResult(rs);
    CHECK(a.kind == K::Find && a.restart);                          // RESTARTED：自动重搜一次
    CHECK(m.onResult(rs).kind == K::None);                          // 重搜仍 restarted：不再自动（防循环）
    CHECK(m.onKey(SearchBarKey::Enter).restart);                    // 交给用户 Enter
    uishell::SearchResult ok; ok.found = true; ok.total = 1;
    m.onResult(ok); CHECK(m.statusText() == "1/1");
    CHECK(m.onResult(rs).kind == K::Find);                          // 正常结果之后再失效：又可自动一次
    CHECK(m.onTextChanged("").kind == K::Clear && m.statusText().empty());
    CHECK(m.onKey(SearchBarKey::Escape).kind == K::Close);
    m.onTextChanged("x"); m.reset(); CHECK(m.needle().empty() && !m.last().found);
  }
  {  // searchHistory 默认 Ctrl+Shift+F（全部默认绑定规范化后互不冲突由 settings_tests 覆盖）
    auto d = defaultSettings();
    CHECK(d.keybindings.at("searchHistory") == "Ctrl+Shift+F");
    CHECK(matchKey(d.keybindings, {true, true, false, "F"}).action == "searchHistory");
    CHECK(matchKey(d.keybindings, {true, false, false, "F"}).action != "searchHistory");
  }
  if (!fails) std::printf("shell_logic_tests OK\n");
  return fails ? 1 : 0;
}
