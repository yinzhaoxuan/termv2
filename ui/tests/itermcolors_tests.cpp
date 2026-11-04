#include "uicfg/itermcolors.h"
#include "uicfg/settings.h"
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace uicfg;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static std::string plist(const std::string& body) { return "<plist version=\"1.0\"><dict>" + body + "</dict></plist>"; }
static std::string color(const std::string& key, const std::string& r = "0.6", const std::string& g = "0.4",
                         const std::string& b = "0.2", const std::string& space = "sRGB", const std::string& tag = "real") {
  auto part = [&](const char* name, const std::string& value) {
    return "<key>" + std::string(name) + " Component</key><" + tag + ">" + value + "</" + tag + ">";
  };
  return "<key>" + key + "</key><dict>" + part("Red", r) + part("Green", g) + part("Blue", b) +
      "<key>Color Space</key><string>" + space + "</string></dict>";
}

static void sample() {
  std::ifstream in(ITERM_COLORS_SAMPLE, std::ios::binary);
  CHECK(bool(in));
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  auto r = importItermColors(bytes);
  CHECK(r.colors && r.error.empty() && r.missing.empty() && r.ignored.empty() && !r.approximated);
  if (!r.colors) return;
  CHECK(r.colors->foreground == 0xcce6ff && r.colors->background == 0x1a334d && r.colors->cursor == 0xff8000);
  CHECK(r.colors->ansi[1] == 0x800000 && r.colors->ansi[8] == 0x4d4d4d && r.colors->ansi[15] == 0xffffff);
  const char* current = std::setlocale(LC_NUMERIC, nullptr);
  const std::string old = current ? current : "C";
  if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
    const auto localized = importItermColors(bytes);
    CHECK(localized.colors == r.colors && localized.error.empty());
  } else std::puts("SKIP locale de_DE.UTF-8：本机不可用");
  std::setlocale(LC_NUMERIC, old.c_str());
}

