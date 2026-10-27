/**
 * 控件几何的实现。
 *
 * 全部工作是把"控件区间（块号 + 码点）"翻译成"页内矩形（磅）"：唯一的依据是排版结果里
 * 的簇位置。这里不查文档、不看模式、不看缩放 —— 也就不会被它们其中的任何一个改变口径。
 */
#include "core/engine/control_geometry.h"

#include <cstddef>
#include <cstdint>

namespace heditor {

void ControlGeometry::bind(const ControlSet* controls, const LayoutResult* layout,
                           const std::unordered_map<int, int>* owner_index) {
    controls_ = controls;
    layout_ = layout;
    owner_index_ = owner_index;
}

int ControlGeometry::indexOfOwner(int owner) const {
    if (owner_index_ == nullptr) return -1;
    const auto it = owner_index_->find(owner);
    return it == owner_index_->end() ? -1 : it->second;
}

std::vector<PageRect> ControlGeometry::rects(int index) const {
    std::vector<PageRect> out;
    if (controls_ == nullptr || layout_ == nullptr) return out;
    if (index < 0 || index >= controls_->size()) return out;
    const ControlRange& cr = controls_->at(index).range;

    for (const LineBox& lb : layout_->lines) {
        if (lb.paragraph < cr.start.para || lb.paragraph > cr.end.para) continue;

        // 这一行里被区间覆盖的码点范围 [lo, hi)
        uint32_t lo = lb.first_cp;
        uint32_t hi = lb.last_cp;
        if (lb.paragraph == cr.start.para && cr.start.offset > lo)
            lo = static_cast<uint32_t>(cr.start.offset);
        if (lb.paragraph == cr.end.para && cr.end.offset < hi)
            hi = static_cast<uint32_t>(cr.end.offset);
        // 没有码点的空控件：几何只剩提示槽那一段（见头文件）。
        if (lo >= hi) {
            for (size_t i = 0; i < lb.clusters.size(); ++i) {
                const LineCluster& c = lb.clusters[i];
                if (!c.control_hint || indexOfOwner(c.owner_control) != index) continue;
                const float h0 = lb.inline_origin_pt + c.inline_pt;
                const float h1 = (i + 1 < lb.clusters.size())
                                     ? lb.inline_origin_pt + lb.clusters[i + 1].inline_pt
                                     : lb.inline_origin_pt + lb.inline_end_pt;
                if (!(h1 > h0)) continue;
                Rect hr;
                if (!lb.vertical) {
                    hr = Rect{h0, lb.block_lo_pt, h1 - h0, lb.block_hi_pt - lb.block_lo_pt};
                } else {
                    hr = Rect{lb.block_lo_pt, h0, lb.block_hi_pt - lb.block_lo_pt, h1 - h0};
                }
                out.push_back(PageRect{lb.page, hr});
                break;
            }
            continue;
        }

        // 左右边缘不能用同一个函数取，这是最容易写错的地方：
        //   左边缘取"lo 那个簇自己的起点"（对）；
        //   右边缘若也取"hi 那个簇的起点"，拿到的是区间内最后一个字的左边缘，
        //   会整整少掉一个字的宽度（症状：框比字窄一格，看着像没框住）。
        //   所以右边缘取"最后一个 cp < hi 的簇的右边缘"。
        // 跳过占位簇：括号在内容之外（[内容]），控件矩形只该框住内容。
        //   不跳的话矩形会把前缀的括号宽度也算进来，左边缘会左移一个括号 ——
        //   于是"点在字段矩形中心"这类断言会差几像素，而画面看着又是对的。
        float left = lb.inline_end_pt;
        for (const LineCluster& c : lb.clusters) {
            if (c.cp >= lo && !c.placeholder) {
                left = c.inline_pt;
                break;
            }
        }
        float right = left;
        for (size_t i = 0; i < lb.clusters.size(); ++i) {
            if (lb.clusters[i].cp >= hi) break;
            right = (i + 1 < lb.clusters.size()) ? lb.clusters[i + 1].inline_pt : lb.inline_end_pt;
        }
        if (!(right > left)) continue;

        const float a0 = lb.inline_origin_pt + left;
        const float a1 = lb.inline_origin_pt + right;
        Rect r;
        if (!lb.vertical) {
            r.x = a0;
            r.y = lb.block_lo_pt;
            r.w = a1 - a0;
            r.h = lb.block_hi_pt - lb.block_lo_pt;
        } else {
            r.x = lb.block_lo_pt;
            r.y = a0;
            r.w = lb.block_hi_pt - lb.block_lo_pt;
            r.h = a1 - a0;
        }
        out.push_back(PageRect{lb.page, r});
    }
    return out;
}

}  // namespace heditor
