/* 渲染用模拟快照：实现 itcore.h 的快照访问器，供 T05 完成前驱动渲染原型与测试。
   不与真实 itcore 同时链接。 */
#pragma once
#include "itcore.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 由 UTF-8 文本构造快照：'\n' 分行为硬换行，超出列宽自动软换行；宽字符（CJK/全角）占 2 格并生成尾格。
   cursor 置于最后写入位置之后。返回的快照用 itcore_snapshot_release 释放。 */
itcore_snapshot *itmock_snapshot_from_utf8(const char *utf8, size_t len, uint16_t columns, uint16_t rows);
/* 为指定格设置样式与颜色（测试 SGR 渲染用） */
void itmock_set_style(itcore_snapshot *s, uint16_t row, uint16_t col, uint16_t flags, uint32_t fg, uint32_t bg);
/* 模拟字素池换存储（RIS/ED 3/压缩） */
void itmock_bump_epoch(itcore_snapshot *s);
#ifdef __cplusplus
}
#endif
