/**
 * 序列化：文档模型 <-> JSON 文本（见 docs/文档模型设计.md 第十三节）。
 *
 * 形状是扁平不嵌套的：一张节点表（按深度优先序拍平，每个节点只记自己的孩子个数）、
 * 一张符号表（节点种类名与其它名字字符串，正文里只出现下标）、一个根列表（正文根与各节
 * 页眉页脚片段的根），外加一个分配器水位。段落不落盘 —— 段落是派生量，落盘的只有段落标记
 * 与它的属性，重新打开时由标记重新导出段落。
 *
 * 落盘的四条硬约定：无空白（只用于人工调试的输出才缩进）、默认值不写、浮点写十进制字符串
 * （否则 parse(print(x)) == x 会失败）、枚举按名字写（按枚举值写的话，将来重排枚举会让旧文件
 * 静默变成另一个意思）。
 *
 * 不带两样东西：摘下的子树（撤销留下的池内节点，它们不是文档内容），以及样式表与资源表
 * （1.6 起由调用方持有、还没挂到 Document 上，所以随"Document 挂样式表"那一步一起进来）。
 * 分配器水位要落盘，于是重新打开之后新分配的 id 不会与文件里的撞上。
 *
 * 记录级（字符属性、段落覆盖、外观、控件）的编解码在 record_json，与修订层的落盘共用
 * 同一份：两处各写一遍字段映射，迟早只改一处而另一处静默读成默认值。nlohmann 的类型只出现
 * 在 record_json.h 与两个序列化 .cpp 里，这里的签名不出现。
 */
#pragma once

#include <string>

#include "core/doc/node.h"

namespace heditor {

/**
 * 落盘格式的版本号。读到别的版本一律拒绝，不做"尽力而为"的迁移 —— 猜着读会把一份新文件
 * 读成一份看起来正常的旧结构。
 */
inline constexpr int kSerializeFormatVersion = 1;

/**
 * 序列化结果。写不出来只有一种可能：某个字符串不是合法 UTF-8。写出之前自己查一遍 ——
 * JSON 库对非法字节的处理是抛异常（有异常的环境）或终止进程（wasm 关掉异常），
 * 两条路都不该走到，所以宁可在这里返回失败并说明原因。
 */
struct SerializeResult {
    bool ok = false;
    std::string text;
    std::string error;
};

/**
 * 把文档写成 JSON 文本（无空白）。
 */
SerializeResult serializeDocument(const NodeTree& tree);

/**
 * 反序列化的结果。失败时 error 写明是哪一处读不下去（键名、节点下标、还是枚举名），
 * 并且 out 不动 —— 读坏了就什么都不改，不交出一棵半截的树。
 */
struct DeserializeResult {
    bool ok = false;
    std::string error;
};

/**
 * 从 JSON 文本重建文档。out 会被整体替换（先清成空文档再按记录装回去，节点 id 原样保留）。
 */
DeserializeResult deserializeDocument(const std::string& text, NodeTree* out);

}  // namespace heditor
