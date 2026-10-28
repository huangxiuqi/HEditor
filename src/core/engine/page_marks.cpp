/**
 * 页位图上的非正文图层：实现见 page_marks.h。
 *
 * 这一族只碰 Bitmap 与 RenderInput，不碰引擎状态 —— 所以它们能脱离引擎单独验：
 * 一块位图、一份输入、一个页号，画出来就该是那张页。
 */
#include "core/engine/page_marks.h"

#include <hb.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/rev/control_spec.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 行内原子的显式占位框（说明见 page_marks.h 的同名声明）。
 *
 * 为什么画框而不是内容：图片的字节还没进来（二进制资源外置是阶段 6 的事），公式的排版
 * 后端本期不做 —— 把表达式按普通文字画出来会得到错误的字距与上下标，那是伪渲染，
 * 比一个明确的框更危险。框里写上它是什么，读者一眼看得出这里是一枚原子。
 */
void drawAtomBox(PageSink& target, const RenderInput& in, const LineBox& lb, const LineCluster& c,
                 float a1_pt, float off_y) {
    const float zoom = in.cfg->zoom;
    const float a0 = lb.inline_origin_pt + c.inline_pt;
    // 底边坐在基线上（与字形同一个原点），高度是原子自己的 —— 不是行盒的：一行里混排
    //   大字号文字时行盒比盒子高，拿行盒画框会画出一个不对的矩形。
    const float axis = lb.bracket_axis_pt;
    const float top = axis - c.atom_h_pt;
    Rect box;
    if (!lb.vertical) {
        box = Rect{a0 * zoom, top * zoom + off_y, (a1_pt - a0) * zoom, c.atom_h_pt * zoom};
    } else {
        box = Rect{top * zoom, a0 * zoom + off_y, c.atom_h_pt * zoom, (a1_pt - a0) * zoom};
    }
    if (!(box.w > 0.0f) || !(box.h > 0.0f)) return;

    const rev::Node* n = rev::findNode((*in.doc), static_cast<NodeId>(c.owner_control));
    const ImageSpec* im = (n != nullptr) ? std::get_if<ImageSpec>(&n->payload) : nullptr;
    // 图片的字节不进文档（第十一节），资源表里没有它时退回占位框 —— 这是常规状态
    //   （文件刚打开、宿主还没注册），不是错误。竖排本期仍画占位框：图片在竖排里要不要
    //   跟着转 90 度是版式决定，按横排的盒子直接铺上去会得到一张被拉长的图。
    const ImageResource* res =
        (im != nullptr && !im->resource_id.empty()) ? in.images->find(im->resource_id) : nullptr;
    if (c.atom == InlineAtom::Image && res != nullptr && !lb.vertical) {
        target.drawImage(imageResourceKey(res->id), res->version, res->rgba.data(), res->width,
                         res->height, box);
        return;
    }

    target.fillRect(box, in.cfg->atom_fill);
    target.strokeRect(box, in.cfg->atom_border, 1.0f);

    hb_font_t* hbf = (lb.bracket_font != nullptr) ? lb.bracket_font->hb() : nullptr;
    if (hbf == nullptr) return;
    // 框里写什么：图片写资源标识，公式写表达式；两者都空就写一个说明性的词 ——
    //   空框看不出是哪一种原子，而"看不出"正是这一层要避免的。
    std::string label;
    if (im != nullptr) {
        label = im->resource_id;
    } else if (n != nullptr && c.atom == InlineAtom::Formula) {
        const auto* f = std::get_if<FormulaSpec>(&n->payload);
        if (f != nullptr) label = f->expr;
    }
    if (label.empty()) label = (c.atom == InlineAtom::Formula) ? "公式" : "图片";

    const float pad = in.cfg->atom_pad_pt;
    const float avail = (a1_pt - a0) - pad * 2.0f;
    if (!(avail > 0.0f)) return;
    const float units_to_px = zoom / kFontScalePerPt;
    // 框内居中。横排下"居中"指基线：基线 = 框顶 + (框高 + ascent - descent) / 2；
    //   竖排下标签转 90°，居中指的是列内横轴，取框的横向中心（与括号同一条做法）。
    const float asc = lb.bracket_font->ascenderPt();
    const float desc = lb.bracket_font->descenderPt();
    const float centered = top + (c.atom_h_pt + asc - desc) * 0.5f;
    const float cross = lb.vertical ? (top + c.atom_h_pt * 0.5f) : centered;
    const float rot = lb.vertical ? 90.0f : 0.0f;

    std::vector<uint32_t> ids;
    std::vector<float> xs;
    std::vector<float> ys;
    float pen = 0.0f;
    for (uint32_t cp : utf8ToCodepoints(label)) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hbf, cp, &g) || g == 0) continue;  // 缺字跳过这一枚
        const float adv = hb_font_get_glyph_h_advance(hbf, g) / kFontScalePerPt;
        // 放不下就截断：溢出的字会盖到相邻内容上，比少写几个字更糟。
        if (pen + adv > avail) break;
        if (!lb.vertical) {
            xs.push_back((a0 + pad + pen) * zoom);
            ys.push_back(cross * zoom + off_y);
        } else {
            xs.push_back(cross * zoom);
            ys.push_back((a0 + pad + pen) * zoom + off_y);
        }
        ids.push_back(g);
        pen += adv;
    }
    if (!ids.empty()) {
        target.drawGlyphs(hbf, ids.data(), xs.data(), ys.data(), static_cast<int>(ids.size()),
                          units_to_px, in.cfg->atom_text, rot);
    }
}

