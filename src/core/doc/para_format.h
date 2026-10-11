/**
 * 段落格式：段落级属性的集合，挂在段落结束标记上（第三节：段落属性的唯一宿主）。
 *
 * 0.4 时字段形状整体借用 BlockSpec 的块级字段，因为那一步的判据是"编译结果与现有
 * BlockSpec 路径逐字段一致"。0.5 把其中两个长度字段换成模型自己的表示（Length，EMU 定点），
 * 编译时做一次换算 —— 引擎仍然只吃磅值，所以那条判据依然成立，而且它现在顺带证明了
 * 长度换算的精确性。
 *
 * 本结构是解析之后的完整形式：节点上挂的是 ParaFormatPatch（只存与样式不同的那些属性），
 * 编译时按"文档默认值 → 样式链 → 内联覆盖"三层解析出它（1.6）。引擎只吃这里的结果，
 * 不认识样式表。
 *
 * 为什么不另建一张"段落格式表"：编译要按流顺序取它，而流顺序就是树的顺序，
 * 与 Node.hidden 同理（挂在节点上，树一改自然跟着变）。
 */
#pragma once

#include <optional>
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

/**
 * 段落格式的"只写了差异"形式：未设置的字段表示继承，设置了的表示覆盖。
 *
 * 元素只存样式引用加与样式不同的那些属性（第六节），所以节点上挂的是本结构而不是完整的
 * ParaFormat —— 完整形式带着全部默认值，一挂上就"处处都是内联覆盖"，样式再也起不了作用。
 * 它与 CharFormat 是同一个口径（三态、随规范化比较），只是字段集是段落级的那一组。
 *
 * 为什么可选的是 language 而不是让裸指针兼表"未设置"：裸指针的 nullptr 在 ParaFormat 里
 * 表示"不指定语言"，那是设置成了空，与"这一层没说"不是一回事。混起来就没法把某个语言
 * 从下层继承的值上取消掉。
 */
struct ParaFormatPatch {
    std::optional<int> font_index;
    std::optional<Length> size;
    std::optional<WritingMode> wm;
    std::optional<float> line_factor;
    std::optional<Length> space_after;
    std::optional<bool> page_break_before;
    std::optional<const char*> language;
    std::optional<Color> color;
    std::optional<std::vector<int>> fallback_font_indices;

    /**
     * 一个字段都没设置吗。规范化与诊断靠它判断"这一段有没有内联覆盖"。
     */
    bool empty() const {
        return !font_index.has_value() && !size.has_value() && !wm.has_value() &&
               !line_factor.has_value() && !space_after.has_value() &&
               !page_break_before.has_value() && !language.has_value() && !color.has_value() &&
               !fallback_font_indices.has_value();
    }

    bool operator==(const ParaFormatPatch& o) const = default;
};

/**
 * 把 patch 里已设置的字段盖到 base 上。base 是上一层（样式链或默认值）算出来的结果。
 */
void applyParaFormatPatch(const ParaFormatPatch& patch, ParaFormat* base);

/**
 * 逐字段合并两个 patch：over 里设置了的覆盖 under，没设置的保留 under 的。
 * 样式链的解析就是它按"从根到叶"的顺序叠加。
 */
ParaFormatPatch mergeParaFormatPatch(const ParaFormatPatch& under, const ParaFormatPatch& over);

}  // namespace heditor
