/**
 * 解析结果缓存：按元素与样式表版本失效。
 */
#pragma once

#include <unordered_map>
#include <vector>

#include "core/doc/node.h"
#include "core/rev/style.h"

namespace heditor {

class StyleCache {
public:

    explicit StyleCache(const StyleSheet& sheet);

    /**
     * 一个段落标记的段落格式。第二次查同一个标记且样式表没改过时直接命中。
     * @param tree 文档
     * @param mark_id 段落标记的节点 id
     * @return 解析出来的段落格式（缓存的引用）
     */
    const ParaFormat& paragraph(const NodeTree& tree, NodeId mark_id);

    /**
     * 一段文本的字符格式。para_style 由它所在的那一段给出：同一段里的 run 共用一个段落样式，
     * 换了段落样式就是另一份结果，所以缓存记下上一次用的段落样式名，不同就当没命中。
     * @param tree 文档
     * @param run_id 文本节点的节点 id
     * @param para_style 它所在段的段落样式名
     * @return 解析出来的字符格式（缓存的引用）
     */
    const CharFormat& run(const NodeTree& tree, NodeId run_id, const std::string& para_style);

    /**
     * 某个元素自己改了（内联覆盖变了、文本被重写）时只失效它一个。
     * @param id 改了的那个元素
     */
    void invalidate(NodeId id);

    /**
     * 清掉全部缓存条目与诊断（计数不动）。
     */
    void clear();

    /**
     * 真正跑过解析的次数。
     * @return 次数
     */
    int computations() const { return computations_; }

    /**
     * 缓存命中的次数。
     * @return 次数
     */
    int hits() const { return hits_; }

    /**
     * 缓存里现在有多少条。
     * @return 条数
     */
    int entries() const { return static_cast<int>(paragraphs_.size() + runs_.size()); }

    /**
     * 解析过程中攒下的诊断（成环、未知样式名），按原文去重。
     * @return 诊断（按原文去重）
     */
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:

    /**
     * 样式表版本变了就整体作废。每次查询前调一次。
     */
    void syncVersion();

    /**
     * 样式表（不持有）。
     */
    const StyleSheet& sheet_;

    /**
     * 缓存对应的样式表版本；与样式表不符时整体作废。
     */
    int version_ = 0;

    /**
     * 段落标记的解析结果（键是标记节点 id）。
     */
    std::unordered_map<NodeId, ParaFormat> paragraphs_;

    /**
     * 文本节点的解析结果，附带解析时用的段落样式名（换了段落样式就不算命中）。
     */
    std::unordered_map<NodeId, std::pair<std::string, CharFormat>> runs_;

    /**
     * 解析过程中攒下的诊断（成环、未知样式名）。
     */
    std::vector<std::string> diagnostics_;

    /**
     * 真正跑过解析的次数。
     */
    int computations_ = 0;

    /**
     * 缓存命中的次数。
     */
    int hits_ = 0;
};

}  // namespace heditor