namespace {

/**
 * 换行符标记（形如 ↵）：一横、左端一枚实心箭头、右端向上折一竖。
 *
 * 用矩形与三角形拼，不借字体字形：这三份字体里都没有 U+21B5，箭头也只有中文字体才有
 * 一字不差的 U+2190 —— 借字形会在缺字的那一行静默消失。尺寸按这一行的字号算，于是
 * 缩放与字号一起变。
 * @param target 目标位图
 * @param along 行末位置（磅，行内轴）
 * @param cross 基线（磅，块轴）
 * @param size_pt 这一行的字号（磅）
 * @param zoom 设备像素 / 磅
 * @param vertical 竖排：整枚标记转 90 度
 * @param color 颜色
 * @param dy_px 整笔的纵向平移（连页接缝补画那半截时是负的带高）
 */
void drawBreakMark(PageSink& target, float along, float cross, float size_pt, float zoom,
                   bool vertical, Color color, float dy_px) {
    // 尺寸与落点都取整到设备像素：连页接缝那一趟是同一枚标记整体上移一个带高再画一次，
    //   两趟必须落到同一批像素上（判据是逐字节相同）。取整之前算，两趟会差出半像素的
    //   抗锯齿覆盖率 —— 差的正好是带边界那两行。
    const float t = std::round(std::max(1.0f, size_pt * 0.065f * zoom));  // 笔画宽度
    const float w = std::round(size_pt * 0.58f * zoom);                   // 横笔长度
    const float h = std::round(size_pt * 0.28f * zoom);                   // 竖笔高度
    const float head = std::round(size_pt * 0.28f * zoom);                // 箭头长度
    const float half = std::round(size_pt * 0.15f * zoom);                // 箭头半高
    // 平移量在缩放与取整之后加：先除 zoom 再乘回来会引入末位误差，而判据是逐字节比。
    const float a0 = std::round(along * zoom + size_pt * 0.06f * zoom);
    // 横笔抬到字高中段（0.28 em）：贴着基线画会显得沉在字底，与字处理软件里那一枚
    //   标记的位置也不同 —— 它大致落在 x 高度上沿那一带。
    const float bar_lo = std::round(cross * zoom - size_pt * 0.28f * zoom) + dy_px;
    const float right = a0 + w;
    const float mid = bar_lo + t * 0.5f;
    if (!vertical) {
        target.fillRect(Rect{a0, bar_lo, w, t}, color);
        target.fillRect(Rect{right - t, bar_lo - h, t, h + t}, color);
        target.fillTriangle(a0, mid, a0 + head, mid - half - t * 0.3f, a0 + head,
                            mid + half + t * 0.3f, color);
    } else {
        // 竖排：整枚转 90 度 —— 行内轴变 y、块轴变 x。
        target.fillRect(Rect{bar_lo, a0, t, w}, color);
        target.fillRect(Rect{bar_lo - h, right - t, h + t, t}, color);
        target.fillTriangle(mid, a0, mid - half - t * 0.3f, a0 + head, mid + half + t * 0.3f,
                            a0 + head, color);
    }
}

}  // namespace

