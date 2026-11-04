#undef NDEBUG
#include "uicfg/shell_logic.h"
#include <cassert>
#include <string>

using uicfg::clipboardReadPrompt;

static void empty() {
  const auto prompt = clipboardReadPrompt("测试服务器", "");
  assert(prompt.mainText == "「测试服务器」中的程序请求读取本机剪贴板");
  assert(prompt.content == "剪贴板为空");
}

static void shortText() {
  const auto prompt = clipboardReadPrompt("服务器", "你好😀");
  assert(prompt.content == "剪贴板当前有 3 个字符。允许后，该程序会得到全部内容。\n你好😀");
  assert(clipboardReadPrompt("", "hello").content ==
         "剪贴板当前有 5 个字符。允许后，该程序会得到全部内容。\nhello");
}

static void truncation() {
  std::string line, shown;
  for (int i = 0; i < 62; ++i) line += "😀";
  for (int i = 0; i < 60; ++i) shown += "😀";
  auto prompt = clipboardReadPrompt("服务器", line);
  assert(prompt.content == "剪贴板当前有 62 个字符。允许后，该程序会得到全部内容。\n" + shown);
  // CRLF / LF / CR 统一分行；只显示前三行，末尾换行不额外算空行。
  prompt = clipboardReadPrompt("服务器", line + "\r\n第二行\n第三行\r第四行\r\n");
  assert(prompt.content.ends_with("\n" + shown + "\n第二行\n第三行\n…（其余 1 行未显示）"));
  assert(prompt.content.find("第四行") == std::string::npos);
}

static void controls() {
  auto prompt = clipboardReadPrompt("服务器", "a\t\x01\x1b\x7f");
  assert(prompt.content == "剪贴板当前有 5 个字符。允许后，该程序会得到全部内容。\na^I^A^[^?");
  assert(clipboardReadPrompt("", std::string("a\0b", 3)).content ==
         "剪贴板当前有 3 个字符。允许后，该程序会得到全部内容。\na^@b");
  // ^X 占两个可见码点，不能在第 60 列只显示一半。
  assert(clipboardReadPrompt("", std::string(59, 'x') + "\tZ").content.ends_with("\n" + std::string(59, 'x')));
  assert(clipboardReadPrompt("", std::string(58, 'x') + "\tZ").content.ends_with("\n" + std::string(58, 'x') + "^I"));
}

static void local() {
  assert(clipboardReadPrompt("", "").mainText == "「本地终端」中的程序请求读取本机剪贴板");
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  if (mode == "empty") empty();
  else if (mode == "short") shortText();
  else if (mode == "truncation") truncation();
  else if (mode == "controls") controls();
  else if (mode == "local") local();
  else return 1;
}
