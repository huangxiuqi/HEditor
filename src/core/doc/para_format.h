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
#include <string>
#include <vector>

#include "core/doc/length.h"
#include "core/utils/types.h"
#include "core/utils/writing_mode.h"

namespace heditor {
/**
 * 行距规则。与 OOXML 的 w:lineRule 一一对应，也是 Word 段落面板上那三档。
 *
 * 三态的差别只在"行高怎么定"：auto 按单倍行高的倍数；exact 就是那个磅值，内容更高时
 * 裁掉（Word 也是裁，不是溢出）；atLeast 取固定值与内容高的较大者。
 */
enum class LineSpacingRule : uint8_t {
    /**
     * 倍数行距：行高 = 单倍行高 × 倍数。默认档。
     */
    Auto = 0,

    /**
     * 固定值：行高就是给定磅值，内容超出部分裁掉。
     */
    Exact = 1,

    /**
     * 最小值：行高不小于给定磅值，内容更高时跟着长。
     */
    AtLeast = 2,
};

/**
 * 行距规则的字面量。文档 JSON 与宿主载荷共用这一份，读回来也认它。
 * @param r 规则
 * @return auto / exact / atLeast
 */
inline const char* lineSpacingRuleName(LineSpacingRule r) {
    if (r == LineSpacingRule::Exact) return "exact";
    if (r == LineSpacingRule::AtLeast) return "atLeast";
    return "auto";
}

/**
 * 按字面量认行距规则。
 * @param name 字面量
 * @param out  解析结果（认得出时才写）
 * @return true = 认得这个名字
 */
inline bool lineSpacingRuleFromName(const std::string& name, LineSpacingRule* out) {
    if (name == "auto") {
        *out = LineSpacingRule::Auto;
        return true;
    }
    if (name == "exact") {
        *out = LineSpacingRule::Exact;
        return true;
    }
    if (name == "atLeast") {
        *out = LineSpacingRule::AtLeast;
        return true;
    }
    return false;
}

/**
 * 段落格式。只对段落结束标记（ParagraphMark）有意义，其余种类读它没有定义。
 *
 * 默认值取自 BlockSpec 的默认值（12 磅字号、单倍行距、6 磅段后距）：于是"末尾还没有
 * 结束标记的那一段"直接用默认格式编译，不需要再引一层文档默认格式（1.6 的样式表才需要）。
 */
struct ParaFormat {
    /**
     * 主字体（FontLibrary 里的下标）。
     */
    int font_index = 0;

    /**
     * 字号。绝对长度（EMU），默认 12 磅。字号必须是绝对长度 —— 相对字号算不出磅值，
     * 编译期会拒绝非绝对的字号并给诊断。
     */
    Length size = Length::abs(ptToEmu(12.0));

    /**
     * 书写模式（横排 / 竖排与列序）。
     */
    WritingMode wm = WritingMode::HorizontalTB();

    /**
     * 行距规则（auto / exact / atLeast）。值在下面两栏：auto 只看倍数，
     * exact 与 atLeast 只看磅值。默认 auto。
     */
    LineSpacingRule line_rule = LineSpacingRule::Auto;

    /**
     * 倍数行距的倍数（相对单倍行高，即字体的 ascent + descent + lineGap）。
     * 默认 1.0 与 Word 的单倍行距同口径；只在 line_rule 是 auto 时有意义。
     */
    float line_factor = 1.0f;

    /**
     * 固定值 / 最小值的磅值（EMU）。0 视为没设，按 auto 处理。
     * 只在 line_rule 是 exact 或 atLeast 时有意义。
     */
    Length line_exact = Length::abs(0.0);

    /**
     * 段前距。绝对长度（EMU），默认 0 磅。
     *
     * 与段后距各算各的：Word 里相邻两段的 6 磅 + 6 磅是 12 磅，不折叠（CSS 的 margin
     * 会折叠成 6，那是唯一一处不能照抄浏览器的段落属性）。页顶让出的那一段不加
     * —— 分页之后段首的那截空白属于页边距，不属于段落。
     */
    Length space_before = Length::abs(ptToEmu(0.0));