bool choiceMarkerOf(const RenderInput& in, const PlacedGlyph& pg, bool* checked, bool* square,
                    Color* color) {
    if (checked != nullptr) *checked = false;
    if (square != nullptr) *square = false;
    if (color != nullptr) *color = Color{};
    if (pg.paragraph < 0 || pg.paragraph >= static_cast<int>((*in.blocks).size())) return false;
    const std::vector<uint32_t>& text = (*in.blocks)[static_cast<size_t>(pg.paragraph)].cp_text;
    if (pg.cluster < 0 || static_cast<size_t>(pg.cluster) >= text.size()) return false;
    const uint32_t cp = text[static_cast<size_t>(pg.cluster)];
    const bool box = (cp == kCheckboxMarkOff || cp == kCheckboxMarkOn);
    const bool dot = (cp == kRadioMarkOff || cp == kRadioMarkOn);
    if (!box && !dot) return false;

    // 落在哪个控件上：控件集合的区间是按段给码点偏移的，问一次就知道。
    for (size_t i = 0; i < (*in.control_node_ids).size(); ++i) {
        const Control& c = in.controls->at(static_cast<int>(i));
        if (c.range.start.para != pg.paragraph) continue;
        const int off = pg.cluster - static_cast<int>(c.range.start.offset);
        if (off < 0) continue;
        const rev::Node* n = rev::findNode((*in.doc), (*in.control_node_ids)[i]);
        if (n == nullptr) continue;
        const auto* spec = std::get_if<ControlSpec>(&n->payload);
        if (spec == nullptr) continue;
        bool on = false;
        if (!controlMarkerAt(*spec, off, &on)) continue;
        if (checked != nullptr) *checked = on;
        if (square != nullptr) *square = box;
        if (color != nullptr) *color = spec->appearance.glyph.color;
        return true;
    }
    return false;
}

