/**
 * 域：值在排版或导出时才确定的内容（见 docs/文档模型设计.md 第八节）。
 *
 * 按值的来源分两类，只有第一类是本次排版的结果，也是唯一会产生迭代的一类：页码与总页数
 * 依赖上一轮的页数，页脚同时写页码与总页数时两者会互相追着跑；打印日期与文档属性来自
 * 文档之外，取出来就固定。患者姓名这类信息不是域，而是绑定业务 key 的控件值（第七节），
 * 所以域只剩这两类。
 *
 * 两条纪律：求值只写缓存值、不产生编辑步骤，否则打开文档就会往撤销栈里塞东西；
 * 域元素在流里是原子，占一个位置、参与断行，不可被拆开。
 *
 * 与 para / marks / control 一样挂在节点上、只对 Field 有意义：求值要按流顺序走一遍树，
 * 而流顺序就是树的顺序。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/rev/node_id.h"

namespace heditor {

class NodeTree;

/**
 * 域的种类。排版结果类需要迭代，运行时刻类不需要。PrintDate 的语义是导出或打印那一刻，
 * 不是打开那一刻，否则昨天打开的文档会一直显示昨天的日期；DocProperty 取文档属性表里的
 * 一个命名值。
 */
enum class FieldKind : uint8_t { PageNumber = 0, PageCount = 1, PrintDate = 2, DocProperty = 3 };

/**
 * 域种类的名字。
 * @param kind 种类
 * @return 名字
 */
const char* fieldKindName(FieldKind kind);

/**
 * 名字转域种类。
 * @param name 名字
 * @param out 写回种类
 * @return true = 名字认识
 */
bool fieldKindFromName(const std::string& name, FieldKind* out);

/**
 * 这个种类的域是不是本次排版的结果。是则它的值依赖上一轮排版，页脚同时写页码与总页数时
 * 正是这一条让它有循环风险 —— 所以它是"哪一类域需要迭代"的唯一判据。
 * @param kind 域种类
 * @return true = 这类域的值来自排版结果（页码 / 总页数）
 */
bool isLayoutResult(FieldKind kind);

struct FieldSpec {
    /**
     * 域的种类。
     */
    FieldKind kind = FieldKind::PageNumber;

    /**
     * 文档属性名，只对 DocProperty 有意义。其余种类读它没有定义。
     */
    std::string name;

    /**
     * 缓存值：上一次求值算出来的文本。它随域落盘（重新打开时先显示已有的值而不是空），
     * 但它是派生值，编辑步骤不产生它 —— 由求值函数写入。
     */
    std::string cache;

    bool operator==(const FieldSpec& o) const = default;
};

/**
 * 求值的输入。模型层没有时钟、也不假装有：打印日期与文档属性由宿主填进来，
 * 页码与总页数由排版填进来（那是本次排版的结果）。
 */
struct FieldContext {
    /**
     * 这一页的页码。
     */
    int page_number = 0;

    /**
     * 文档总页数。
     */
    int page_count = 0;

    /**
     * 打印日期（由宿主给，模型没有时钟）。
     */
    std::string print_date;

    /**
     * 文档属性表。
     */
    std::map<std::string, std::string> doc_properties;
};

/**
 * 一个域在给定上下文里的值。四种取值都只依赖上下文，所以对同一份上下文重复调用得到同一个
 * 结果 —— 求值幂等的前提。DocProperty 在属性表里查不到时给空串：属性缺失是常规状态，
 * 不猜也不报错。
 * @param field 域记录
 * @param ctx 求值上下文
 * @return 值（属性查不到时为空串）
 */
std::string fieldValue(const FieldSpec& field, const FieldContext& ctx);

/**
 * 一次求值的读数。examined 是看到的域个数，updated 是缓存值真的变了的那几个 ——
 * 幂等判据看的就是第二次求值的 updated 为 0。
 */
struct FieldEvalResult {
    /**
     * 看到的域个数。
     */
    int examined = 0;

    /**
     * 缓存值真的变了的个数（第二次求值应为 0）。
     */
    int updated = 0;
};

/**
 * 把全篇的域按给定上下文求值，只写每个域的缓存值。
 *
 * 它不接收事务、也不返回编辑步骤，所以在结构上不可能往撤销栈里塞东西。正文按树顺序走，
 * 页眉页脚的片段由节的记录引用、不在正文流里，所以另外按记录里的根逐个走一遍。
 * @param tree 文档树
 * @param ctx 求值上下文
 * @return 看了几个域、几个缓存值真的变了
 */
FieldEvalResult evaluateFields(NodeTree& tree, const FieldContext& ctx);

}  // namespace heditor
