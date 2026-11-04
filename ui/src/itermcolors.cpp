// SPDX-License-Identifier: GPL-3.0-or-later
#include "uicfg/itermcolors.h"
#include "uicfg/settings.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace uicfg {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::string_view trim(std::string_view s) {
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

void appendCodepoint(std::string& out, std::uint32_t cp) {
  if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
      (cp < 0x20 && cp != 9 && cp != 10 && cp != 13) || cp == 0xfffe || cp == 0xffff)
    throw std::runtime_error("XML 字符实体无效");
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
  else if (cp < 0x10000) {
    out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
  } else {
    out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
    out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
  }
}

std::string decode(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      if ((static_cast<unsigned char>(text[i]) < 0x20 && !space(text[i])) || text.substr(i).starts_with("]]>"))
        throw std::runtime_error("XML 文本含无效字符");
      out += text[i];
      continue;
    }
    auto end = text.find(';', i + 1);
    if (end == text.npos) throw std::runtime_error("XML 实体未结束");
    auto entity = text.substr(i + 1, end - i - 1);
    if (entity == "amp") out += '&';
    else if (entity == "lt") out += '<';
    else if (entity == "gt") out += '>';
    else if (entity == "quot") out += '"';
    else if (entity == "apos") out += '\'';
    else if (entity.starts_with('#')) {
      entity.remove_prefix(1);
      int base = 10;
      if (entity.starts_with('x')) { entity.remove_prefix(1); base = 16; }
      std::uint32_t cp = 0;
      const auto [p, ec] = std::from_chars(entity.data(), entity.data() + entity.size(), cp, base);
      if (entity.empty() || ec != std::errc{} || p != entity.data() + entity.size())
        throw std::runtime_error("XML 字符实体无效");
      appendCodepoint(out, cp);
    } else throw std::runtime_error("不支持的 XML 实体");
    i = end;
  }
  return out;
}

struct Node {
  std::string tag, text;
  std::vector<Node> children;
};

