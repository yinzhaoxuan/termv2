#pragma once
// 第 12.1 / 12.3：标签栏状态模型。每个标签拥有一棵布局树。
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "uicfg/workspace.h"

namespace uicfg {

class TabBar {
 public:
  using IdGen = std::function<std::string()>;  // 生成唯一 paneId
  explicit TabBar(IdGen gen) : gen_(std::move(gen)) {}

  // 新建标签（单 pane），插在当前标签之后并激活；返回新标签下标
  size_t newTab(const std::string& connectionId, const std::string& title);
  // 双击左侧连接：总是新标签 + 新 Session，不复用已有标签
  size_t openConnection(const Connection& c) { return newTab(c.id, c.name); }
  // 关闭标签；激活其右侧（或新的最后一个）标签。返回 false 表示下标无效
  bool closeTab(size_t index);
  void next();
  void prev();
  bool activate(size_t index);
  bool move(size_t from, size_t to);   // 拖动重排，活动标签跟随
  // 键盘重排（moveTabLeft = -1 / moveTabRight = +1）：活动标签移动 delta 位，仍为活动标签。
  // 越界（含已在最左/最右）不回绕，返回 false 且不改变顺序。
  bool moveActive(int delta);

  size_t count() const { return tabs_.size(); }
  bool empty() const { return tabs_.empty(); }
  std::optional<size_t> activeIndex() const { return tabs_.empty() ? std::nullopt : std::optional<size_t>(active_); }
  Tab* activeTab() { return tabs_.empty() ? nullptr : &tabs_[active_]; }
  std::vector<Tab>& tabs() { return tabs_; }
  const std::vector<Tab>& tabs() const { return tabs_; }

  // 标签标题取自活动窗格标题；查不到时退回 Tab::title
  std::string titleFor(size_t index, const std::function<std::optional<std::string>(const std::string&)>& paneTitle) const;
  // 从 workspace 恢复
  void load(std::vector<Tab> tabs, size_t active = 0);

 private:
  IdGen gen_;
  std::vector<Tab> tabs_;
  size_t active_ = 0;
};

}  // namespace uicfg
