#include "core/text/shaper.h"

#include <hb-ot.h>
#include <hb.h>

#include <cstdlib>

#include "core/doc/document.h"
#include "core/text/writing_mode.h"

namespace ced {
namespace {

hb_direction_t toHbDirection(ShapingDirection d) {
    return d == ShapingDirection::TopToBottom ? HB_DIRECTION_TTB : HB_DIRECTION_LTR;
}

}  // namespace

ShapedText shapeText(hb_font_t* font, const uint32_t* cps, size_t count, const WritingMode& wm,
                     const char* language) {
    return shapeTextRange(font, cps, 0, count, wm, language);
}

ShapedText shapeTextRange(hb_font_t* font, const uint32_t* all, size_t begin, size_t end,
                          const WritingMode& wm, const char* language) {
    ShapedText out;
    if (!font || !all || end <= begin) return out;
    const size_t count = end - begin;

    out.vertical = (wm.shapingDirection() == ShapingDirection::TopToBottom);
    out.sideways = wm.isSideways();

    hb_buffer_t* buf = hb_buffer_create();
    // add_utf32 的 (item_offset, item_length)：HarfBuzz 用 item_offset + index
    //   作为 cluster，所以直接传整串指针 + 区间，cluster 就是绝对码点下标。
    //   调用方不需要任何 "+=" 补偏移的算术。
    hb_buffer_add_utf32(buf, all, static_cast<int>(end), static_cast<int>(begin),
                        static_cast<int>(count));
    hb_buffer_set_direction(buf, toHbDirection(wm.shapingDirection()));
    if (language) hb_buffer_set_language(buf, hb_language_from_string(language, -1));
    // 让 HarfBuzz 从文本推断 script（蒙文 → MONGOLIAN，中文 → HAN）。
    // HarfBuzz 会据此选择 shaper：Mongolian 走 Arabic 模型。
    hb_buffer_guess_segment_properties(buf);

    // 竖排 feature（vert / vrt2 / vkrn / vpal）由 HarfBuzz 在 TTB 方向下自动启用
    // （见 hb-ot-shape.cc 的 vertical_features）。这里不重复指定，保持"列序归布局层"的边界。
    //
    // 但可选连字必须显式关掉。这一行不是排版偏好，它决定删除语义。
    //
    //   默认整形会开启 liga（标准连字）/ clig（上下文连字）/ dlig（自由连字），
    //   于是拉丁文 "ff" 被合成为一个连字字形，而且 HarfBuzz 把这两个码点的
    //   cluster 都归到第一个 f 上 —— 后果是视觉上一个 ff 连字看着像少打了一个
    //   字符，编辑上第二个 f 的起点不存在（簇表由 cluster 生成），一次退格会删掉
    //   两个 f。该缺陷只在 f / fi / fl / ffi 这类有连字的组合上出现，全中文的
    //   测试盖不到。
    //
    //   不能因此把"cluster 合并"整体禁掉，那会同时破坏字素簇：e + U+0301 同样是
    //   两个码点一个 cluster，但那里必须一次删两个，否则会留下孤立的组合重音。
    //   区别在于字素簇（grapheme cluster）是 Unicode 定义的不可分割单位、该合并，
    //   而 liga / clig / dlig 是可选的字形替换、不改变字符计数，该拆开。
    //
    //   rlig（required ligature）不在关闭之列：阿拉伯的 lam-alef、天城文的
    //   conjunct、蒙古文的必需连写都靠它。关掉 rlig 不会让编辑变得更精确，
    //   只会让这些文字的字形直接错掉 —— 那是拿正确性换一个不存在的收益。
    static const hb_feature_t kEditorFeatures[] = {
        {HB_TAG('l', 'i', 'g', 'a'), 0u, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END},
        {HB_TAG('c', 'l', 'i', 'g'), 0u, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END},
        {HB_TAG('d', 'l', 'i', 'g'), 0u, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END},
    };
    hb_shape(font, buf, kEditorFeatures,
             static_cast<unsigned>(sizeof(kEditorFeatures) / sizeof(kEditorFeatures[0])));

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);

    out.glyphs.reserve(n);
    int32_t inline_advance = 0;

    for (unsigned i = 0; i < n; ++i) {
        ShapedGlyph g;
        g.glyph_id = info[i].codepoint;
        g.x_advance = pos[i].x_advance;
        g.y_advance = pos[i].y_advance;
        g.x_offset = pos[i].x_offset;
        g.y_offset = pos[i].y_offset;
        g.cluster = info[i].cluster;
        out.glyphs.push_back(g);

        // 竖排约定（已从 HarfBuzz 源码核实）：
        //   hb-ot-font.cc 的 hb_ot_get_glyph_v_advances() 里显式对 vmtx 取负：
        //       font->em_scale_y (- (int) vmtx.get_advance_without_var_unscaled (...))
        //   即 TTB 方向下 y_advance 为负值。
        //   本实现的设备坐标系 y 向下，所以"向下推进"的步长 = -y_advance。
        if (out.vertical) {
            inline_advance += -g.y_advance;
        } else {
            inline_advance += g.x_advance;
        }
    }

    out.inline_advance = inline_advance;
    hb_buffer_destroy(buf);
    return out;
}

ShapedText shapeText(hb_font_t* font, std::string_view utf8, const WritingMode& wm,
                     const char* language) {
    const std::vector<uint32_t> cps = utf8ToCodepoints(std::string(utf8));
    return shapeTextRange(font, cps.data(), 0, cps.size(), wm, language);
}

}  // namespace ced