    /**
     * 段后距。绝对长度（EMU），默认 6 磅（与 BlockSpec 同一档，比 10 磅紧凑一档）。
     */
    Length space_after = Length::abs(ptToEmu(6.0));

    /**
     * 这一段之前强制分页。
     */
    bool page_break_before = false;

    /**
     * 语言标签（BCP 47，例如 zh / mn）。指向驻留字符串（interned），静态生存期，
     * 与 HarfBuzz 的 hb_language_t 是同一个约定；nullptr 表示不指定。
     * 为什么不拿 std::string 存：编译产物里的语言标签要能安全地活过这一次编译，
     * 而 BlockSpec 用的是裸指针（见 layout.h），驻留字符串是两边都能接受的形状。
     */
    const char* language = nullptr;

    /**
     * 默认字色。
     */
    Color color = Color::rgb(24, 24, 24);

    /**
     * 字体回退链（在主字体之后依次尝试）。
     */
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
    /**
     * 覆盖主字体；没设置表示继承。
     */
    std::optional<int> font_index;

    /**
     * 覆盖字号（必须是绝对长度）。
     */
    std::optional<Length> size;

    /**
     * 覆盖书写模式。
     */
    std::optional<WritingMode> wm;

    /**
     * 覆盖行距规则。
     */
    std::optional<LineSpacingRule> line_rule;

    /**
     * 覆盖行距倍数（只在 auto 档生效）。
     */
    std::optional<float> line_factor;

    /**
     * 覆盖行距的固定值（exact / atLeast 档，必须是绝对长度）。
     */
    std::optional<Length> line_exact;

    /**
     * 覆盖段前距。
     */
    std::optional<Length> space_before;

    /**
     * 覆盖段后距。
     */
    std::optional<Length> space_after;

    /**
     * 覆盖"段前分页"。
     */
    std::optional<bool> page_break_before;

    /**
     * 覆盖语言标签。外层是 optional、内层才是裸指针：nullptr 表示"指定为空"，与"这一层没说"是两件事。
     */
    std::optional<const char*> language;

    /**
     * 覆盖字色。
     */
    std::optional<Color> color;

    /**
     * 覆盖回退链。
     */
    std::optional<std::vector<int>> fallback_font_indices;

    /**
     * 一个字段都没设置吗。规范化与诊断靠它判断"这一段有没有内联覆盖"。
     * @return true = 一个字段都没设置（没有内联覆盖）
     */
    bool empty() const {
        return !font_index.has_value() && !size.has_value() && !wm.has_value() &&
               !line_rule.has_value() && !line_factor.has_value() && !line_exact.has_value() &&
               !space_before.has_value() && !space_after.has_value() &&
               !page_break_before.has_value() && !language.has_value() && !color.has_value() &&
               !fallback_font_indices.has_value();
    }

    bool operator==(const ParaFormatPatch& o) const = default;
};

/**
 * 把 patch 里已设置的字段盖到 base 上。base 是上一层（样式链或默认值）算出来的结果。
 * @param patch 覆盖
 * @param base 被覆盖的完整格式（就地改写）
 */
void applyParaFormatPatch(const ParaFormatPatch& patch, ParaFormat* base);

/**
 * 逐字段合并两个 patch：over 里设置了的覆盖 under，没设置的保留 under 的。
 * 样式链的解析就是它按"从根到叶"的顺序叠加。
 * @param under 下层
 * @param over 上层（设置了的覆盖下层）
 * @return 合并结果
 */
ParaFormatPatch mergeParaFormatPatch(const ParaFormatPatch& under, const ParaFormatPatch& over);

/**
 * 驻留一个语言标签，返回静态生存期的指针。
 *
 * ParaFormat 的 language 是裸指针（引擎侧的 BlockSpec 也用它），所以标签必须活过任何一次
 * 编译。从文件读回来时字符串是临时的，只能靠驻留把它钉住 —— 这也是"驻留字符串"这个约定
 * 在序列化（1.10）这一侧的唯一落点。空标签返回 nullptr（表示不指定语言）。
 * @param tag 语言标签
 * @return 静态生存期的指针；空标签为 nullptr
 */
const char* internLanguage(const std::string& tag);

}  // namespace heditor
