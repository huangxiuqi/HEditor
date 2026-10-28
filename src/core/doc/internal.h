/**
 * 修订模块的内部工具：路径复制、定位、孩子表改写。
 *
 * 只给本模块的另一个编译单元（步骤层）用。放在一个自有头里而不是各自复制：这些工具定义
 * "改一处等于换一条路径"的唯一做法，复制一份就会长出第二条构造路径，哈希迟早对不上。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/doc/document.h"

namespace heditor::doc::detail {
/**
 * 同内容换孩子表。哈希与段落数由孩子重算，所以调用方给的是内容而不是派生量。
 * @param node 原节点
 * @param children 新的孩子表
 * @return 换过孩子的新节点（哈希重算）
 */
Ref withChildren(const Ref& node, std::vector<Ref> children);

/**
 * 换掉一棵子树里的一个节点，沿路径复制。没找到时原样返回同一个指针。
 * @param node 原节点
 * @param target 要替换的节点号
 * @param replacement 替换成什么
 * @param found 写回"找到了没有"
 * @return 换过的新节点
 */
Ref replaceNode(const Ref& node, NodeId target, const Ref& replacement, bool* found);

/**
 * 一个节点在流里贡献多少个可寻址码点。段落标记与控件边界都不占码点。
 * @param n 节点
 * @return 这棵子树占几个流位置
 */
int flowLen(const Ref& n);

/**
 * 块级容器：内容自成段落序列，不从属于外面那一段。表格与它的行、格、节都是。
 * @param kind 节点种类
 * @return true = 块级容器（表格 / 行 / 格 / 节）
 */
bool isBlockContainer(NodeKind kind);

/**
 * 段落内容在流里的落点。InRun 落在某个 run 内部；BeforeChild 落在两项之间
 * （段首、控件之前、段末标记之前）。容器内部不是另一种落点，只是 path 更长。
 */
enum class SiteKind : uint8_t { InRun, BeforeChild };

/**
 * (段号, 段内码点) 落在哪儿。
 *
 * path 既是递归栈也是结果：到"落点所在那一层孩子表"的下标链，空表示根的孩子表。
 * 位置可能有两种落法（上一段末尾与下一段开头是同一处边界），这里取靠前的那一个：
 * 两种落法对文本结果等价，但必须定死一种，否则同一份编辑会产出两棵不同的树。
 */
struct Site {
    /**
     * 落在 run 内，还是在某一层孩子表之前。
     */
    SiteKind kind = SiteKind::BeforeChild;

    /**
     * 孩子表里的下标（BeforeChild 时有效）。
     */
    int child = -1;

    /**
     * run 内的码点偏移（InRun 时有效）。
     */
    int off = 0;
};

/**
 * 把 (段号, 段内码点) 落成"哪一层孩子表的哪个位置"。
 * @param kids 孩子表（从根那一层开始）
 * @param para 段号
 * @param cp 段内码点位置
 * @param path 写回落点路径
 * @param out 写回落法
 * @return true = 落得进去
 */
bool locate(const std::vector<Ref>& kids, int para, int cp, std::vector<int>* path, Site* out);

/**
 * 第 para 段的结束标记落在哪一层：path 指到那一层，*at 是它在该层孩子表里的下标。
 * @param kids 孩子表
 * @param para 段号
 * @param path 写回落点路径
 * @param at 写回标记在孩子表里的下标
 * @return 结束标记节点；这一段还没结束标记时为 nullptr
 */
bool markOf(const std::vector<Ref>& kids, int para, std::vector<int>* path, int* at);

/**
 * 按路径取到某一层的孩子表。路径越界时返回 nullptr。
 * @param doc 修订
 * @param path 孩子表路径
 * @return 那一层的孩子表；路径不存在时为 nullptr
 */
const std::vector<Ref>* kidsAt(const Doc& doc, const std::vector<int>& path);

/**
 * 沿路径复制：把第 path[0] 个孩子换成 fn(它) 的结果，逐层换到根。
 * @param node 从哪里往下走
 * @param path 孩子表路径
 * @param k 路径走到第几层
 * @param fn 对这一层孩子表做什么
 * @return 改过的新节点
 */
Ref applyAtPath(const Ref& node, const std::vector<int>& path, size_t k,
                const std::function<Ref(const Ref&)>& fn);

/**
 * 把"落点那一层的新孩子表"装回去，返回新的根。path 为空时改的是根。
 * @param doc 修订
 * @param path 孩子表路径
 * @param kids 新的孩子表
 * @return 换过这一层孩子的新修订
 */
Ref withLeafChildren(const Doc& doc, const std::vector<int>& path, std::vector<Ref> kids);

/**
 * 造一个文本 run 节点（内部用：载荷与哈希一并算好）。
 * @param id 节点号
 * @param text 码点
 * @param marks 字符格式
 * @param style 字符样式引用
 * @return 新节点
 */
Ref runNode(NodeId id, std::vector<uint32_t> text, const CharFormat& marks,
            const std::string& style);

}  // namespace heditor::doc::detail
