/**
 * 长度量化与单位换算。
 *
 * 两件事放在一起，因为它们都是"长度的数值表示"这一件事的两面：
 *   一、量化：1/16 磅是引擎侧的坐标比较步长（指纹、脏页判定、缓存键都只认量化后的值）；
 *   二、EMU 定点表示：模型层存 EMU 整数，引擎层算磅（float），编译时做一次乘除。
 *
 * 内部精度单位选 EMU（1/914400 英寸）的理由见 docs/文档模型设计.md 第十一节：
 * 七个常用单位的 EMU 数全是整数（磅 12700、半磅 6350、缇 635、像素 9525、毫米 36000、
 * 厘米 360000、英寸 914400），所以公制与英制同时精确；而拿厘米做基准时 12 磅 =
 * 0.42333… 无法精确表示。EMU 步长 7.87e-5 磅，也比 auditLayout 的 1e-3 磅容差细 12.7 倍。
 *
 * 与 utils 里其他头并列，是因为它被排版与控件两处共用，不属于任何一层。
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/utils/types.h"

namespace heditor {
/**
 * 把磅坐标量化到 1/16 磅再比较。
 * 不量化就会出问题：浮点末位抖动会让"内容没变"的页也判成脏页，于是每一页都重画，
 * 增量就等于没做。这类 bug 的表象是"看着对，但数字没下来"。
 */
inline int32_t quantPt(float v) { return static_cast<int32_t>(std::lround(v * 16.0f)); }

/**
 * EMU：内部精度单位，1/914400 英寸。模型层存这个整数，引擎层算磅。
 * 为什么必须定点整数：相等比较可靠（增量排版要判"这一行有没有变"，浮点在模型重建后
 * 末位可能不同）、序列化无歧义（整数直接写）、跨平台一致（不依赖浮点单元行为）。
 * 代价是数值比磅大 12700 倍、直接读不直观，所以任何手写的 EMU 字面量都算错误 ——
 * 走下面的换算函数，例如 ptToEmu(12.0) 而不是 152400。
 */
using Emu = int64_t;

/**
 * 长度单位。七个都在第十一节列出，各自都有去处：磅是字号 / 行距 / 线宽，
 * 半磅是 OOXML 的 w:sz（字号单位），缇是 OOXML 的 w:twip（导入时用），
 * 像素是图片的原始尺寸，毫米与厘米是页边距与纸张，英寸是纸张。
 */
enum class Unit : uint8_t { Pt, HalfPt, Twip, Px, Mm, Cm, Inch };

/**
 * 一个单位等于多少 EMU。这七个数全是整数，这就是选 EMU 的理由。
 */
inline constexpr Emu emuPerUnit(Unit u) {
    switch (u) {
        case Unit::Pt:
            return 12700;
        case Unit::HalfPt:
            return 6350;
        case Unit::Twip:
            return 635;
        case Unit::Px:
            return 9525;
        case Unit::Mm:
            return 36000;
        case Unit::Cm:
            return 360000;
        case Unit::Inch:
            return 914400;
    }
    return 0;
}

/**
 * 数值 → EMU，四舍五入（半数远离零）。不用 std::llround 是因为它不能在常量表达式里求值，
 * 而段落格式的默认值要在编译期定下来。
 */
inline constexpr Emu toEmu(double v, Unit u) {
    const double scaled = v * static_cast<double>(emuPerUnit(u));
    return static_cast<Emu>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
}

/**
 * EMU → 数值。除法在 double 里做：EMU 与单位因子都在 2^53 以内，商是正确舍入的结果。
 */
inline double fromEmu(Emu e, Unit u) {
    return static_cast<double>(e) / static_cast<double>(emuPerUnit(u));
}

/**
 * 常用单位的具名包装。为什么不叫 pt() / cm() 这样的短名：现有代码里已有同名局部变量
 * （layout.cpp 的 float pt、engine_diag.cpp 的 const Rect px、native_main.cpp 的
 * long long mm），裸名会被作用域里的局部变量静默遮蔽，所以一律带方向后缀。
 * 其余单位用通用的 toEmu(v, Unit::X) —— 它是上面这几个的唯一实现，这几个只是转发。
 */
inline constexpr Emu ptToEmu(double v) { return toEmu(v, Unit::Pt); }
inline constexpr Emu cmToEmu(double v) { return toEmu(v, Unit::Cm); }
inline constexpr Emu mmToEmu(double v) { return toEmu(v, Unit::Mm); }

/**
 * 字号：OOXML 的 w:sz 以半磅为单位（小四 = 24、五号 = 21），是整数乘法、无舍入。
 */
inline constexpr Emu halfPtToEmu(int half_points) {
    return static_cast<Emu>(half_points) * emuPerUnit(Unit::HalfPt);
}

inline double emuToPt(Emu e) { return fromEmu(e, Unit::Pt); }
inline double emuToCm(Emu e) { return fromEmu(e, Unit::Cm); }
inline double emuToMm(Emu e) { return fromEmu(e, Unit::Mm); }

/**
 * 页内磅矩形 → 页内像素矩形，呈现层的最后一跳。
 *
 * 取整方向固定为左上 floor、右下 ceil，使像素矩形完整覆盖磅矩形 —— 少半像素就是把内容
 * 切掉一条边。宽高为零时的处置有两档，由调用方按语义选，因为它们是两个不同的保证：
 * 光标必须留下至少一个像素（看不见的光标与没有光标在界面上无法区分），选区矩形画不出来
 * 就不该登记（零面积的反白条会让上层白贴一次图）。
 *
 * @param r           页内磅矩形
 * @param density     设备像素 / 磅
 * @param keep_min_px 为真时宽高保底一个像素；为假时宽高不为正即视为不可画
 * @param out         结果，仅在返回 true 时写入
 * @return 是否产出一个可画的矩形
 */
inline bool rectPtToPx(const Rect& r, float density, bool keep_min_px, Rect* out) {
    // 取整落在两条边上，不是"角点 + 宽高"各取一次：后者的右边界是
    //   floor(x*d) + ceil(w*d)，可能小于 ceil((x+w)*d)，右边因此被切掉一小条。
    const float x0 = std::floor(r.x * density);
    const float y0 = std::floor(r.y * density);
    Rect px{};
    px.x = x0;
    px.y = y0;
    px.w = std::ceil((r.x + r.w) * density) - x0;
    px.h = std::ceil((r.y + r.h) * density) - y0;
    if (keep_min_px) {
        px.w = std::max(1.0f, px.w);
        px.h = std::max(1.0f, px.h);
    } else if (!(px.w > 0.0f && px.h > 0.0f)) {
        return false;
    }
    *out = px;
    return true;
}

}  // namespace heditor
