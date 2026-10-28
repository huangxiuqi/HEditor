/**
 * 字符属性（marks）：字符级的样式集合，挂在文本节点上（第二节）。
 *
 * 每个字段都是三态的：未设置（继承样式层的值，见 1.6）、或已设为某个具体值。为什么不能给个
 * 默认值了事：run 上的属性表达的是"相对样式层的一次覆盖"，只有区分"没写"与"写成了默认值"，
 * 继承才算得对。依据：OOXML 的 w:rPr 就是"属性出现与否"的表达，不出现即继承。
 *
 * 标记刻意不带 ID（第二节）：它是文本节点上的属性集合，随规范化自动合并与拆分。把标记做成
 * 带 ID 的节点，"合并相邻同格式文本"这条不变量就无法成立 —— 合并会让两个身份变成一个。
 *
 * 字段形状是"结构体 + 可选字段"而不是键值列表：无重复、顺序固定这两条因此不可违反，
 * 不需要另立规则去查（1.6 引入样式引用时会再决定要不要变成表）。
 */
#pragma once

#include <optional>

#include "core/doc/length.h"
#include "core/utils/types.h"

namespace heditor {

struct CharFormat {
    /**
     * 加粗；没设置表示继承。
     */
    std::optional<bool> bold;

    /**
     * 斜体。
     */
    std::optional<bool> italic;

    /**
     * 下划线。
     */
    std::optional<bool> underline;

    /**
     * 字体（引擎侧是字体库下标；字体名与回退链到 1.6 的样式表再定）。
     */
    std::optional<int> font_index;

    /**
     * 字号。用 Length 而不是磅值：绝对长度按 EMU 存，与段落格式同一套表示（第十一节）。
     */
    std::optional<Length> size;

    /**
     * 字色。
     */
    std::optional<Color> color;

    /**
     * 一个属性都没设置吗。规范化靠它判断"标记集合是否相同"，编译靠它判断"有没有被丢掉的
     * 字符样式"。
     * @return true = 一个属性都没设置（等于"全部继承"）
     */
    bool empty() const {
        return !bold.has_value() && !italic.has_value() && !underline.has_value() &&
               !font_index.has_value() && !size.has_value() && !color.has_value();
    }

    /**
     * 逐字段相等就是"标记集合相同"。合并相邻文本节点的判据正是它，所以必须是全字段比较，
     * 不能只看其中几个。
     */
    bool operator==(const CharFormat& o) const = default;
};

/**
 * 逐字段合并：over 里设置了的覆盖 under，没设置的保留 under 的。
 *
 * 样式链的三层解析就是它按"从根到叶"的顺序叠加，内联 marks 最后叠一次。放在这一层而不是
 * 样式层，是因为它只认这六个字段，与样式表没有关系。
 * @param under 下层（被覆盖）
 * @param over 上层（设置了的覆盖下层）
 * @return 合并结果
 */
CharFormat mergeCharFormat(const CharFormat& under, const CharFormat& over);

/**
 * 逐字段取"两个标记都认同的部分"：不相等的那一项清成未设置。
 *
 * 它是回显用的：工具栏要回答"选中这一段是什么格式"，而一段里可能有一半是粗体。三态在这里
 * 正好合适 —— 缺席就表示"这一段没有统一的值"，按钮不点亮，用户按下去等于给它统一成粗体。
 * @param a 第一个标记
 * @param b 第二个标记
 * @return 共同值
 */
CharFormat intersectCharFormat(const CharFormat& a, const CharFormat& b);

}  // namespace heditor