void paintChoiceMarker(PageSink& target, const RenderInput& in, const PlacedGlyph& pg, bool checked,
                       bool square, Color color, float dy) {
    const float zoom = in.cfg->zoom;
    if (!(zoom > 0.0f)) return;
    // 记号色没配（全透明）就跟随文字：记号是内容的一部分，正文什么颜色它就什么颜色。
    if (color.a == 0) color = pg.color;

    const float pen_x = pg.x_pt * zoom;
    const float pen_y = pg.y_pt * zoom + dy;

    // 记号的外径统一按字号定：方框与圆圈共用同一个数，两族图标的视觉大小才对得上。
    //   圆圈若借字体里的字形，它的直径就取决于字体 —— 同一个字号下与方框并排放着
    //   一个偏小一个偏大，一眼就看出来。
    const float size_px = pg.size_pt * zoom;
    const float adv_px = (pg.advance_pt > 0.0f ? pg.advance_pt : pg.size_pt) * zoom;
    if (!(size_px > 1.0f)) return;
    const float side = std::max(5.0f, std::round(size_px * 0.72f));
    const float thick = std::max(1.0f, std::round(size_px * 0.075f));
    // 格宽决定左右居中；底边坐在基线上，与字体里那些方框字一致。
    const float cx = pen_x + adv_px * 0.5f;
    const float cy = pen_y - side * 0.5f;

    if (!square) {
        // 圆圈：圆环 + 选中时圆心一个实心点。半径收到"外沿正好等于 side/2"，
        //   于是方框与圆圈的外接框一样大，切换时看着不跳。
        const float radius = std::max(1.0f, side * 0.5f - thick * 0.5f);
        target.strokeCircle(cx, cy, radius, color, thick);
        if (!checked) return;
        target.fillCircle(cx, cy, std::max(1.0f, radius - thick * 1.6f), color);
        return;
    }

    // 方框：四条边由整数矩形拼出来，边长与线宽都取整 —— 右 / 下边才不会比左 / 上边多占一像素。
    const float left = std::round(cx - side * 0.5f);
    const float top = std::round(cy - side * 0.5f);
    target.strokeRect(Rect{left, top, side, side}, color, thick);
    if (!checked) return;

    // 对勾：一条折线，圆头圆角，覆盖率按距离算 —— 与方框同为三像素宽的笔画，
    //   斜线上不会出现锯齿台阶。比例按框边长算，字号变了跟着缩。
    const float mark[6] = {
        left + side * 0.26f, top + side * 0.52f,  left + side * 0.43f,
        top + side * 0.70f,  left + side * 0.76f, top + side * 0.30f,
    };
    const float w = std::max(1.0f, std::round(side * 0.14f));
    target.strokePolyline(mark, 3, color, w);
}

void drawWatermark(PageSink& target, const RenderInput& in, int page_index, const float* band_lo_pt,
                   const float* band_hi_pt) {
    (void)page_index;
    const WatermarkSpec w = in.watermark;
    if (!w.enabled || w.text.empty()) return;
    const std::vector<uint32_t> cps = utf8ToCodepoints(w.text);
    if (cps.empty()) return;

    // 挑一张画得出第一个字的脸：引擎不知道"哪一号是中文"，只能试。
    //   水印是一条辅助文字，选错字体的代价是缺字，不会影响正文。
    FontInstance* face = nullptr;
    for (int i = 0; i < in.fonts->faceCount(); ++i) {
        FontInstance* f = in.fonts->instance(i, w.size_pt, kWeightRegular);
        if (f == nullptr) continue;
        hb_codepoint_t g = 0;
        if (hb_font_get_nominal_glyph(f->hb(), cps.front(), &g) && g != 0) {
            face = f;
            break;
        }
    }
    if (face == nullptr) return;

    hb_font_t* hb = face->hb();
    std::vector<uint32_t> gids;
    gids.reserve(cps.size());
    std::vector<float> advs;
    advs.reserve(cps.size());
    float total = 0.0f;
    for (const uint32_t cp : cps) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hb, cp, &g)) g = 0;
        const float a = static_cast<float>(hb_font_get_glyph_h_advance(hb, g)) / kFontScalePerPt;
        gids.push_back(static_cast<uint32_t>(g));
        advs.push_back(a);
        total += a;
    }
    if (!(total > 0.0f)) return;

    const float zoom = in.cfg->zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    // 落在整页的中心，不是正文区的中心：Word 的水印以纸面为参照，页边距改了水印不跟着挪。
    const float cx = in.cfg->page.width_pt * 0.5f;
    const float cy = in.cfg->page.height_pt * 0.5f;
    const float rad = w.angle_deg * 3.14159265f / 180.0f;
    const float dx = std::cos(rad);
    const float dy = std::sin(rad);
    // 基线沿法线让开一点：字形坐在基线上，不让开的话整行会偏到中心之上。
    const float nx = -dy;
    const float ny = dx;
    const float off = w.size_pt * 0.36f;

    float s = -total * 0.5f;
    for (size_t i = 0; i < gids.size(); ++i) {
        const float px = cx + dx * s + nx * off;
        const float py = cy + dy * s + ny * off;
        s += advs[i];
        if (gids[i] == 0) continue;
        // 分带重画：只画这一带里的字（上下各留一个字身，免得半个字被裁掉）。
        if (band_lo_pt != nullptr && band_hi_pt != nullptr &&
            (py < *band_lo_pt - w.size_pt || py > *band_hi_pt + w.size_pt)) {
            continue;
        }
        const uint32_t one = gids[i];
        const float x = px * zoom;
        const float y = py * zoom;
        target.drawGlyphs(hb, &one, &x, &y, 1, units_to_px, w.color, w.angle_deg);
    }
}

