// 滚动历史搜索的后端驱动（平台无关，UI 线程）：把 uishell findInScrollback / clearSearch 转发到
// itcore_search_*（草案 /workspace/qa/itcore_search-草案-v0.2.md），step 按预算分批，由定时器续跑。
// v0.1 只做纯文本；regex=true 直接 found=false，不退回子串搜索。
#pragma once
#include "itcore.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace it2app {

class PaneSearch {
public:
    struct Options {
        uint8_t caseMode = ITCORE_SEARCH_CASE_SMART;  // ITCORE_SEARCH_CASE_*
        bool regex = false;
        bool wrap = true;
        bool operator==(const Options &) const = default;
    };
    // 与 uishell::SearchResult 一一对应，另加视口位置
    struct State {
        bool found = false;
        int index = 0;
        int total = -1;
        bool hasNewOutput = false;
        bool restarted = false;
        bool pending = false;
        bool hasView = false;   // found 且当前结果能映射到视口坐标
        int32_t viewRow = 0;    // 相对网格顶的行（负 = 历史），同 Viewport::firstRow 的坐标系
        uint16_t viewCol = 0;
        bool moved = false;     // tick：挂起（PENDING）的移动在本次落定，调用方应把视口滚到 viewRow
        bool operator==(const State &) const = default;
    };
    // 一次查找时 step 的预算（UI 线程同步调用，保持很小）
    static constexpr uint32_t kFindCells = 64 * 1024, kFindUs = 2000;

    PaneSearch() = default;
    PaneSearch(const PaneSearch &) = delete;
    PaneSearch &operator=(const PaneSearch &) = delete;
    ~PaneSearch() = default;  // 句柄归核心所有：调用方须在 itcore_destroy 之前 end()

    // needle 或选项与上次不同 → 新一轮（end + begin）；否则在当前结果上移动（backward=prev，否则 next）
    State find(itcore_core *core, std::string_view needle, const Options &opt, bool backward);
    // 定时器调用：按预算 step 一次，推进计数 / 落定 PENDING / 发现失效与新输出。状态有变化时返回新状态
    std::optional<State> tick(uint32_t budgetCells, uint32_t budgetUs);
    bool active() const { return h_ != nullptr; }
    bool busy() const { return h_ && (status_ == ITCORE_SEARCH_RUNNING || pendingDir_.has_value()); }
    void end();
    // 视口内的高亮区间（草案 §2.6），first_row 为绝对行号；渲染层接入前供调用方使用
    std::vector<itcore_search_range> ranges(int32_t firstViewRow, uint32_t rows) const;

private:
    int step(uint32_t cells, uint32_t us);
    State restartedState();
    void applyMove(int st, const itcore_search_match &m, int64_t idx, State &s);

    itcore_core *core_ = nullptr;
    itcore_search *h_ = nullptr;
    std::string needle_;
    Options opt_{};
    int status_ = ITCORE_SEARCH_DONE;
    int64_t total_ = -1, current_ = -1;
    uint32_t flags_ = 0;
    std::optional<bool> pendingDir_;  // PENDING 的 next(false) / prev(true)
    State last_{};
};

}  // namespace it2app
