/**
 * 修订层的落盘格式：修订与 JSON 文本互转。
 *
 * 只存形状与身份，不存派生量：哈希与段落数在重建时由孩子重算。于是往返的判据不是"文本相同"，
 * 而是"重算出来的全文哈希相同" —— 它比逐字段比对更强：任何一个参与哈希的字段没落盘，哈希就会
 * 变。反过来，种类、隐藏标记、载荷、孩子都在哈希里，剩下的身份（节点号）与分配器水位单独断言。
 *
 * 记录级字段（字符属性、段落覆盖、外观、控件）的编码与模型层共用 record_json，两套格式里同一个
 * 字段只有一个名字。这一份覆盖本层表示得了的形状：文字、段落标记、控件。
 *
 * 读坏了一份文件时 out 一个字节都不动，调用方拿到的失败回执与"修订没变"是同一件事。
 */
#pragma once

#include <string>

#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 落盘格式的版本号。读到别的版本一律拒绝：猜着读会把一份新文件读成一份看起来正常的旧结构。
 */
inline constexpr int kRevFormatVersion = 1;

struct SerializeResult {
    bool ok = false;
    std::string text;
    std::string error;
};

struct DeserializeResult {
    bool ok = false;
    std::string error;
};

/**
 * 写成 JSON 文本（无空白）。节点种类与枚举按名字写，浮点写十进制字符串。
 */
SerializeResult serialize(const Doc& doc);

/**
 * 从 JSON 文本重建修订。成功时 next_id 取自文件，节点号原样保留。
 */
DeserializeResult deserialize(const std::string& text, Doc* out);

}  // namespace heditor::rev
