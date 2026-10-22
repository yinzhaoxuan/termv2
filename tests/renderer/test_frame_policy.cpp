#include "frame_policy.h"
#include "itcore.h"
#include <cstdio>
using namespace itren;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
int main() {
    LayoutKeeper k;
    auto f = k.update(80, 24, 1, 1);
    CHECK(!f.gen_mismatch && f.cols == 80 && f.rows == 24 && f.layout_changed);
    f = k.update(80, 24, 1, 1);
    CHECK(!f.layout_changed);
    // 用户拖大窗口，期望 gen=2，核心尚未出新快照：保留 80x24
    f = k.update(80, 24, 1, 2);
    CHECK(f.gen_mismatch && f.cols == 80 && f.rows == 24 && f.layout_changed);
    // 中间快照比旧布局大/小：只画交集
    f = k.update(100, 20, 3, 2);
    CHECK(f.gen_mismatch && f.cols == 80 && f.rows == 24 && f.draw_cols == 80 && f.draw_rows == 20);
    // 对齐后采用新布局
    f = k.update(100, 30, 2, 2);
    CHECK(!f.gen_mismatch && f.cols == 100 && f.rows == 30 && f.draw_rows == 30 && f.layout_changed);
    // 首帧即不一致：无旧布局，采用快照
    LayoutKeeper k2;
    f = k2.update(40, 10, 5, 6);
    CHECK(f.gen_mismatch && f.cols == 40 && f.rows == 10);
    // 期望为 0 视为未设
    f = k2.update(50, 12, 9, 0);
    CHECK(!f.gen_mismatch && f.cols == 50);
    auto s = text_style(ITCORE_CELL_BOLD | ITCORE_CELL_UNDERLINE, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
    CHECK(s.bold && !s.italic && s.underline && s.format_index() == 1);
    s = text_style(ITCORE_CELL_BOLD | ITCORE_CELL_ITALIC, ITCORE_CELL_BOLD, ITCORE_CELL_ITALIC, ITCORE_CELL_UNDERLINE);
    CHECK(s.format_index() == 3 && !s.underline);
    if (!failures) std::printf("frame_policy OK\n");
    return failures ? 1 : 0;
}