/**
 * 不可见字符标记（页内坐标，画进页位图）：段落标记、空格的中点、制表符的箭头。
 *
 * 只画进位图、不占宽度 —— 标记是版面之外的一层注释，断行、命中、光标落点都不认它，
 * 所以开关它只需要重光栅（签名里混了档位与颜色，见 rasterSignature）。
 *
 * 几何全部从行盒派生：段落标记坐在"这一段的最后一行"的行末（段内码点数 == 行盒的
 * last_cp），空格的中点坐在该簇的 inline_pt 与下一簇的 inline_pt 之间。字体与基线取
 * 行盒的 bracket_font / bracket_axis_pt —— 那是块主字体，标记的粗细与风格才跟正文一致。
 */
void drawEditingMarks(PageSink& target, const RenderInput& in, int page_index,
                      const float* band_lo_pt, const float* band_hi_pt, float dy_px,
                      bool spill_only) {
    // 档位与只读态的合成判断（只读是呈现态，标记会被印出来，与表单括号同一条）。
    if (!in.marks_shown) return;
    const MarksMode mode = in.cfg->marks;

    const float zoom = in.cfg->zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    const float slab_pt = in.cfg->page.slabHeightPt();
    const bool band = band_lo_pt && band_hi_pt;
    const std::vector<BlockSpec>& bs = (*in.blocks);
    const bool dot_mode = (mode == MarksMode::All);

    const std::vector<LineBox>& lines = in.layout->lines;
    for (int k : in.layout->linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        hb_font_t* hbf = lb.bracket_font->hb();
        const float x0 = lb.inline_origin_pt;
        const float x1 = lb.inline_origin_pt + lb.inline_end_pt;
        const float axis = lb.bracket_axis_pt;
        if (lb.paragraph < 0 || lb.paragraph >= static_cast<int>(bs.size())) continue;
        const std::vector<uint32_t>& text = bs[static_cast<size_t>(lb.paragraph)].cp_text;

        // 空格与制表符：每个簇各画一枚，位置取簇的起点、宽度取到下一簇的起点。
        //   簇的 cp 是段内码点下标（不是码点本身），要拿它回块流里查那一个字。
        if (dot_mode) {
            // 中点的纵向位置按这一行的字号算：太小看不见，太低就跟正文混在一起。
            const float mid = axis - lb.size_pt * 0.30f;
            for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                const LineCluster& c = lb.clusters[ci];
                if (c.placeholder || c.atom != InlineAtom::None) continue;
                if (c.cp >= static_cast<uint32_t>(text.size())) continue;
                const float a0 = x0 + c.inline_pt;
                const float a1 =
                    (ci + 1 < lb.clusters.size()) ? x0 + lb.clusters[ci + 1].inline_pt : x1;
                if (!(a1 > a0)) continue;
                const uint32_t ch = text[c.cp];
                if (ch == 0x20 || ch == 0x3000) {
                    const float cx = (a0 + a1) * 0.5f;
                    // 半角空格画中点，全角空格画空心小方块：两者的占位差四倍，同一个点
                    //   放在全角格里会显得那一格是空的（与字处理软件的画法也一致）。
                    const bool full = (ch == 0x3000);
                    const float s0 = full ? std::max(3.0f, lb.size_pt * 0.42f * zoom) : 0.0f;
                    // 半角空格的格宽只有 0.22 em：点若按字号取（0.09 em）就占了四成，
                    //   再经 fillRect 的 floor/ceil 向外取整，1 px 的框会撑成 2 px，
                    //   连续空格于是挤成一条线。按格宽的三成取，并吸附到整像素。
                    const float dot_px = std::max(1.0f, std::round((a1 - a0) * 0.30f * zoom));
                    if (!lb.vertical) {
                        if (full) {
                            const float x0 = cx * zoom - s0 * 0.5f;
                            const float y0 = mid * zoom + dy_px - s0 * 0.5f;
                            const float st = std::max(1.0f, s0 * 0.12f);
                            target.fillRect(Rect{x0, y0, s0, st}, in.cfg->mark_color);
                            target.fillRect(Rect{x0, y0 + s0 - st, s0, st}, in.cfg->mark_color);
                            target.fillRect(Rect{x0, y0, st, s0}, in.cfg->mark_color);
                            target.fillRect(Rect{x0 + s0 - st, y0, st, s0}, in.cfg->mark_color);
                        } else {
                            target.fillRect(Rect{std::round(cx * zoom - dot_px * 0.5f),
                                                 std::round(mid * zoom + dy_px), dot_px, dot_px},
                                            in.cfg->mark_color);
                        }
                    } else {
                        // 竖排：行内轴是 y、块轴是 x，中点跟着换轴（方块不必转）。
                        target.fillRect(
                            Rect{std::round(mid * zoom),
                                 std::round(cx * zoom + dy_px - dot_px * 0.5f), dot_px, dot_px},
                            in.cfg->mark_color);
                    }
                } else if (ch == 0x09) {
                    // 制表符用右箭头字形；字体里没有这一枚就跳过，不画半个箭头。
                    hb_codepoint_t g = 0;
                    if (!hb_font_get_nominal_glyph(hbf, 0x2192, &g) || !g) continue;
                    if (!lb.vertical) {
                        target.drawGlyph(hbf, g, a0 * zoom, axis * zoom + dy_px, units_to_px,
                                         in.cfg->mark_color, 0.0f);
                    } else {
                        target.drawGlyph(hbf, g, axis * zoom, a0 * zoom + dy_px, units_to_px,
                                         in.cfg->mark_color, 90.0f);
                    }
                }
            }
        }

        // 段落标记只画在这一段的最后一行：段内码点数 == 行盒的 last_cp。
        //   空段（码点 0 个）的最后一行就是它唯一那一行，标记落在行首。
        if (lb.last_cp != static_cast<uint32_t>(text.size())) continue;
        drawBreakMark(target, x1, axis, lb.size_pt, zoom, lb.vertical, in.cfg->mark_color, dy_px);
    }
}

