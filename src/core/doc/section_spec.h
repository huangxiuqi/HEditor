/**
 * 节的记录与页面设置（见 docs/文档模型设计.md 第十节）。
 *
 * 节是唯一会让页度量在文档中途变化的机制：纸张、四边页边距、页眉页脚、页码重编、分栏、
 * 文字方向都能按节变化。模型对页面设置的态度是完整保留、按需理解 —— 引擎真正理解的是
 * 纸张宽高与方向、四边页边距、页眉页脚、页码这几项；其余（装订线、镜像页边距、分栏、
 * 垂直对齐、页边框、行号、脚注尾注、文档网格）只原样存住、不参与排版。这条边界对导入
 * 外院文档尤其要紧：不认识的部分不能丢，否则打开再保存就会损坏文档。
 *
 * 纸张只存宽高与方向：规格名（A4 / Letter / A3……）是宿主的概念，引擎按长度排版。
 * 文档里存名字就必须有一张表把名字翻成尺寸，而那张表在宿主那一侧 —— 两份表迟早分家。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/doc/length.h"
#include "core/doc/node_id.h"
#include "core/utils/api_codes.h"
#include "core/utils/types.h"
#include "core/utils/writing_mode.h"

namespace heditor {
enum class Orientation : uint8_t { Portrait = 0, Landscape = 1 };

/**
 * 方向的名字。
 * @param o 方向
 * @return 名字
 */
const char* orientationName(Orientation o);

/**
 * 名字转方向。
 * @param text 名字
 * @param out 写回方向
 * @return true = 名字认识
 */
bool orientationFromName(const std::string& text, Orientation* out);

/**
 * 纸张：宽高与方向。宽度与高度是绝对长度，默认值取 A4 —— 与 PageMetrics 的默认页度量、
 * 宿主那张规格表都来自 codes::kPaperSizes 的第一行。
 *
 * 横向不交换存下来的宽高：方向只改摆放，规格本身不变。
 */
struct Paper {
    /**
     * 方向（纵向 / 横向）。
     */
    Orientation orient = Orientation::Portrait;

    /**
     * 纸宽。
     */
    Length width = Length::abs(ptToEmu(codes::kPaperSizes[codes::kDefaultPaperIndex].w_pt));

    /**
     * 纸高。
     */
    Length height = Length::abs(ptToEmu(codes::kPaperSizes[codes::kDefaultPaperIndex].h_pt));

    bool operator==(const Paper& o) const = default;
};

/**
 * 四边页边距。必须四边独立：病历排版常见上窄下宽、左右不同，单一的一对数字撑不住。
 * 默认与引擎当前的默认边距一致（2 厘米），于是没有写节属性的文档排版结果不变。
 */
struct SectionMargins {
    /**
     * 上边距。
     */
    Length top = Length::abs(cmToEmu(2.0));

    /**
     * 下边距。
     */
    Length bottom = Length::abs(cmToEmu(2.0));

    /**
     * 左边距。
     */
    Length left = Length::abs(cmToEmu(2.0));

    /**
     * 右边距。
     */
    Length right = Length::abs(cmToEmu(2.0));

    bool operator==(const SectionMargins& o) const = default;
};

/**
 * 装订线。本期只原样存住，不参与排版。
 */
struct Gutter {
    enum class Side : uint8_t { Left = 0, Top = 1 };

    /**
     * 装订线宽度。
     */
    Length size;

    /**
     * 装订线在哪一侧。
     */
    Side side = Side::Left;

    bool operator==(const Gutter& o) const = default;
};

/**
 * 节类型：本节相对前一节的分页行为。第一节忽略它，因为它前面没有节。
 *
 * 奇数页与偶数页在 Word 里会补空白页让新节起始于指定奇偶页，模型只能表达"下一节从新页
 * 开始"，补空白页要引擎支持页码奇偶，因此这两档现在与下一页等效。
 */
enum class SectionBreak : uint8_t {
    Continuous = 0,
    NextPage = 1,
    OddPage = 2,
    EvenPage = 3,
};

