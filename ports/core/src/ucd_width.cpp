// SPDX-License-Identifier: GPL-2.0-only
#include "ucd_width.h"
#include <algorithm>
#include <cstddef>

namespace itcore::ucd {
namespace {
struct UcdRange { uint32_t lo, hi; };
#include "ucd_width_table.inc"
template <size_t N>
bool in(const UcdRange (&t)[N], uint32_t cp) {
    auto it = std::upper_bound(t, t + N, cp, [](uint32_t c, const UcdRange &r) { return c < r.lo; });
    return it != t && cp <= (it - 1)->hi;
}
}  // namespace

const char *version() { return ITCORE_UCD_VERSION; }
bool is_wide(uint32_t cp) { return in(kUcd_wide, cp); }
bool is_ambiguous(uint32_t cp) { return in(kUcd_ambiguous, cp); }
bool is_combining(uint32_t cp) { return in(kUcd_combining, cp); }
bool is_ignorable(uint32_t cp) { return cp == 0x200B || in(kUcd_ignorable, cp); }
bool is_ext_pict(uint32_t cp) { return in(kUcd_ext_pict, cp); }
bool is_modifier_base(uint32_t cp) { return in(kUcd_modifier_base, cp); }
bool is_modifier(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
bool is_vs16_base(uint32_t cp) { return in(kUcd_vs16_base, cp); }
bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

int width(uint32_t cp, bool ambiguous_wide) {
    if (cp == 0 || is_combining(cp) || is_ignorable(cp)) return 0;
    if (cp < 0x1100) return (ambiguous_wide && is_ambiguous(cp)) ? 2 : 1;  // 上游快速路径
    if (is_wide(cp) || is_regional_indicator(cp)) return 2;
    if (ambiguous_wide && is_ambiguous(cp)) return 2;
    return 1;
}
}  // namespace itcore::ucd
