// SPDX-License-Identifier: GPL-2.0-only
// iTerm2 Windows 派生核心：UCD 宽度/字素分类（静态表由 tools/gen_width_table.py 生成）。
// 派生自 iTerm2 sources/CharacterSets/iTermCharacterWidth.c 与
// sources/ScreenChar/ScreenChar.m（StringToScreenChars）。固定提交见 UPSTREAM.md。
#pragma once
#include <cstdint>

namespace itcore::ucd {
const char *version();
bool is_wide(uint32_t cp);           // EAW W/F（Unicode 9+ 语义）
bool is_ambiguous(uint32_t cp);      // EAW A（默认按窄处理，同上游 ambiguousIsDoubleWidth=NO）
bool is_combining(uint32_t cp);      // Mn/Me/Mc、Hangul V/T、ZWJ/ZWNJ、VS、TAG、肤色修饰
bool is_ignorable(uint32_t cp);      // Default_Ignorable（不占格、丢弃）+ U+200B
bool is_ext_pict(uint32_t cp);       // Extended_Pictographic（ZWJ 序列后继）
bool is_modifier_base(uint32_t cp);  // Emoji_Modifier_Base
bool is_modifier(uint32_t cp);       // U+1F3FB..1F3FF
bool is_vs16_base(uint32_t cp);      // emoji-variation-sequences 中接受 FE0F 的基字
bool is_regional_indicator(uint32_t cp);
// 单码点列宽：0=零宽/并入前字素，1，2。区域指示符按上游 fullWidthFlags=YES 记 2。
int width(uint32_t cp, bool ambiguous_wide = false);
}  // namespace itcore::ucd
