#include "uicfg/shell_models.h"
#include <algorithm>
#include <cmath>

namespace uicfg {

namespace {
void walkDiv(const LayoutNode& n, Rect a, double t, std::vector<int>& path, std::vector<Divider>& out) {
  if (n.kind != LayoutNode::Kind::Split) return;
  Divider d; d.path = path; d.axis = n.axis; d.splitArea = a;
  Rect fa = a, sa = a;
  if (n.axis == SplitAxis::Columns) {
    double w1 = a.w * n.ratio; fa.w = w1; sa.x = a.x + w1; sa.w = a.w - w1;
    d.bar = {a.x + w1 - t / 2, a.y, t, a.h};
  } else {
    double h1 = a.h * n.ratio; fa.h = h1; sa.y = a.y + h1; sa.h = a.h - h1;
    d.bar = {a.x, a.y + h1 - t / 2, a.w, t};
  }
  out.push_back(d);
  path.push_back(0); walkDiv(*n.first, fa, t, path, out); path.back() = 1;
  walkDiv(*n.second, sa, t, path, out); path.pop_back();
}
}  // namespace

std::vector<Divider> computeDividers(const LayoutNode& root, Rect area, double thickness) {
  std::vector<Divider> out; std::vector<int> path;
  walkDiv(root, area, thickness, path, out);
  return out;
}

std::optional<size_t> hitDivider(const std::vector<Divider>& ds, double x, double y) {
  for (size_t i = ds.size(); i-- > 0;) {
    const Rect& b = ds[i].bar;
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
  }
  return std::nullopt;
}

LayoutNode* nodeAtPath(LayoutNode& root, const std::vector<int>& path) {
  LayoutNode* n = &root;
  for (int s : path) {
    if (n->kind != LayoutNode::Kind::Split) return nullptr;
    n = s == 0 ? n->first.get() : n->second.get();
  }
  return n;
}

double dragRatio(const LayoutNode& split, Rect a, double mx, double my, double cellW, double cellH,
                 int minCols, int minRows) {
  if (split.kind != LayoutNode::Kind::Split) return split.ratio;
  bool cols = split.axis == SplitAxis::Columns;
  double total = cols ? a.w : a.h;
  if (total <= 0) return split.ratio;
  double desired = cols ? (mx - a.x) / total : (my - a.y) / total;
  return clampRatio(split, a, desired, cellW, cellH, minCols, minRows);
}

std::optional<size_t> tabDropIndex(const std::vector<std::pair<double, double>>& spans, double x) {
  if (spans.empty()) return std::nullopt;
  for (size_t i = 0; i < spans.size(); ++i) if (x < spans[i].second) return i;
  return spans.size() - 1;
}

bool isDragStart(double dx, double dy, double threshold) {
  return std::abs(dx) > threshold || std::abs(dy) > threshold;
}

ScrollBarState scrollBarFor(int off, int hist, int rows) {
  ScrollBarState s;
  hist = std::max(0, hist); rows = std::max(1, rows); off = std::clamp(off, 0, hist);
  s.min = 0; s.page = rows; s.max = hist + rows - 1; s.pos = hist - off; s.enabled = hist > 0;
  return s;
}

int scrollLinesForThumb(int newPos, int off, int hist) {
  hist = std::max(0, hist);
  int newOff = hist - std::clamp(newPos, 0, hist);
  return std::clamp(off, 0, hist) - newOff;  // 向上（newOff 变大）→ 负数
}

int runningCount(const TreeGroup& g) {
  int n = 0; for (auto& it : g.items) n += it.runningSessions; return n;
}
std::string groupLabel(const TreeGroup& g) {
  std::string s = g.id.empty() ? "未分组" : g.name;
  if (int n = runningCount(g)) s += " (" + std::to_string(n) + " 运行中)";
  return s;
}
std::string itemLabel(const TreeItem& it) {
  std::string s = (it.hasRunning() ? "● " : "") + it.name;
  if (!it.host.empty()) s += " · " + it.host;
  if (it.runningSessions > 1) s += " (" + std::to_string(it.runningSessions) + ")";
  return s;
}

std::optional<double> fontSizeFor(const std::string& action, double cur) {
  double v;
  if (action == "fontBigger") v = std::floor(cur) + 1;
  else if (action == "fontSmaller") v = std::ceil(cur) - 1;
  else if (action == "fontReset") v = kDefaultFontSize;
  else return std::nullopt;
  return std::clamp(v, kMinFontSize, kMaxFontSize);
}

bool isSpecialKey(const std::string& k) {
  static const char* names[] = {"Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown",
                                "Insert", "Delete", "Enter", "Tab", "Backspace", "Escape"};
  for (auto* n : names) if (k == n) return true;
  if (k.rfind("Numpad", 0) == 0) return true;
  if (k.size() >= 2 && k[0] == 'F' && std::all_of(k.begin() + 1, k.end(), ::isdigit)) {
    int n = std::stoi(k.substr(1)); return n >= 1 && n <= 24;
  }
  return false;
}

unsigned keyMods(bool ctrl, bool shift, bool alt) {
  return (shift ? 1u : 0u) | (alt ? 2u : 0u) | (ctrl ? 4u : 0u);
}

KeyRoute routeKey(const std::string& key, bool processKey, bool composing, bool ctrl, bool alt, bool altGrText) {
  if (processKey || composing) return KeyRoute::PassToIme;
  if (ctrl && alt && altGrText) return KeyRoute::Text;     // AltGr 字符按文本发
  if (isSpecialKey(key)) return KeyRoute::SendKey;
  if (alt && !ctrl && !key.empty()) return KeyRoute::SendKey;  // Alt+字母/数字：后端加 ESC 前缀
  return KeyRoute::Text;
}

AttemptPanelLayout attemptPanelLayout(int h, bool hasOld, double sc) {
  AttemptPanelLayout l;
  h = std::max(0, h);
  if (hasOld) {
    l.sepH = std::min(h, (int)std::lround(20 * sc));
    l.panelH = std::min({(int)std::lround(200 * sc), h * 35 / 100, h - l.sepH});
    l.panelH = std::max(0, l.panelH);
  }
  l.termY = l.panelH + l.sepH;
  l.termH = std::max(0, h - l.termY);
  return l;
}

std::string attemptHeader(int id) { return "上一次 attempt #" + std::to_string(id) + "（只读）"; }

}  // namespace uicfg
