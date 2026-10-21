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

#include <functional>
#include <string>

#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 落盘格式的版本号。
 *
 * 每一次"改含义"都升一格，读端接受 [kMinReadableVersion, kRevFormatVersion]：
 *
 * v2 相对 v1：文本写成 UTF-8 字符串（v1 是码点数组，那是内存里的表示泄漏到文件里），
 * 顶层带一张字体表（v1 只有 fi 这种库下标，换台机器就是另一个字体）。
 * v3 相对 v2：字体表每一项从字符串（文件名）变成对象 —— 标识换成 PostScript 名，
 * 族名给人看，文件名排在最后。文件名可以改，字体本身不能，所以文件名不该是身份。
 *
 * 读端按类型分派而不是按版本号分派（文本是字符串还是数组、字体项是对象还是字符串，
 * 类型本身就是证据），版本号只用来拒绝"未来"的文件。
 */
inline constexpr int kRevFormatVersion = 3;

/**
 * 还能读的最低版本。往下加版本时不要动它：老文件不会因为文件格式进步而失效。
 */
inline constexpr int kMinReadableVersion = 1;

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
 * 一张字体在文件里的身份。
 *
 * 三样都指同一张脸，但稳定性递减：PostScript 名（OpenType 里唯一性有规范保证）→ 族名
 * （同一族可能有多张，只作次选）→ 文件名（可以改，所以排在最后）。写端填它能填的，
 * 读端按同样的顺序找。
 */
struct FontRef {
    std::string postscript;
    std::string family;
    std::string file;
};

/**
 * 写出去时要知道的事实。只有字体一项：文档里存的是字体库下标，而库的装载顺序是进程的事，
 * 文件必须把"第 0 号是哪张脸"一并写下来。
 */
struct SerializeOptions {
    /**
     * 字体下标 → 字体身份。空则表示这一侧不认识字体库，文件里就不带字体表。
     */
    std::function<FontRef(int)> font_ref;
};

/**
 * 打开时要知道的事实。与 SerializeOptions 对称：文件里的字体标识换成本机的下标。
 */
struct DeserializeOptions {
    /**
     * 字体身份 → 本机下标。返回负数表示这一侧没有这张脸，调用方保留文件里的原下标
     * （按回退链去挑一张能用的，比换成一个随机的下标强）。
     */
    std::function<int(const FontRef&)> resolve_font;
};

/**
 * 写成 JSON 文本（无空白）。节点种类与枚举按名字写，文本写 UTF-8，浮点写最短十进制。
 */
SerializeResult serialize(const Doc& doc, const SerializeOptions& opt = {});

/**
 * 从 JSON 文本重建修订。成功时 next_id 取自文件，节点号原样保留。
 */
DeserializeResult deserialize(const std::string& text, Doc* out,
                              const DeserializeOptions& opt = {});

}  // namespace heditor::rev
