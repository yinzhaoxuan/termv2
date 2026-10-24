#pragma once
// T08 外壳交互模型（平台无关，Linux 单测）：分隔条拖动、标签拖放、滚动条、会话树文案、
// 字号快捷键、按键路由（sendKey / 文本 / IME）、旧 attempt 面板布局。
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "uicfg/layout_ops.h"
#include "uicfg/session_tree.h"
#include "uicfg/workspace.h"

namespace uicfg {

// ---- 分隔条 ----
struct Divider {
  std::vector<int> path;  // 从根出发：0 = first，1 = second
  SplitAxis axis = SplitAxis::Columns;
  Rect splitArea;         // 该 split 节点占据的区域
  Rect bar;               // 可点击区域（以分界线为中心，宽 thickness）
};
std::vector<Divider> computeDividers(const LayoutNode& root, Rect area, double thickness);
// 命中最深（最后加入）的分隔条
std::optional<size_t> hitDivider(const std::vector<Divider>& ds, double x, double y);
LayoutNode* nodeAtPath(LayoutNode& root, const std::vector<int>& path);
// 鼠标位置（与 area 同坐标系）→ 新 ratio，经 clampRatio 保证两侧 ≥ minCols×minRows
double dragRatio(const LayoutNode& split, Rect splitArea, double mouseX, double mouseY,
                 double cellW, double cellH, int minCols = 20, int minRows = 5);

// ---- 标签拖放 ----
// spans：每个标签的 [left, right)；返回放下位置（夹在 0..n-1），n==0 返回 nullopt
std::optional<size_t> tabDropIndex(const std::vector<std::pair<double, double>>& spans, double x);
// 拖动中的插入线：与 tabDropIndex 选中的放下位置一致。往左放画在目标标签左边界，往右放画在
// 目标标签右边界；放回原位（或 from 越界 / 无标签）返回 nullopt，不画线
std::optional<double> tabDropLineX(const std::vector<std::pair<double, double>>& spans, size_t from, double x);
// 拖动阈值：移动超过 threshold 像素才算拖动
bool isDragStart(double dx, double dy, double threshold);

// ---- 滚动条（onViewportChanged 驱动）----
struct ScrollBarState { int min = 0, max = 0, page = 1, pos = 0; bool enabled = false; };
ScrollBarState scrollBarFor(int offsetFromBottom, int historyLines, int visibleRows);
// 用户把滑块拖到 newPos → scrollViewport(lines)（负数 = 进入历史）；0 表示无需滚动
int scrollLinesForThumb(int newPos, int offsetFromBottom, int historyLines);

// ---- 会话树文案 ----
int runningCount(const TreeGroup& g);
std::string groupLabel(const TreeGroup& g);   // "生产 (2 运行中)" / "未分组"
std::string itemLabel(const TreeItem& it);    // "● web-1 · 10.0.0.1 (2)"

// ---- 字号快捷键 ----
constexpr double kMinFontSize = 6.0, kMaxFontSize = 72.0, kDefaultFontSize = 12.0;
// fontBigger / fontSmaller / fontReset → 新字号（夹在 6–72）；非字号动作返回 nullopt
std::optional<double> fontSizeFor(const std::string& action, double current);
// 外部来的字号（ChooseFont 的 10.5、settings.json 手改）→ 钳到 6–72 并四舍五入到 0.5；非有限值 → 默认
double normalizeFontSize(double pt);
// 显示用：整数不带小数（"12"），否则一位小数（"10.5"）
std::string formatFontSize(double pt);
// DirectWrite 本地化族名列表（locale, name）里挑一个：优先 en-us（不分大小写），否则第一个；空列表 → 空串
std::wstring pickFamilyName(const std::vector<std::pair<std::wstring, std::wstring>>& names);

// ---- 按键路由 ----
enum class KeyRoute {
  PassToIme,   // VK_PROCESSKEY 或输入法组字中：不处理，交给 DefWindowProc/IME
  Text,        // 等 WM_CHAR → sendInput（含 AltGr 组合 ToUnicode 得到的字符）
  SendKey,     // 特殊键 → sendKey(vk, mods)，并吞掉随后的 WM_CHAR
};
// key：规范键名（同 Chord::key，另有 "Backspace"、"Numpad0".."Numpad9"、"Char"=其它字符键）；空串表示非特殊的普通字符键
// altGrText：Ctrl+Alt 同时按下且 ToUnicode 产出可打印字符
KeyRoute routeKey(const std::string& key, bool imeProcessKey, bool imeComposing,
                  bool ctrl, bool alt, bool altGrText);
bool isSpecialKey(const std::string& key);
// Alt+Space → 系统菜单（SC_KEYMENU）：msg 为 WM_SYSKEYDOWN(0x0104) / WM_SYSCHAR(0x0106)、
// w 为 VK_SPACE(0x20)、Alt 按下且无 Ctrl 时返回 true（handleKey 应 return false 交给 DefWindowProc）。
// 普通 KEYDOWN、有 Ctrl（含 AltGr）不放行。
bool passAltSpaceToSystemMenu(unsigned msg, unsigned w, bool ctrl, bool alt);
unsigned keyMods(bool ctrl, bool shift, bool alt);  // PaneBackend::KeyMods

// ---- 旧 attempt 面板（只读，位于分隔行之上）----
struct AttemptPanelLayout { int panelH = 0, sepH = 0, termY = 0, termH = 0; };
AttemptPanelLayout attemptPanelLayout(int paneH, bool hasOld, double dpiScale);
std::string attemptHeader(int oldAttemptId);  // "attempt #1（只读）"

}  // namespace uicfg
