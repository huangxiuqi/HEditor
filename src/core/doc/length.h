/**
 * 长度：长度不只有"绝对"这一种性质（第十一节）。
 *
 * 必须用类型区分三种性质，不能一律塞成一个数字：绝对（EMU 整数）、相对于某个参照的比例
 * （百分比 / 相对字号 / 相对字符位）、以及"由内容决定"的自动值。混成一个 float 会在三处
 * 直接出错 —— 表格算列宽时不知道百分比相对谁、行距的倍数无法与固定值区分、自动值没有
 * 数值可以表达。所以这里是带标签的联合，不是一个数。
 *
 * 数值一律是整数或整数比：绝对长度存 EMU 整数，比例存分子分母。于是落盘不经过浮点、
 * 相等比较可靠（第十一节的三条理由）。引擎要磅值时由编译做一次乘除。
 *
 * 本类型不做"求值"（Length → EMU 需要一个参照：字号、容器宽度、字符宽度）。求值要等真有
 * 使用它的那一步（表格列宽、首行缩进、控件固定宽度）把参照带进来，现在给一个没有参照的
 * 求值只会变成猜。
 */
#pragma once

#include <cstdint>
#include <string>

#include "core/utils/units.h"

namespace heditor {

/**
 * 长度的性质。Em 与 Chars 刻意分开：中文全角字是 1 em，但数字与字母是半角（约 0.5 em），
 * "3 个字符宽"在数字字段上会算成一半。Chars 表示"n 个全角字符位"，
 * 与文档网格的"每行多少字"是同一个度量（第七节控件固定宽度）。
 */
enum class LengthKind : uint8_t {
    Abs,      // 绝对：EMU 整数
    Percent,  // 与参照长度成比例：分子 / 分母
    Em,       // 相对当前字号的倍数（1 em = 字号）
    Chars,    // n 个全角字符位
    Auto,     // 无值：由内容或参照方决定
};

/**
 * 长度值。字段按种类取用，与 Node 的 children / text 是同一个约定。
 */
struct Length {
    LengthKind kind = LengthKind::Auto;

    /**
     * Abs 的长度（EMU）。其余种类不用。
     */
    Emu abs_emu = 0;

    /**
     * Percent / Em / Chars 的分子与分母。构造时按最大公约数约分、分母恒正、num 为 0 时
     * 分母归一为 1，于是相等比较与文本形式都是唯一的。1.6 倍存成 8/5 而不是浮点的 1.6，
     * 落盘与比较因此不经过浮点。其余种类不用这两个字段。
     */
    int32_t num = 0;
    int32_t den = 1;

    static Length abs(Emu e);

    /**
     * 比例。分母为 0 不是可表达的比例，返回自动值（没有数值可表达）—— 不猜一个分母，
     * 也不返回一个用 0 做除法的值。调用方要区分"用户写了 auto"与"分母算错了"，
     * 得自己保证分母非零；文本形式里的 pct:1/0 是被显式拒绝的。
     */
    static Length percent(int32_t num, int32_t den);

    /**
     * 相对字号的倍数。按千分位取整（1.6 倍存成 8/5，1/3 倍只能到 333/1000），
     * 因为存的是整数比而不是浮点。更多位数没有实际用途：倍数来自用户输入。
     */
    static Length em(double multiple);

    /**
     * n 个全角字符位，同样按千分位取整。
     */
    static Length chars(double count);

    static Length automatic();

    bool operator==(const Length& o) const {
        return kind == o.kind && abs_emu == o.abs_emu && num == o.num && den == o.den;
    }
    bool operator!=(const Length& o) const { return !(*this == o); }
};

/**
 * 长度的文本形式：精确、无歧义，1.10 的序列化直接用它。五个分支各有一个前缀，
 * 避免"看起来像数字但含义不同"：
 *   abs:<EMU 整数>      例如 abs:914400（2.54 厘米）
 *   pct:<分子>/<分母>    分母为 1 时省略斜杠，例如 pct:1/2
 *   em:<分子>/<分母>     例如 em:8/5
 *   chars:<分子>/<分母>
 *   auto
 *
 * 为什么不写成 2.54cm 这种可读形式：EMU 换算成磅常常是无限小数（12700 含因子 127），
 * 有限位数会丢精度，而这一层要的是 parse(print(x)) == x。给用户看的显示形式（厘米两位
 * 小数）是视图层的事，不在这里。
 */
std::string printLength(const Length& len);

/**
 * 文本形式 → 长度。任何不符合上面语法的输入都返回 false，不猜、不截断、不夹取 ——
 * 静默纠正会把"文件坏了"变成"读出来一个别的值"。
 */
bool parseLength(const std::string& text, Length* out);

}  // namespace heditor
