/**
 * 投影缓存：修订投影出的块流、块位置与结构索引。
 *
 * 投影产物的所有权在这一层：它只持有数据，重建逻辑仍在引擎的 rebuildProjection。
 * 块的唯一真相仍是修订，这里是可以整份丢弃重算的派生量。
 */
#pragma once

#include <vector>

#include "core/compile/compile_result.h"
#include "core/compile/project.h"
#include "core/engine/structure_index.h"
#include "core/layout/layout.h"

namespace heditor {

/**
 * 投影缓存：块流、块位置、表格与结构索引的所有者。
 */
class ProjectionService {
public:

    /**
     * 筛选后的排版输入；名单覆盖全部控件时为空，调用方用 blocks_full。
     */
    std::vector<BlockSpec> blocks;

    /**
     * 未筛选的全部块。
     */
    std::vector<BlockSpec> blocks_full;

    /**
     * 每块的局部尺寸。
     */
    std::vector<doc::BlockExtent> block_extents;

    /**
     * 每块内容起点的全局流位置。
     */
    std::vector<FlowPos> block_flow;

    /**
     * 每块内容占的位数。
     */
    std::vector<int> block_flow_span;

    /**
     * 每块的来源身份。
     */
    std::vector<BlockSource> block_src;

    /**
     * 表格结构。
     */
    std::vector<LayoutTable> tables;

    /**
     * 投影槽位。
     */
    std::vector<doc::Slot> slots;

    /**
     * 投影历史。
     */
    doc::BlockHistory history;

    /**
     * 结构索引。
     */
    StructureIndex structure;
};

}  // namespace heditor