/**
 * 表单标记（页内坐标，画进页位图）：方括号常显（开关关掉则一个都不显示）；小点是独立
 * 开关，开时所有控件都画，与焦点无关。
 * 括号用字体里的字形而非矩形拼几何：真字形与正文同粗细、同风格，随字号缩放；无法纵向
 * 拉伸，所以跨行时每行各括一次。
 *
 * 几何从行盒派生：位置取占位簇的 inline_pt、朝向取 bracket_side、字体与基线取行盒的
 * bracket_font 与 bracket_axis_pt —— 要画与要占位共用同一份数据，槽在就画得出。
 *
 * 画在页位图里而不是帧上：每一次变化都伴随重排或光栅签名变化（见 docs/步5-渲染层方案.md
 * 的 5D 小节），不需要"矩形出口"；跟着内容缓存反而少一份逐帧擦除的账。
 */
void drawControlMarks(PageSink& target, const RenderInput& in, int page_index,
                      const float* band_lo_pt, const float* band_hi_pt, float dy_px,
                      bool spill_only) {
    // 只读不画：它是呈现与打印态，括号落进版面就印出来了。
    if (in.mode == EditMode::ReadOnly) return;

    const float zoom = in.cfg->zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    const float dot = std::max(1.0f, in.cfg->control_dot_pt * zoom);
    const bool band = band_lo_pt && band_hi_pt;
    // 接缝那一趟：只画越过带底的那一截（见 drawPage），坐标整体上移一个带高。
    const float slab_pt = in.cfg->page.slabHeightPt();

    // 括号：只过这一页的行（见 LayoutResult::page_lines），带外的行跳过。
    //   与原子框那段同一个走法：一条带的边界就是行的块轴范围。
    const std::vector<LineBox>& lines = in.layout->lines;
    for (int k : in.layout->linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        for (const LineCluster& c : lb.clusters) {
            if (!c.placeholder) continue;
            // 占位簇不止括号一种：行内原子也占一个槽（它不产生字形，所以同样是占位）。
            //   这里必须显式把 None 挡掉，不能写成"不是闭括号就是开括号"：原子的
            //   bracket_side 正是 None，那样写会给每枚原子左边长出一枚开括号，
            //   而它用的是括号色、贴在框的左沿上，看着像原子自带的装饰。
            if (c.bracket_side == BracketSide::None) continue;
            // 括号跟着内容 run 的字体走：排版的槽宽按同一份字体算，绘制必须用同一个，
            //   否则字形的 advance 与槽宽对不上，括号会压字或留一条空缝。
            hb_font_t* hbf = (c.bracket_font != nullptr ? c.bracket_font : lb.bracket_font)->hb();
            const bool open = (c.bracket_side == BracketSide::Open);
            hb_codepoint_t g = 0;
            const hb_codepoint_t want = static_cast<hb_codepoint_t>(open ? '[' : ']');
            if (!hb_font_get_nominal_glyph(hbf, want, &g) || !g) continue;
            const float adv = hb_font_get_glyph_h_advance(hbf, g) * units_to_px;
            if (!(adv > 0.0f)) continue;

            const Color col = c.read_only ? in.cfg->control_mark_ro : in.cfg->control_mark;

            // 槽 = [槽首, 槽首 + 括号advance + 间隙)（槽宽由排版给，等于簇的 advance）。
            //   字形要贴住槽里朝向内容的那一端，间隙才落在括号与内容之间：
            //     [ 槽：内容在右 → 字形贴槽首（间隙落在括号右边）
            //     ] 槽：内容在左 → 字形贴槽尾（间隙落在括号左边）
            //   间隙必须和排版用同一个常数（kControlBracketSlotGapPt）。
            //     各算各的 = 括号盖到相邻的字上，或者槽里空一条而字已经走了。
            const float along_pt =
                lb.inline_origin_pt + c.inline_pt + (open ? 0.0f : kControlBracketSlotGapPt);

            if (!lb.vertical) {
                // 横排：字形原点的 y 就是基线，行内轴是 x。
                target.drawGlyph(hbf, g, along_pt * zoom, lb.bracket_axis_pt * zoom + dy_px,
                                 units_to_px, col, 0.0f);
            } else {
                // 竖排：括号跟着转 90°（顺时针）。字形轮廓本身是横排形态，
                //   [ 转过来开口朝下 —— 正好是"内容继续往下排"的意思。
                //   不转的话括号会横躺在列里，开口方向与行内轴垂直（错的）。
                //   旋转后 advance 沿 +y，所以位置分量是 (列内横轴笔位, 行内轴)。
                target.drawGlyph(hbf, g, lb.bracket_axis_pt * zoom, along_pt * zoom + dy_px,
                                 units_to_px, col, 90.0f);
            }
        }
    }

    // 提示文字：空控件里那句"该填什么"。几何取占位簇的位置与宽度（槽宽就是它排出来的
    //   那一段），字体与基线取括号那一套。它替内容占位，所以用灰色 —— 与正文同色会被
    //   当成已经填好的字读走。
    //   只读态上面已经返回了：提示是编辑期的辅助，印在纸上就是凭空多出一句话。
    for (int k : in.layout->linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
            const LineCluster& c = lb.clusters[ci];
            if (!c.control_hint) continue;
            const rev::Node* n = rev::findNode((*in.doc), static_cast<NodeId>(c.owner_control));
            const auto* spec = (n != nullptr) ? std::get_if<ControlSpec>(&n->payload) : nullptr;
            if (spec == nullptr || spec->appearance.placeholder.empty()) continue;
            const FontInstance* face =
                (c.bracket_font != nullptr) ? c.bracket_font : lb.bracket_font;
            hb_font_t* hbf = (face != nullptr) ? face->hb() : nullptr;
            if (hbf == nullptr) continue;

            const float a0 = lb.inline_origin_pt + c.inline_pt;
            const float a1 = (ci + 1 < lb.clusters.size())
                                 ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                                 : lb.inline_origin_pt + lb.inline_end_pt;
            // 宽度上限就是槽宽：排版按它截过一次，这里按同一个上限截第二次 ——
            //   换成另一个上限，截断点就会落在槽外或槽里空一段（见 hintAdvancePt）。
            const float avail = a1 - a0;
            if (!(avail > 0.0f)) continue;

            std::vector<uint32_t> ids;
            std::vector<float> xs;
            std::vector<float> ys;
            float pen = 0.0f;
            for (uint32_t cp : utf8ToCodepoints(spec->appearance.placeholder)) {
                hb_codepoint_t g = 0;
                if (!hb_font_get_nominal_glyph(hbf, cp, &g) || g == 0) continue;  // 缺字跳过
                const float adv = hb_font_get_glyph_h_advance(hbf, g) / kFontScalePerPt;
                if (pen + adv > avail + 0.01f) break;
                if (!lb.vertical) {
                    xs.push_back((a0 + pen) * zoom);
                    ys.push_back(lb.bracket_axis_pt * zoom + dy_px);
                } else {
                    // 竖排：与括号同一套做法，整笔转 90 度，沿列往下推进。
                    xs.push_back(lb.bracket_axis_pt * zoom);
                    ys.push_back((a0 + pen) * zoom + dy_px);
                }
                ids.push_back(g);
                pen += adv;
            }
            if (ids.empty()) continue;
            target.drawGlyphs(hbf, ids.data(), xs.data(), ys.data(), static_cast<int>(ids.size()),
                              units_to_px, in.cfg->control_hint, lb.vertical ? 90.0f : 0.0f);
        }
    }

    // 小点是独立开关，与括号无关：关着就到这儿结束。
    if (!in.cfg->show_control_dots) return;

    // 开启时所有控件都画 —— 不区分焦点。它只表示"这里有一个字段"，
    //   不表示"光标不在这里"，所以这里既没有 innermostAt，也没有"跳过聚焦的那个"。
    for (int ci = 0; ci < in.controls->size(); ++ci) {
        const Control& c = in.controls->at(ci);
        const Color col = c.read_only ? in.cfg->control_mark_ro : in.cfg->control_mark;
        for (const PageRect& cr : in.control_geometry->rects(ci)) {
            if (cr.page != page_index) continue;
            if (spill_only && !(cr.r_pt.y + cr.r_pt.h > slab_pt)) continue;
            const float lo_pt = cr.r_pt.y + dy_px / zoom;
            const float hi_pt = cr.r_pt.y + cr.r_pt.h + dy_px / zoom;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            // 小点：右下角、内缩一点
            const float x1 = (cr.r_pt.x + cr.r_pt.w) * zoom;
            const float y1 = (cr.r_pt.y + cr.r_pt.h) * zoom + dy_px;
            target.fillRect(Rect{x1 - dot, y1 - dot, dot, dot}, col);
        }
    }
}

}  // namespace heditor