// 只读 plist 子集；不加载 DTD，不展开外部实体。
class PlistReader {
public:
  explicit PlistReader(std::string_view bytes) : bytes_(bytes) {
    if (bytes_.starts_with("\xef\xbb\xbf")) pos_ = 3;
  }
  Node read() {
    skip();
    auto root = node(1);
    skip();
    if (pos_ != bytes_.size() || root.tag != "plist" || root.children.size() != 1 || root.children[0].tag != "dict")
      fail("需要 <plist><dict> 顶层字典");
    return root;
  }
private:
  [[noreturn]] static void fail(const char* text) { throw std::runtime_error(text); }
  bool at(std::string_view s) const { return bytes_.substr(pos_).starts_with(s); }
  void through(std::string_view end) {
    auto p = bytes_.find(end, pos_);
    if (p == bytes_.npos) fail("XML 声明或注释未结束");
    pos_ = p + end.size();
  }
  void doctype() {
    pos_ += 9;
    int brackets = 0;
    char quote = 0;
    while (pos_ < bytes_.size()) {
      char c = bytes_[pos_++];
      if (quote) { if (c == quote) quote = 0; }
      else if (c == '\'' || c == '"') quote = c;
      else if (c == '[') ++brackets;
      else if (c == ']') { if (--brackets < 0) fail("DOCTYPE 无效"); }
      else if (c == '>' && brackets == 0) return;
    }
    fail("DOCTYPE 未结束");
  }
  void skip() {
    for (;;) {
      while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
      if (at("<!--")) { pos_ += 4; through("-->"); }
      else if (at("<?xml") && pos_ + 5 < bytes_.size() && space(bytes_[pos_ + 5])) { pos_ += 5; through("?>"); }
      else if (at("<!DOCTYPE") && pos_ + 9 < bytes_.size() && space(bytes_[pos_ + 9])) doctype();
      else return;
    }
  }
  std::string name() {
    size_t start = pos_;
    while (pos_ < bytes_.size()) {
      char c = bytes_[pos_];
      if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')) break;
      ++pos_;
    }
    if (start == pos_) fail("XML 标签无效");
    return std::string(bytes_.substr(start, pos_ - start));
  }
  Node node(unsigned depth) {
    if (depth > 8) fail("plist 嵌套超过 8 层");
    if (!at("<")) fail("缺少 XML 标签");
    ++pos_;
    Node n; n.tag = name();
    const bool container = n.tag == "plist" || n.tag == "dict" || n.tag == "array";
    if (!container && n.tag != "key" && n.tag != "real" && n.tag != "integer" && n.tag != "string" &&
        n.tag != "true" && n.tag != "false") fail("不支持的 plist 标签");
    while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
    if (n.tag == "plist" && at("version")) {
      pos_ += 7;
      while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
      if (!at("=")) fail("plist version 无效");
      ++pos_;
      while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
      if (!at("\"") && !at("'")) fail("plist version 无效");
      const char quote = bytes_[pos_++];
      auto end = bytes_.find(quote, pos_);
      if (end == bytes_.npos || bytes_.substr(pos_, end - pos_) != "1.0") fail("plist version 无效");
      pos_ = end + 1;
      while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
    }
    if (at("/>")) { pos_ += 2; return n; }
    if (!at(">")) fail("XML 起始标签无效");
    ++pos_;
    if (container) {
      skip();
      while (!at("</")) { n.children.push_back(node(depth + 1)); skip(); }
    } else {
      // 标量中的注释可以跳过，普通文本保留空白。
      while (!at("</")) {
        if (at("<!--")) { pos_ += 4; through("-->"); continue; }
        auto end = bytes_.find('<', pos_);
        if (end == bytes_.npos) fail("XML 文本未结束");
        if (end == pos_) fail("标量中不能嵌套标签");
        n.text += decode(bytes_.substr(pos_, end - pos_));
        pos_ = end;
      }
    }
    pos_ += 2;
    if (name() != n.tag) fail("XML 结束标签不匹配");
    while (pos_ < bytes_.size() && space(bytes_[pos_])) ++pos_;
    if (!at(">")) fail("XML 结束标签无效");
    ++pos_;
    if ((n.tag == "true" || n.tag == "false") && !trim(n.text).empty()) fail("plist 布尔值无效");
    if (n.tag == "dict") {
      if (n.children.size() % 2) fail("plist 字典缺少值");
      for (size_t i = 0; i < n.children.size(); i += 2)
        if (n.children[i].tag != "key" || n.children[i + 1].tag == "key") fail("plist 字典键值无效");
    }
    return n;
  }
  std::string_view bytes_;
  size_t pos_ = 0;
};

using Dictionary = std::map<std::string, const Node*>;
Dictionary dictionary(const Node& n) {
  if (n.tag != "dict") throw std::runtime_error("颜色值必须是字典");
  Dictionary d;
  for (size_t i = 0; i < n.children.size(); i += 2)
    if (!d.emplace(n.children[i].text, &n.children[i + 1]).second) throw std::runtime_error("plist 字典键重复");
  return d;
}

const Node* colorNode(const Dictionary& d, const std::string& key) {
  if (auto it = d.find(key); it != d.end()) return it->second;
  if (auto it = d.find(key + " (Dark)"); it != d.end()) return it->second;
  return nullptr;
}

double component(const Dictionary& d, const char* key) {
  auto it = d.find(key);
  if (it == d.end() || (it->second->tag != "real" && it->second->tag != "integer"))
    throw std::runtime_error(std::string("缺少或无效分量：") + key);
  auto text = trim(it->second->text);
  if (it->second->tag == "integer" && text.find_first_not_of("+-0123456789") != text.npos)
    throw std::runtime_error("integer 分量无效");
  // classic locale 固定小数点，不受用户的 LC_NUMERIC 影响。
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  double v = 0;
  in >> std::noskipws >> v;
  if (!in || !in.eof() || !std::isfinite(v)) throw std::runtime_error("颜色分量不是有限数字");
  return std::clamp(v, 0.0, 1.0);
}

double linear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double encoded(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }

