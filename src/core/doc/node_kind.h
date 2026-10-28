/**
 * 节点种类与种类判定：不属于树结构，只描述结构语义。
 *
 * 这些判定同时被修订投影与旧模型编译使用；先收进值层，树容器随后再删。
 */
#pragma once

#include <cstdint>
#include <string>

namespace heditor {

/**
 * 节点种类：只回答"结构上怎么处理"这一个问题。
 */
enum class NodeKind : uint8_t {
    /**
     * 容器：持有子节点序列，在流里表现为「起始边界 … 内容 … 结束边界」
     */
    Document = 0,
    Section,
    Table,
    TableRow,
    TableCell,
    ContentControl,
    Quote,
    CodeBlock,
    ListItem,
    Link,
    /**
     * 页眉页脚内容片段根。
     */
    HeaderFooter,
    /**
     * 原子：不可再分，在流里是一个带宽度、参与断行的叶子
     */
    Image,
    Formula,
    Break,
    PageBreak,
    Tab,
    Field,
    /**
     * 文本：一段共享同一套标记的连续文本。
     */
    TextRun,
    /**
     * 段落标记。
     */
    ParagraphMark,
};

/**
 * 是否持有子节点。
 * @param kind 节点种类
 * @returns true = 持有子节点
 */
bool isContainer(NodeKind kind);

/**
 * 是否为文本节点。
 * @param kind 节点种类
 * @returns true = 文本节点
 */
bool isText(NodeKind kind);

/**
 * 是否为段落标记。
 * @param kind 节点种类
 * @returns true = 段落标记
 */
bool isParagraphMark(NodeKind kind);

/**
 * 是否为原子。
 * @param kind 节点种类
 * @returns true = 原子
 */
bool isAtom(NodeKind kind);

/**
 * 种类名。
 * @param kind 节点种类
 * @returns 名字
 */
const char* nodeKindName(NodeKind kind);

/**
 * 种类名 → 种类。
 * @param name 名字
 * @param out 写回种类
 * @returns true = 名字认识
 */
bool nodeKindFromName(const std::string& name, NodeKind* out);

/**
 * 谁允许装谁。
 * @param parent 父节点种类
 * @param child 子节点种类
 * @returns true = 允许这样嵌套
 */
bool acceptsChild(NodeKind parent, NodeKind child);

}  // namespace heditor
