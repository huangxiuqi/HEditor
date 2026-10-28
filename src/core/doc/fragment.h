/**
 * 片段：从文档里取出来的一段内容，可以原样插回。
 *
 * 它只装文本与段落标记 —— 这是 1.2 的可编辑集合。含其它元素的区间在提取时就被拒并给出原因，
 * 不会产出一个半截的片段（静默截断比"还不支持"难查得多）。片段在内存里是内容的一份拷贝，
 * 与文档树没有连接，所以既能当剪贴板内容，也能当步骤的还原数据。
 *
 * 为什么片段不记节点 ID：它表达的是"内容"，而内容的落点由插回时的位置决定。文本节点的身份
 * 本来也不参与往返判据 —— 区间边界落在文本中间时原节点被切开，插回时是新的节点。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/flow.h"
#include "core/doc/node.h"
#include "core/doc/range.h"
#include "core/rev/char_format.h"
#include "core/rev/fragment_value.h"
#include "core/rev/para_format.h"

namespace heditor {
/**
 * 区间覆盖到的一个直接子节点：节点本身，以及被覆盖的那一段（流坐标）。
 */
struct Cover {
    /**
     * 被覆盖的直接子节点。
     */
    NodeId node = kInvalidNodeId;

    /**
     * 这个节点自己的流起点。
     */
    FlowPos first = 0;

    /**
     * 被覆盖部分的起点。
     */
    FlowPos from = 0;

    /**
     * 被覆盖部分的终点（开）。
     */
    FlowPos to = 0;
};

/**
 * 区间覆盖到的直接子节点清单，按流顺序。
 *
 * 覆盖到的元素一定同属一个父容器：任何夹在中间的容器都会落进区间，而容器在这个口径下不被
 * 支持、先被拒掉。于是调用方拿到的是同一层的孩子，不需要跨层处理。
 *
 * 有元素既不是文本也不是段落标记时返回 false 并写明是哪一个 —— 不静默截断。
 * @param tree 文档树
 * @param range 区间
 * @param out 写回覆盖清单
 * @param error 写回失败原因（区间里有不支持的节点）
 * @return true = 覆盖到的都是文本与段落标记
 */
bool collectCovers(const NodeTree& tree, const Range& range, std::vector<Cover>* out,
                   std::string* error);

/**
 * 同上，复用调用方已建好的流索引（Design 口径）。
 *
 * 批量删除时每一步都要取一次覆盖清单，而建一次索引是 O(节点数) —— 四千步的批次就是
 *   几千次全树遍历。索引必须与 tree 同一口径。
 * @param tree 文档树
 * @param range 区间
 * @param out 写回覆盖清单
 * @param error 写回失败原因（区间里有不支持的节点）
 * @return true = 覆盖到的都是文本与段落标记
 * @param idx 调用方建好的流索引（Design 口径）
 */
bool collectCovers(const NodeTree& tree, const FlowIndex& idx, const Range& range,
                   std::vector<Cover>* out, std::string* error);

/**
 * 提取结果。失败时 fragment 为空、error 写明原因。
 */
struct ExtractResult {
    /**
     * 取出来的片段。
     */
    Fragment fragment;

    /**
     * 取到了没有。
     */
    bool ok = false;

    /**
     * 失败原因（区间里有不支持的节点）。
     */
    std::string error;
};

/**
 * 取区间（纯读，不产生步骤）。
 *
 * 内部先做端点规范化，于是片段永远良构：不可能切在控件中间，也不会留下落单的边界。
 * @param tree 文档树
 * @param range 区间
 * @return 片段；区间不合法时为空并带原因
 */
ExtractResult extractRange(const NodeTree& tree, Range range);

}  // namespace heditor
