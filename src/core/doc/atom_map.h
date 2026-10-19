/**
 * 行内原子的两件事：内容指纹与盒子几何。
 *
 * 模型层的编译与修订层的投影必须给出同一枚盒子。两边各写一份，"只给宽不给高"这种三岔口
 * 上迟早分叉，而分叉的表现是同一份文档在两条路上得到不同的行高 —— 判据只有"逐字段比较"
 * 时才看得见，画面上看不出来。
 *
 * 盒子几何与字号有关（公式的默认框按段落字号估），所以这里只算，不决定什么时候算；
 * 调用方在落段时把块级字号传进来，与它落 run 的时机一致。
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/length.h"

namespace heditor {

/**
 * 原子的内容指纹：图片是资源标识，公式是表达式与后端。
 *
 * 尺寸不进它 —— 尺寸改了行盒几何跟着改，行指纹自己就变了。它管的是几何看不见的那一类：
 * 换了一张同样大的图、改了公式表达式。
 */
uint64_t atomContentHash(const ImageSpec& image);
uint64_t atomContentHash(const FormulaSpec& formula);

/**
 * 盒子几何的输入。两个模型都能填出来：模型层从 Node::image / Node::formula，
 * 修订层从载荷。expr_cp 只在公式上有意义（默认框按表达式的码点数估宽）。
 */
struct AtomGeometry {
    Length width;
    Length height;
    int px_w = 0;
    int px_h = 0;
    int dpi = 96;
    std::size_t expr_cp = 0;
    bool is_image = true;
};

/**
 * 盒子几何的结果。两个 *_relative 标记"作者给的是相对长度、这一层算不出参照值"，
 * 由调用方按自己的节点身份记诊断：几何只有一份，日志各记各的。
 */
struct AtomBox {
    float w_pt = 0.0f;
    float h_pt = 0.0f;
    bool width_relative = false;
    bool height_relative = false;
};

/**
 * 定出原子盒子的显示尺寸（磅）。
 *
 * 优先用作者给的绝对显示尺寸；只给一边时按原始宽高比推另一边 —— 原图比例是"给了尺寸却
 * 不给另一边"唯一说得通的解释。两边都没有时按原始像素与 DPI 折算（96 DPI 是行业惯例，
 * 见 image_spec.h）。三条都走不通（图片既没有像素也没有显示尺寸）时用默认框 ——
 * 排版必须有一份几何，诊断让调用方知道这不是作者要的结果。
 */
AtomBox resolveAtomBox(const AtomGeometry& g, float block_size_pt);

/**
 * 相对长度那条诊断的文案。两个调用方各记各的节点号，句子只有一份 ——
 * 否则同一份文档在两条路上会给出两种措辞，而按诊断做的比对就没法直接用。
 */
const char* atomRelativeDiag(bool width);

}  // namespace heditor