static void parsing() {
  const auto defaults = builtinAppearance().colors;
  auto r = importItermColors(plist(color("Foreground Color (Dark)")));
  CHECK(r.colors && r.colors->foreground == 0x996633 && r.colors->ansi == defaults.ansi && r.missing.size() == 18);
  CHECK(std::find(r.missing.begin(), r.missing.end(), "Background Color") != r.missing.end());
  r = importItermColors(plist(color("Foreground Color (Dark)", "0", "0", "0") + color("Foreground Color", "1", "1", "1") +
                            color("Foreground Color (Light)", "0", "0", "0")));
  CHECK(r.colors && r.colors->foreground == 0xffffff);
  CHECK(!importItermColors(plist(color("Foreground Color (Light)"))).colors);
  r = importItermColors(plist(color("Cursor Color", "1", "0", "1", "sRGB", "integer")));
  CHECK(r.colors && r.colors->cursor == 0xff00ff);
  r = importItermColors(plist(color("Foreground Color", "-1", "2", "0.5")));
  CHECK(r.colors && r.colors->foreground == 0x00ff80);
  r = importItermColors(plist(color("Foreground Color", "1", "0", "0", "P3")));
  CHECK(r.colors && r.colors->foreground == 0xff0000 && !r.approximated);
  r = importItermColors(plist(color("Foreground Color", "0.6", "0.4", "0.2", "P3")));
  // 独立预计算：Display P3 (0.6,0.4,0.2) → sRGB (162,99,38)，量化容差 ±1。
  CHECK(r.colors);
  if (r.colors) {
    const auto c = r.colors->foreground;
    CHECK(std::abs(int((c >> 16) & 255) - 162) <= 1);
    CHECK(std::abs(int((c >> 8) & 255) - 99) <= 1);
    CHECK(std::abs(int(c & 255) - 38) <= 1);
  }
  for (const auto* space : {"Calibrated", "future-space", ""}) {
    r = importItermColors(plist(color("Foreground Color", "0.6", "0.4", "0.2", space)));
    CHECK(r.colors && r.approximated && r.colors->foreground == 0x996633);
  }
  r = importItermColors(plist(color("Foreground Color", "0.6", "0.4", "0.2", "s&#82;GB") +
                            "<key>unknown &amp; &lt; &gt; &quot; &apos; &#65; &#x1F600;</key><array><true/><false/><string>x</string></array>"));
  CHECK(r.colors && !r.approximated);
  r = importItermColors(plist(color("Foregro&#117;nd Color")));
  CHECK(r.colors && r.missing.size() == 18);
  std::string ignored = color("Foreground Color");
  for (const auto* key : {"Selection Color", "Selected Text Color", "Bold Color", "Link Color", "Cursor Text Color",
                          "Cursor Guide Color", "Badge Color", "Underline Color", "Tab Color"}) {
    ignored += color(std::string(key) + " (Dark)");
    ignored += color(std::string(key) + " (Light)");
  }
  ignored += color("Selection Color");
  r = importItermColors(plist(ignored));
  CHECK(r.colors && r.ignored.size() == 9 && std::count(r.ignored.begin(), r.ignored.end(), "Selection Color") == 1);
  r = importItermColors("bplist00abc");
  CHECK(!r.colors && r.error == "这是二进制 plist，请先转换：plutil -convert xml1 <文件>");
  r = importItermColors(std::string(kItermColorsMaxBytes + 1, ' '));
  CHECK(!r.colors && r.error.find("1 MiB") != std::string::npos);
  for (const auto& bad : std::vector<std::string>{"", "<plist><dict>", "<plist><dict></plist></dict>",
      plist("<key>not-color</key><string>x</string>"), plist("<key>Foreground Color</key><dict/>"),
      plist(color("Foreground Color", "nan")), plist(color("Foreground Color", "0,5")),
      plist(color("Foreground Color", "1.0x")), plist(color("Foreground Color", "1e9999")),
      plist(color("Foreground Color", "1.5", "0", "0", "sRGB", "integer")),
      plist(color("Foreground Color") + "<key>x</key><data>0</data>"),
      plist(color("Foreground Color") + "<key>x</key><string>&unknown;</string>"),
      plist(color("Foreground Color") + "<key>x</key><string>&#xD800;</string>"),
      plist(color("Foreground Color") + "<key>x</key><string>&#0;</string>"),
      plist(color("Foreground Color") + "<key>x</key><string>&#x110000;</string>"),
      plist(color("Foreground Color") + "<key>x</key><string>&amp</string>"),
      plist(color("Foreground Color") + "<key>dangling</key>"),
      plist(color("Foreground Color")) + "<dict/>",
      plist(color("Foreground Color") + color("Foreground Color"))}) {
    auto failed = importItermColors(bad);
    CHECK(!failed.colors && !failed.error.empty());
  }
  auto boundary = plist(color("Foreground Color"));
  boundary.resize(kItermColorsMaxBytes, ' ');
  CHECK(importItermColors(boundary).colors);
  CHECK(!importItermColors(plist(color("Foreground Color") + "<key>x</key><string>]]></string>")).colors);
  const auto nulText = "<key>x</key><string>" + std::string(1, '\0') + "</string>";
  CHECK(!importItermColors(plist(color("Foreground Color") + nulText)).colors);
  std::string deep = "<string>x</string>";
  for (int i = 0; i < 5; ++i) deep = "<array>" + deep + "</array>";
  CHECK(importItermColors(plist(color("Foreground Color") + "<key>depth8</key>" + deep)).colors);
  deep = "<array>" + deep + "</array>";
  r = importItermColors(plist(color("Foreground Color") + "<key>deep</key>" + deep));
  CHECK(!r.colors && r.error.find("8") != std::string::npos);
  const auto noSpace = plist("<key>Background Color</key><dict><key>Red Component</key><real>0.5</real>"
                            "<key>Green Component</key><real>0.5</real><key>Blue Component</key><real>0.5</real></dict>");
  r = importItermColors(noSpace);
  CHECK(r.colors && r.colors->background == 0x808080 && !r.approximated);
}

static void prompt() {
  ItermColorsImport r; r.colors = builtinAppearance().colors;
  auto text = itermImportPrompt(r, "手写.itermcolors");
  CHECK(text.find("导入「手写.itermcolors」的配色到默认外观？") == 0);
  CHECK(text.find("19 项") != text.npos && text.find("缺少") == text.npos && text.find("不支持") == text.npos && text.find("近似") == text.npos);
  r.missing = {"Background Color", "Cursor Color"};
  r.ignored = {"Selection Color", "Bold Color"}; r.approximated = true;
  text = itermImportPrompt(r, "test");
  CHECK(text.find("17 项") != text.npos && text.find("缺少（保留默认）：Background Color、Cursor Color") != text.npos);
  CHECK(text.find("不支持（已忽略）：选区色、粗体色") != text.npos && text.find("部分颜色的色彩空间按 sRGB 近似。") != text.npos);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "sample") sample();
  else if (mode == "parsing") parsing();
  else if (mode == "prompt") prompt();
  else return 2;
  return fails != 0;
}