/**
 * 分节方式的名字。
 * @param b 分节方式
 * @return 名字
 */
const char* sectionBreakName(SectionBreak b);

/**
 * 名字转分节方式。
 * @param text 名字
 * @param out 写回分节方式
 * @return true = 名字认识
 */
bool sectionBreakFromName(const std::string& text, SectionBreak* out);

/**
 * 内容在页面上的垂直分布。本期只原样存住，不参与排版。
 */
enum class VerticalAlign : uint8_t { Top = 0, Center = 1, Bottom = 2, Both = 3 };

/**
 * 垂直分布的名字。
 * @param a 分布
 * @return 名字
 */
const char* verticalAlignName(VerticalAlign a);

/**
 * 名字转垂直分布。
 * @param text 名字
 * @param out 写回分布
 * @return true = 名字认识
 */
bool verticalAlignFromName(const std::string& text, VerticalAlign* out);

enum class PageNumberFormat : uint8_t {
    Arabic = 0,
    RomanUpper = 1,
    RomanLower = 2,
    Chinese = 3,
};

/**
 * 页码格式的名字。
 * @param f 格式
 * @return 名字
 */
const char* pageNumberFormatName(PageNumberFormat f);

/**
 * 名字转页码格式。
 * @param text 名字
 * @param out 写回格式
 * @return true = 名字认识
 */
bool pageNumberFormatFromName(const std::string& text, PageNumberFormat* out);

/**
 * 页码：格式、起始值、本节是否重新编号。restart 为真时本节从 start 重新起算，
 * 否则接着上一节累加。
 */
struct PageNumbering {
    /**
     * 页码格式。
     */
    PageNumberFormat format = PageNumberFormat::Arabic;

    /**
     * 起始页码（restart 为真时生效）。
     */
    int start = 1;

    /**
     * 本节是否重新编号。
     */
    bool restart = false;

    bool operator==(const PageNumbering& o) const = default;
};

/**
 * 页眉页脚的三组变体：首页、奇数页、偶数页。对应 Word 的 titlePg 与 evenAndOddHeaders。
 */
enum class HfVariant : uint8_t { First = 0, Odd = 1, Even = 2 };

inline constexpr int kHeaderFooterVariantCount = 3;

/**
 * 一组页眉或页脚。
 *
 * root 是内容片段的根（一个装块级内容的容器），它不在正文流里 —— 页眉页脚有自己的编译
 * 路径，位置换算与段落序号都看不到它。root 为无效值表示这一组没有内容。
 *
 * linked_to_previous 是链接到前一节：默认继承前一节的页眉页脚，显式断链才独立。它是一个
 * 必须存下来的状态，否则改了一节的页眉会莫名其妙影响后面的节。
 *
 * distance 是距页边界的距离（页眉距上边、页脚距下边），与页边距是两回事，页眉可以落在
 * 页边距以内。
 */
struct HeaderFooter {
    /**
     * 内容片段的根（装块级内容的容器）；无效值表示这一组没有内容。
     */
    NodeId root = kInvalidNodeId;

    /**
     * 距页边界的距离（页眉距上边、页脚距下边）。
     */
    Length distance;

    /**
     * 链接到前一节：继承它的页眉页脚。
     */
    bool linked_to_previous = true;

    /**
     * 这一组有没有内容。
     * @return true = 没有内容（root 无效）
     */
    bool empty() const { return root == kInvalidNodeId; }

    bool operator==(const HeaderFooter& o) const = default;
};

/**
 * 水印：整页衬在正文下方的一行半透明文字（与 Word 的"自定义文字水印"同一种形态）。
 *
 * 它挂在节上而不是另立一个文档级记录：水印按节生效是 Word 的口径，而节已经是页面设置的
 * 归属地。引擎只认一行文字加几个外观参数 —— 平铺、图片水印、每节不同水印都要另一套模型，
 * 现在没有，也不假装有：enabled 为假时一个字段都不画。
 */
struct WatermarkSpec {
    /**
     * 画不画。默认不画 —— 没有水印的文档不该因为多了这个字段而变样。
     */
    bool enabled = false;

