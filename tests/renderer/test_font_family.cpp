#undef NDEBUG  // assert 在任何构建类型下都必须执行
// FONT-02：启动字体族选择——找不到时退回 Consolas → Courier New，全都没有返回空串
#include "font_family.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <vector>
using namespace itren;

struct FakeCollection {
    std::set<std::wstring> have;
    std::vector<std::wstring> asked;
    bool operator()(const std::wstring &n, std::wstring *o) {
        asked.push_back(n);
        if (!have.count(n)) return false;
        *o = n; return true;
    }
};

int main() {
    bool fb = true;
    {  // 存在：原样，不试默认
        FakeCollection c{{L"Cascadia Mono", L"Consolas"}};
        assert(pick_font_family(L"Cascadia Mono", c, &fb) == L"Cascadia Mono" && !fb && c.asked.size() == 1);
    }
    {  // 不存在：退回 Consolas
        FakeCollection c{{L"Consolas", L"Courier New"}};
        assert(pick_font_family(L"No Such Font", c, &fb) == L"Consolas" && fb);
    }
    {  // Consolas 也没有：Courier New
        FakeCollection c{{L"Courier New"}};
        assert(pick_font_family(L"No Such Font", c, &fb) == L"Courier New" && fb);
    }
    {  // 请求的就是 Consolas 且不存在：不重复试
        FakeCollection c{{L"Courier New"}};
        assert(pick_font_family(L"Consolas", c, &fb) == L"Courier New" && fb && c.asked.size() == 2);
    }
    {  // 全都没有：空串，调用方保持原样
        FakeCollection c{{}};
        assert(pick_font_family(L"No Such Font", c, &fb).empty() && !fb);
    }
    {  // 空族名：直接走默认
        FakeCollection c{{L"Consolas"}};
        assert(pick_font_family(L"", c, &fb) == L"Consolas" && fb);
    }
    {  // 归一（GDI 名 → DirectWrite 族名）不算回退
        auto r = [](const std::wstring &n, std::wstring *o) {
            if (n == L"Cascadia Mono SemiBold") { *o = L"Cascadia Mono"; return true; }
            return false;
        };
        assert(pick_font_family(L"Cascadia Mono SemiBold", r, &fb) == L"Cascadia Mono" && !fb);
    }
    std::puts("font_family OK");
    return 0;
}