std::uint32_t readColor(const Node& node, bool& approximated) {
  const auto d = dictionary(node);
  std::array<double, 3> rgb{component(d, "Red Component"), component(d, "Green Component"), component(d, "Blue Component")};
  std::string colorSpace = "sRGB";
  if (auto it = d.find("Color Space"); it != d.end()) {
    if (it->second->tag != "string") throw std::runtime_error("Color Space 必须是 string");
    colorSpace = it->second->text;
  }
  if (colorSpace == "P3") {
    const double r = linear(rgb[0]), g = linear(rgb[1]), b = linear(rgb[2]);
    // Display P3（D65）→ 线性 sRGB；两者使用同一传递函数。
    rgb = {1.224940176 * r - 0.224940176 * g,
           -0.042056955 * r + 1.042056955 * g,
           -0.019637555 * r - 0.078636046 * g + 1.098273601 * b};
    for (auto& c : rgb) c = encoded(std::clamp(c, 0.0, 1.0));
  } else if (colorSpace != "sRGB") approximated = true;
  std::uint32_t out = 0;
  for (double c : rgb) out = (out << 8) | std::uint32_t(std::lround(c * 255));
  return out;
}

constexpr std::array<std::pair<const char*, const char*>, 9> kIgnored{{
    {"Selection Color", "选区色"}, {"Selected Text Color", "选区文字色"}, {"Bold Color", "粗体色"},
    {"Link Color", "链接色"}, {"Cursor Text Color", "光标文字色"}, {"Cursor Guide Color", "光标引导色"},
    {"Badge Color", "徽章色"}, {"Underline Color", "下划线色"}, {"Tab Color", "标签色"}}};

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& p : parts) { if (!out.empty()) out += "、"; out += p; }
  return out;
}
}  // namespace

ItermColorsImport importItermColors(std::string_view fileBytes) {
  ItermColorsImport out;
  if (fileBytes.starts_with("bplist00")) {
    out.error = "这是二进制 plist，请先转换：plutil -convert xml1 <文件>";
    return out;
  }
  if (fileBytes.size() > kItermColorsMaxBytes) { out.error = "配色文件超过 1 MiB"; return out; }
  try {
    const auto root = PlistReader(fileBytes).read();
    const auto d = dictionary(root.children[0]);
    auto colors = builtinAppearance().colors;
    unsigned count = 0;
    auto read = [&](const std::string& key, std::uint32_t& dest) {
      if (const auto* n = colorNode(d, key)) { dest = readColor(*n, out.approximated); ++count; }
      else out.missing.push_back(key);
    };
    read("Foreground Color", colors.foreground);
    read("Background Color", colors.background);
    read("Cursor Color", colors.cursor);
    for (size_t i = 0; i < colors.ansi.size(); ++i) read("Ansi " + std::to_string(i) + " Color", colors.ansi[i]);
    for (const auto& [key, label] : kIgnored) { (void)label; if (colorNode(d, key)) out.ignored.emplace_back(key); }
    if (!count) out.error = "不是 iTerm2 配色文件";
    else out.colors = colors;
  } catch (const std::exception& e) { out.error = e.what(); }
  return out;
}

std::string itermImportPrompt(const ItermColorsImport& result, const std::string& fileName) {
  std::string text = "导入「" + fileName + "」的配色到默认外观？\n已读取：前景、背景、光标、16 色中的 " +
      std::to_string(19 - std::min<size_t>(19, result.missing.size())) + " 项";
  if (!result.missing.empty()) text += "\n缺少（保留默认）：" + join(result.missing);
  if (!result.ignored.empty()) {
    std::vector<std::string> labels;
    for (const auto& key : result.ignored) {
      auto it = std::find_if(kIgnored.begin(), kIgnored.end(), [&](const auto& p) { return key == p.first; });
      labels.push_back(it == kIgnored.end() ? key : it->second);
    }
    text += "\n不支持（已忽略）：" + join(labels);
  }
  if (result.approximated) text += "\n部分颜色的色彩空间按 sRGB 近似。";
  return text;
}
}  // namespace uicfg