    /**
     * 水印文字。空串时即使 enabled 为真也不画（没有内容的"水印"只是一个不可见的开关）。
     */
    std::string text;

    /**
     * 字号（磅）。Word 的默认档是 48 磅上下。
     */
    float size_pt = 48.0f;

    /**
     * 颜色与不透明度。alpha 在这里就是"半透明"的唯一表达，不另设 opacity 字段。
     */
    Color color = Color{128, 128, 128, 128};

    /**
     * 倾斜角（度，负值是逆时针）。0 = 水平；Word 的默认斜排是 -45。
     */
    float angle_deg = -45.0f;

    bool operator==(const WatermarkSpec&) const = default;
};

/**
 * 一份节记录。只对 Section 有意义。
 *
 * extra 是属性袋：引擎不认识的页面设置（打印机设置、兼容性标记、主题色）原样放进来、
 * 序列化时原样写出。它的价值是让"打开再保存"不损坏文档，而不是让引擎理解它们。
 *
 * 不提供按值相等的默认实现：书写模式 WritingMode 没有相等比较，也不该为它单加一个。
 * 需要比较时用 sameAs，它逐字段比清楚。
 */
struct SectionSpec {
    /**
     * 纸张与方向。
     */
    Paper paper;

    /**
     * 四边页边距。
     */
    SectionMargins margins;

    /**
     * 装订线（只存住，不参与排版）。
     */
    Gutter gutter;

    /**
     * 镜像页边距（只存住，不参与排版）。
     */
    bool mirror_margins = false;

    /**
     * 本节相对前一节的分页行为。
     */
    SectionBreak break_kind = SectionBreak::Continuous;

    /**
     * 内容在页面上的垂直分布（只存住，不参与排版）。
     */
    VerticalAlign valign = VerticalAlign::Top;

    /**
     * 这一节的书写模式（横排 / 竖排与列序）。
     */
    WritingMode writing_mode = WritingMode::HorizontalTB();

    /**
     * 页码格式与重新编号。
     */
    PageNumbering numbering;

    /**
     * 页眉：首页、奇数页、偶数页三组。
     */
    HeaderFooter headers[kHeaderFooterVariantCount];

    /**
     * 页脚：首页、奇数页、偶数页三组。
     */
    HeaderFooter footers[kHeaderFooterVariantCount];

    /**
     * 属性袋：引擎不认识的页面设置原样存住、原样写出。
     */
    std::map<std::string, std::string> extra;

    /**
     * 水印（衬在正文下方的半透明文字）。
     */
    WatermarkSpec watermark;

    /**
     * 按变体取页眉（可写）。
     * @param v 变体（首页 / 奇数页 / 偶数页）
     * @return 那一组页眉
     */
    HeaderFooter& header(HfVariant v) { return headers[static_cast<int>(v)]; }

    /**
     * 按变体取页眉（只读）。
     * @param v 变体（首页 / 奇数页 / 偶数页）
     * @return 那一组页眉
     */
    const HeaderFooter& header(HfVariant v) const { return headers[static_cast<int>(v)]; }

    /**
     * 按变体取页脚（可写）。
     * @param v 变体（首页 / 奇数页 / 偶数页）
     * @return 那一组页脚
     */
    HeaderFooter& footer(HfVariant v) { return footers[static_cast<int>(v)]; }

    /**
     * 按变体取页脚（只读）。
     * @param v 变体（首页 / 奇数页 / 偶数页）
     * @return 那一组页脚
     */
    const HeaderFooter& footer(HfVariant v) const { return footers[static_cast<int>(v)]; }

    /**
     * 逐字段比较，含属性袋与书写模式的三个分量。给的是这张表的名字，不是按值相等的替身：
     * 属性袋里多一个键、书写模式差一个分量，都要能比出来。
     * @param o 另一份节记录
     * @return true = 逐字段相同（含属性袋与书写模式）
     */
    bool sameAs(const SectionSpec& o) const;
};

}  // namespace heditor
