/**
 * 图片：资源标识、原始像素尺寸、显示尺寸，以及定位与环绕方式。
 *
 * 模型里不出现像素以外的设备相关量（第十一节）：原始像素尺寸加 DPI 是图片文件自己的属性，
 * 与设备无关；显示尺寸是 Length，绝对或相对，由作者给。只给像素不给 DPI 时按 96 DPI 假设，
 * 这是行业惯例。显示尺寸留 Auto 表示"按原始像素与 DPI 折算"，两边都 Auto 时按比例缩放，
 * 于是"只给宽"或"只给高"都能推出来 —— 那正是作者最常给的那一种。
 *
 * 定位方式与环绕方式本期只存不排（第十一节末尾）：浮动的断行核心是"每行可用宽度不再是一个
 * 常数，而随浮动框横向范围变化"，与现有"左右边界是常数"的假设冲突；先留位、后补引擎不产生
 * 返工，它也不该拖住原子进流这一步。
 *
 * 与 para / marks / control 一样挂在节点上、只对 Image 有意义：编译要按流顺序取它，
 * 而流顺序就是树的顺序。
 */
#pragma once

#include <cstdint>
#include <string>

#include "core/doc/length.h"

namespace heditor {
/**
 * 定位方式。本期内联；浮动两档先留位 —— 它改的是断行核心，不是加一个字段。
 */
enum class ImageAnchor : uint8_t {
    Inline = 0,  // 行内：与文字同流，参与断行（本期）
    Float = 1,   // 浮动：脱离流，另起一层（本期不排，见文件头）
};

/**
 * 环绕方式。Inline 下恒为 None；Float 下分四档，与 Word 的"环绕方式"一一对应。
 *
 * 前两档参与断行（文字要给图让位），后两档完全不占位（图叠在文字上或压在文字下）——
 * 这个差别是排版层唯一要分开的一件事。
 */
enum class ImageWrap : uint8_t {
    None = 0,     // 上下型：文字不进入图片的纵向区间
    Square = 1,   // 四周型：文字绕在图片两侧
    Behind = 2,   // 衬于文字下方：不占位，压在文字底下
    InFront = 3,  // 浮于文字上方：不占位，盖在文字上面
};

/**
 * 名字与枚举的互换。落盘只写名字（第六节：格式里的枚举用字符串），
 * 于是加一档不改格式、旧文件读得回来。
 * @param a 浮动方式
 * @return 名字
 */
const char* imageAnchorName(ImageAnchor a);

/**
 * 绕排方式的名字。
 * @param w 绕排方式
 * @return 名字
 */
const char* imageWrapName(ImageWrap w);

/**
 * 名字转浮动方式。
 * @param name 名字
 * @param out 写回方式
 * @return true = 名字认识
 */
bool imageAnchorFromName(const std::string& name, ImageAnchor* out);

/**
 * 名字转绕排方式。
 * @param name 名字
 * @param out 写回方式
 * @return true = 名字认识
 */
bool imageWrapFromName(const std::string& name, ImageWrap* out);

struct ImageSpec {
    /**
     * 资源标识。图片的字节不进文档（第十一节：二进制资源外置，文档只存 ID）。
     * 空表示还没有绑定资源 —— 排版照常按尺寸给它一个占位框，不会静默消失。
     */
    std::string resource_id;

    /**
     * 原始像素尺寸与 DPI。像素为正是不变量（一张没有尺寸的图片不是图片），
     * 显示尺寸的 Auto 档由它们折算。
     */
    int px_w = 0;

    /**
     * 原始像素高。
     */
    int px_h = 0;

    /**
     * 原始像素的 DPI。
     */
    int dpi = 96;

    /**
     * 显示尺寸。Auto 表示按原始像素与 DPI 折算；一边 Auto 时按另一边的比例缩放，
     * 保持原始宽高比。两边的换算都只发生一次，在编译层（见 compile.cpp）。
     */
    Length width;

    /**
     * 显示高度（Auto 表示按比例推）。
     */
    Length height;

    /**
     * 浮动还是随文。
     */
    ImageAnchor anchor = ImageAnchor::Inline;

    /**
     * 文字怎么绕排。
     */
    ImageWrap wrap = ImageWrap::None;

    /**
     * 浮动时的相对偏移（磅）：相对锚点段落那一行的内容区原点。只在 Float 下有意义，
     * 0 表示水平居中、纵向贴着锚点那一行的顶。
     */
    float dx_pt = 0.0f;

    /**
     * 同上，纵向偏移。
     */
    float dy_pt = 0.0f;

    bool operator==(const ImageSpec&) const = default;
};

}  // namespace heditor
