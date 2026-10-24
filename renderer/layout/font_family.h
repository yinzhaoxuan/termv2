#pragma once
// 启动字体族选择（平台无关，FONT-02）：settings 里的族名在系统字体集合里找不到时，
// 依次退回内置等宽默认（Consolas → Courier New），不再拿不存在的族名建 TextFormat、度量退回估算。
#include <string>

namespace itren {

// 与 Impl::family / PaneBackendConfig::fontFamily 的默认一致；Courier New 是 Windows 自带的最后兜底
inline constexpr const wchar_t *kFallbackFontFamilies[] = {L"Consolas", L"Courier New"};

// resolve(name, &out)：族存在时写入归一后的族名并返回 true。
// 返回选中的族名；全都找不到返回空串（调用方保持原样，由 DirectWrite 自行回退）。
// *fell_back：选中的不是 requested（含归一后的名字）时置 true。
template <class Resolve>
std::wstring pick_font_family(const std::wstring &requested, Resolve &&resolve, bool *fell_back = nullptr) {
    std::wstring out;
    if (fell_back) *fell_back = false;
    if (!requested.empty() && resolve(requested, &out) && !out.empty()) return out;
    for (const wchar_t *f : kFallbackFontFamilies) {
        out.clear();
        if (requested == f) continue;  // 刚试过
        if (resolve(std::wstring(f), &out) && !out.empty()) { if (fell_back) *fell_back = true; return out; }
    }
    return std::wstring();
}

}  // namespace itren
