/**
 * 片段索引与后缀重新分页。
 *
 * FragmentIndex 是页/块/行三张反查表的唯一持有者。整篇排版收尾时重建一次；增量排版只重排
 * 脏区时，后缀的重新分页也从这里的块行切片出发，不再按标量状态自己扫一遍行盒。
 */
#pragma once

#include <vector>

#include "core/layout/layout.h"

namespace heditor {

/**
 * 后缀重新分页：把 [from, to) 段的行沿块轴重新落位，并重编号码与全局行号。
 *
 * 行高、页底、页首段前距与段前分页由 planBlockLines 给出，与整篇排版同一条规则；
 * 断行结果（簇与行内位置）一个字节都不重算。块行切片读 FragmentIndex，竖排段不属于这条
 * 路径。校验与改写分两趟：返回 false 时行盒、字形与 ParaIndex 都没有改动，只有块行表
 * 可能已经刷新，调用方收尾时的 rebuild 会一起换掉。
 *
 * @param layout 要改写的排版结果；行盒与字形按段序追加
 * @param blocks 块流；只读段前分页、段前距与段后距
 * @param page 页度量
 * @param from 后缀第一段（含）
 * @param to 后缀上界（不含）；通常是后缀里第一张表的起始段
 * @param anchor 后缀第一段的起始续排状态（段首分页与段前距生效之前）
 * @param end_anchor 出参：走到 to 时的续排状态
 * @return true = 已改写；false = 后缀含竖排或字形与行号对不上，未改动
 */
bool repaginateSuffix(LayoutResult* layout, const std::vector<BlockSpec>& blocks,
                      const PageMetrics& page, int from, int to, const ReflowState& anchor,
                      ReflowState* end_anchor);

}  // namespace heditor
