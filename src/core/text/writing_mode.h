/**
 * 书写轴模型。
 * 这是整个引擎里最容易被写死、也最难回补的一层，所以从第一天就把它做成显式模型：
 * 行内推进轴  inline axis   —— 一行文字沿哪条轴排下去（横 / 竖）
 * 块推进方向  block flow    —— 一行/一列排完之后，下一行/列往哪边走
 * 字形朝向    orientation   —— 竖排时字形是否保持正立
 * 三者正交。把"横排 LTR"硬编码进布局核心，后面加蒙文就是重写布局核心；
 * 先把这三维显式建出来，加 vertical-lr 就只是"多一条取值"。
 * 关键事实：蒙文是竖排、但列序从左往右（vertical-lr），且不保持正立
 * —— 横排成型后整段顺时针转 90°（Unicode 标准：蒙文码位编码在
 * "竖排形态逆时针转 90°"的朝向上，所以还原回竖排要顺时针转）。
 * 这跟中日竖排（列序从右往左、字形正立）正好相反。
 */
#pragma once

namespace ced {

enum class InlineAxis {
    Horizontal,
    Vertical,
};

enum class BlockFlow {
    TopToBottom,
    LeftToRight,
    RightToLeft,
};

enum class TextOrientation {
    Mixed,          // 按字符自身属性决定（拉丁数字转正立，CJK 正立）
    Upright,        // 全部正立（中日竖排）
    SidewaysRight,  // 整段侧倒（蒙文竖排）
};

/**
 * 交给 HarfBuzz 的整形方向。刻意只暴露两个值，不把 hb_direction_t 泄进头文件。
 * HarfBuzz 只负责"沿一条基线推进"的整形与竖排 feature（vert/vrt2/vkrn/vpal）；
 * 列序方向由本实现自己的布局层决定。见 HarfBuzz 源码 hb-ot-shape.cc:
 * "Layout engines are responsible for separating vertical/horizontal runs."
 * 所以 vertical-lr 与 vertical-rl 在 HarfBuzz 侧是同一个方向（TTB），差别全在布局层。
 */
enum class ShapingDirection {
    LeftToRight,
    TopToBottom,
};

struct WritingMode {
    InlineAxis inline_axis = InlineAxis::Horizontal;
    BlockFlow block_flow = BlockFlow::TopToBottom;
    TextOrientation orientation = TextOrientation::Mixed;

    bool isVertical() const { return inline_axis == InlineAxis::Vertical; }

    /**
     * 列序是否反向（中日竖排为 true：第一列在最右）
     */
    bool isBlockReversed() const { return block_flow == BlockFlow::RightToLeft; }

    /**
     * 字形是否由本实现旋转（而不是交给 HarfBuzz 的竖排变体字形）
     */
    bool isSideways() const {
        return isVertical() && orientation == TextOrientation::SidewaysRight;
    }

    /**
     * 交给 HarfBuzz 的整形方向。
     *
     * 侧倒（SidewaysRight，蒙文）必须按横排整形，不能走 TTB：走 TTB 时 hb 会启用
     * vert/vrt2 换成字体里的竖排变体并改用 vmtx 的 advance，而 NotoSansMongolian 的
     * vmtx 对每个字形都给 1 em（字形本体仅 0.734 em），于是词内空隙、整词散架，
     * 变体取向也不是"整段旋转"。
     * 正确做法：横排整形取得连写形态与 hmtx 推进，再由布局/渲染层把每个字形顺时针
     * 旋转 90°（PlacedGlyph::rotate_deg）；与 CSS 的 vertical-lr 加 sideways-rl 同路。
     */
    ShapingDirection shapingDirection() const {
        return (isVertical() && !isSideways()) ? ShapingDirection::TopToBottom
                                               : ShapingDirection::LeftToRight;
    }

    /**
     * 布局层要施加的顺时针旋转角（度）。侧倒 = 90°，其余 = 0°。
     */
    float glyphRotationDeg() const { return isSideways() ? 90.0f : 0.0f; }

    const char* name() const {
        if (!isVertical()) return "horizontal-tb";
        return isBlockReversed() ? "vertical-rl" : "vertical-lr";
    }

    /**
     * 中文/英文横排
     */
    static WritingMode HorizontalTB() {
        return WritingMode{InlineAxis::Horizontal, BlockFlow::TopToBottom, TextOrientation::Mixed};
    }

    /**
     * 中日竖排：列序从右往左，字形正立
     */
    static WritingMode VerticalRL() {
        return WritingMode{InlineAxis::Vertical, BlockFlow::RightToLeft, TextOrientation::Upright};
    }

    /**
     * 蒙文竖排：列序从左往右，字形侧倒
     */
    static WritingMode VerticalLR() {
        return WritingMode{InlineAxis::Vertical, BlockFlow::LeftToRight,
                           TextOrientation::SidewaysRight};
    }
};

}  // namespace ced
