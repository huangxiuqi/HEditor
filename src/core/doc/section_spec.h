/**
 * 节的记录与页面设置（见 docs/文档模型设计.md 第十节）。
 *
 * 节是唯一会让页度量在文档中途变化的机制：纸张、四边页边距、页眉页脚、页码重编、分栏、
 * 文字方向都能按节变化。模型对页面设置的态度是完整保留、按需理解 —— 引擎真正理解的是
 * 纸张名称与方向、四边页边距、页眉页脚、页码这几项；其余（装订线、镜像页边距、分栏、
 * 垂直对齐、页边框、行号、脚注尾注、文档网格）只原样存住、不参与排版。这条边界对导入
 * 外院文档尤其要紧：不认识的部分不能丢，否则打开再保存就会损坏文档。
 *
 * 纸张用名称加方向表示，尺寸是派生值，只有自定义纸张才存显式宽高。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/doc/length.h"
#include "core/doc/node_id.h"
#include "core/text/writing_mode.h"

namespace heditor {

/**
 * 纸张名称。尺寸由名称与方向推出来（见 paperSize），所以名称与方向是存下来的那份，
 * 宽高是算出来的那份。内置几种常用规格，其余走自定义。
 */
enum class PaperName : uint8_t { A4 = 0, A5 = 1, B5 = 2, Letter = 3, Legal = 4, Custom = 5 };

const char* paperNameName(PaperName name);

bool paperNameFromName(const std::string& text, PaperName* out);

enum class Orientation : uint8_t { Portrait = 0, Landscape = 1 };

const char* orientationName(Orientation o);

bool orientationFromName(const std::string& text, Orientation* out);

/**
 * 纸张。name 为 Custom 时宽高取自 width 与 height（必须是绝对长度），
 * 其余名称下宽高由规格推出、这两个字段不参与。
 */
struct Paper {
    PaperName name = PaperName::A4;
    Orientation orient = Orientation::Portrait;
    Length width;
    Length height;

    bool operator==(const Paper& o) const = default;
};

/**
 * 纸张尺寸，EMU。自定义纸张的宽高必须都是绝对长度，否则返回零尺寸 —— 没有参照的长度推不出
 * EMU，不如给一个可识别的空值让调用方处理，而不是猜一个。
 */
struct PaperSizeEmu {
    Emu width = 0;
    Emu height = 0;

    bool operator==(const PaperSizeEmu& o) const = default;
};

PaperSizeEmu paperSize(const Paper& paper);

/**
 * 四边页边距。必须四边独立：病历排版常见上窄下宽、左右不同，单一的一对数字撑不住。
 * 默认与引擎当前的默认边距一致（2 厘米），于是没有写节属性的文档排版结果不变。
 */
struct SectionMargins {
    Length top = Length::abs(cmToEmu(2.0));
    Length bottom = Length::abs(cmToEmu(2.0));
    Length left = Length::abs(cmToEmu(2.0));
    Length right = Length::abs(cmToEmu(2.0));

    bool operator==(const SectionMargins& o) const = default;
};

/**
 * 装订线。本期只原样存住，不参与排版。
 */
struct Gutter {
    enum class Side : uint8_t { Left = 0, Top = 1 };

    Length size;
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

const char* sectionBreakName(SectionBreak b);

bool sectionBreakFromName(const std::string& text, SectionBreak* out);

/**
 * 内容在页面上的垂直分布。本期只原样存住，不参与排版。
 */
enum class VerticalAlign : uint8_t { Top = 0, Center = 1, Bottom = 2, Both = 3 };

const char* verticalAlignName(VerticalAlign a);

bool verticalAlignFromName(const std::string& text, VerticalAlign* out);

enum class PageNumberFormat : uint8_t {
    Arabic = 0,
    RomanUpper = 1,
    RomanLower = 2,
    Chinese = 3,
};

const char* pageNumberFormatName(PageNumberFormat f);

bool pageNumberFormatFromName(const std::string& text, PageNumberFormat* out);

/**
 * 页码：格式、起始值、本节是否重新编号。restart 为真时本节从 start 重新起算，
 * 否则接着上一节累加。
 */
struct PageNumbering {
    PageNumberFormat format = PageNumberFormat::Arabic;
    int start = 1;
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
    NodeId root = kInvalidNodeId;
    Length distance;
    bool linked_to_previous = true;

    bool empty() const { return root == kInvalidNodeId; }

    bool operator==(const HeaderFooter& o) const = default;
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
    Paper paper;
    SectionMargins margins;
    Gutter gutter;
    bool mirror_margins = false;
    SectionBreak break_kind = SectionBreak::Continuous;
    VerticalAlign valign = VerticalAlign::Top;
    WritingMode writing_mode = WritingMode::HorizontalTB();
    PageNumbering numbering;
    HeaderFooter headers[kHeaderFooterVariantCount];
    HeaderFooter footers[kHeaderFooterVariantCount];
    std::map<std::string, std::string> extra;

    HeaderFooter& header(HfVariant v) { return headers[static_cast<int>(v)]; }
    const HeaderFooter& header(HfVariant v) const { return headers[static_cast<int>(v)]; }
    HeaderFooter& footer(HfVariant v) { return footers[static_cast<int>(v)]; }
    const HeaderFooter& footer(HfVariant v) const { return footers[static_cast<int>(v)]; }

    /**
     * 逐字段比较，含属性袋与书写模式的三个分量。给的是这张表的名字，不是按值相等的替身：
     * 属性袋里多一个键、书写模式差一个分量，都要能比出来。
     */
    bool sameAs(const SectionSpec& o) const;
};

}  // namespace heditor
