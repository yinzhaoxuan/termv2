#include "uicfg/tab_model.h"
namespace uicfg {
size_t TabBar::newTab(const std::string& conn, const std::string& title) {
  Tab t; t.title = title; std::string id = gen_();
  t.root = LayoutNode::pane(id, conn); t.activePaneId = id;
  size_t pos = tabs_.empty() ? 0 : active_ + 1;
  tabs_.insert(tabs_.begin() + pos, std::move(t));
  active_ = pos; return pos;
}
bool TabBar::closeTab(size_t i) {
  if (i >= tabs_.size()) return false;
  tabs_.erase(tabs_.begin() + i);
  if (tabs_.empty()) { active_ = 0; return true; }
  if (i < active_) --active_;
  else if (active_ >= tabs_.size()) active_ = tabs_.size() - 1;
  return true;
}
void TabBar::next() { if (!tabs_.empty()) active_ = (active_ + 1) % tabs_.size(); }
void TabBar::prev() { if (!tabs_.empty()) active_ = (active_ + tabs_.size() - 1) % tabs_.size(); }
bool TabBar::activate(size_t i) { if (i >= tabs_.size()) return false; active_ = i; return true; }
bool TabBar::move(size_t from, size_t to) {
  if (from >= tabs_.size() || to >= tabs_.size()) return false;
  if (from == to) return true;
  Tab t = std::move(tabs_[from]);
  tabs_.erase(tabs_.begin() + from);
  tabs_.insert(tabs_.begin() + to, std::move(t));
  if (active_ == from) active_ = to;
  else if (from < active_ && to >= active_) --active_;
  else if (from > active_ && to <= active_) ++active_;
  return true;
}
std::string TabBar::titleFor(size_t i, const std::function<std::optional<std::string>(const std::string&)>& pt) const {
  if (i >= tabs_.size()) return {};
  if (pt) if (auto s = pt(tabs_[i].activePaneId); s && !s->empty()) return *s;
  return tabs_[i].title;
}
void TabBar::load(std::vector<Tab> tabs, size_t active) {
  tabs_ = std::move(tabs); active_ = tabs_.empty() ? 0 : (active < tabs_.size() ? active : 0);
}
}  // namespace uicfg
