/**
 * 段落格式：段落级属性的集合，挂在段落结束标记上（第三节：段落属性的唯一宿主）。
 *
 * 0.4 时字段形状整体借用 BlockSpec 的块级字段，因为那一步的判据是"编译结果与现有
 * BlockSpec 路径逐字段一致"。0.5 把其中两个长度字段换成模型自己的表示（Length，EMU 定点），
 * 编译时做一次换算 —— 引擎仍然只吃磅值，所以那条判据依然成立，而且它现在顺带证明了
 * 长度换算的精确性。其余字段（字体下标、语言标签、行距倍数）要等 1.6 的样式表。
 *
 * 为什么不另建一张"段落格式表"：编译要按流顺序取它，而流顺序就是树的顺序，
 * 与 Node.hidden 同理（挂在节点上，树一改自然跟着变）。
 */
#pragma once

#include <vector>

#include "core/doc/length.h"
#include "core/text/writing_mode.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 段落格式。只对段落结束标记（ParagraphMark）有意义，其余种类读它没有定义。
 *
 * 默认值取自 BlockSpec 的默认值（12 磅字号、1.6 倍行距、10 磅段后距）：于是"末尾还没有
 * 结束标记的那一段"直接用默认格式编译，不需要再引一层文档默认格式（1.6 的样式表才需要）。
 */
struct ParaFormat {
    int font_index = 0;

    /**
     * 字号。绝对长度（EMU），默认 12 磅。字号必须是绝对长度 —— 相对字号算不出磅值，
     * 编译期会拒绝非绝对的字号并给诊断。
     */
    Length size = Length::abs(ptToEmu(12.0));

    WritingMode wm = WritingMode::HorizontalTB();

    /**
     * 行距倍数。为什么还不是 Length：它是"相对字号"的比值，而第十一节的行距三态
     * （auto / exact / atLeast）要改引擎的块轴推进公式，属于独立的一步。
     * 这里先保持 BlockSpec 的浮点倍数，也就是三态里的 auto 这一态。
     */
    float line_factor = 1.6f;

    /**
     * 段后距。绝对长度（EMU），默认 10 磅。
     */
    Length space_after = Length::abs(ptToEmu(10.0));

    bool page_break_before = false;

    /**
     * 语言标签（BCP 47，例如 zh / mn）。指向驻留字符串（interned），静态生存期，
     * 与 HarfBuzz 的 hb_language_t 是同一个约定；nullptr 表示不指定。
     * 为什么不拿 std::string 存：编译产物里的语言标签要能安全地活过这一次编译，
     * 而 BlockSpec 用的是裸指针（见 layout.h），驻留字符串是两边都能接受的形状。
     */
    const char* language = nullptr;

    Color color = Color::rgb(24, 24, 24);
    std::vector<int> fallback_font_indices;
};

}  // namespace heditor
