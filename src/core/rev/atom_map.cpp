#include "core/rev/atom_map.h"

#include <algorithm>

#include "core/rev/length.h"
#include "core/utils/hash.h"

namespace heditor {

namespace {

/**
 * 绝对长度 → 磅。Auto 与非绝对都返回 false，由调用方兜。
 */
bool absPt(const Length& len, float* out) {
    if (len.kind != LengthKind::Abs) return false;
    *out = static_cast<float>(emuToPt(len.abs_emu));
    return true;
}

}  // namespace

uint64_t atomContentHash(const ImageSpec& image) {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, 1);
    h = fnvMixCStr(h, image.resource_id.c_str());
    return h;
}

uint64_t atomContentHash(const FormulaSpec& formula) {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, 2);
    h = fnvMixCStr(h, formula.expr.c_str());
    h = fnvMix(h, static_cast<uint64_t>(formula.backend));
    return h;
}

const char* atomRelativeDiag(bool width) {
    return width ? "原子的显示宽度是相对长度，这一层算不出参照值，已按原始尺寸折算"
                 : "原子的显示高度是相对长度，这一层算不出参照值，已按原始尺寸折算";
}

AtomBox resolveAtomBox(const AtomGeometry& g, float block_size_pt) {
    AtomBox box;
    const float dpi = (g.is_image && g.dpi > 0) ? static_cast<float>(g.dpi) : 96.0f;
    const float px_w = g.is_image ? static_cast<float>(g.px_w) : 0.0f;
    const float px_h = g.is_image ? static_cast<float>(g.px_h) : 0.0f;
    const bool has_px = (px_w > 0.0f && px_h > 0.0f);
    const float nat_w = has_px ? px_w * 72.0f / dpi : 0.0f;
    const float nat_h = has_px ? px_h * 72.0f / dpi : 0.0f;

    // 默认框：图片给一个 4:3 的空框（尺寸本该来自像素或作者，走到这儿说明两者都缺），
    // 公式按表达式的码点数估宽、高取一行。估的是框，不是公式的排版结果 ——
    // 本期不做伪渲染（见 formula_spec.h）。
    float def_w = 0.0f;
    float def_h = 0.0f;
    if (g.is_image) {
        def_w = 4.0f * block_size_pt * 4.0f;
        def_h = 4.0f * block_size_pt * 3.0f;
    } else {
        def_w = 2.0f * block_size_pt + static_cast<float>(g.expr_cp) * 0.5f * block_size_pt;
        def_w = std::min(std::max(def_w, 4.0f * block_size_pt), 40.0f * block_size_pt);
        def_h = block_size_pt * 2.0f;
    }

    float w = 0.0f;
    float h = 0.0f;
    const bool w_abs = absPt(g.width, &w);
    const bool h_abs = absPt(g.height, &h);
    box.width_relative = (g.width.kind != LengthKind::Auto) && !w_abs;
    box.height_relative = (g.height.kind != LengthKind::Auto) && !h_abs;
    if (w_abs && !h_abs) {
        h = has_px ? w * (nat_h / nat_w) : def_h;
    } else if (!w_abs && h_abs) {
        w = has_px ? h * (nat_w / nat_h) : def_w;
    } else if (!w_abs && !h_abs) {
        w = has_px ? nat_w : def_w;
        h = has_px ? nat_h : def_h;
    }
    box.w_pt = (w > 0.0f) ? w : def_w;
    box.h_pt = (h > 0.0f) ? h : def_h;
    return box;
}

}  // namespace heditor
