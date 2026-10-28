/**
 * 一页的光栅化（实现见 page_raster.h）。
 *
 * 上下次序就是层次：底色 → 页边框 → 裁剪标记 → 衬于文字下方的图 → 字形 → 表格 → 原子 →
 * 浮于文字上方与四周型的图 → 水印 → 编辑标记 → 控件标记。改次序等于改"谁压住谁"，每一处
 * 都有对应的判据。
 */
#include "core/engine/page_raster.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/doc/document.h"
#include "core/engine/page_marks.h"
#include "core/layout/layout.h"
#include "core/utils/time.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 裁剪标记的臂长（磅）。18 磅 = 1/4 英寸：印刷业的裁切标记就用这个长度，它必须是一个
 * 与排版无关的固定物理尺寸 —— 臂长跟着字号或页边距变，同一份文档在不同页上就不成一套。
 * 页边距小于这个长度时按边距的一半收，免得标记越进正文里。
 */
constexpr float kMarginMarkPt = 18.0f;

void drawPage(PageSink& target, const RenderInput& in, int page_index, const float* band_lo_pt,
              const float* band_hi_pt, PageRasterCost* cost) {
    const float zoom = in.cfg->zoom;
    const int page_px_w = pagePxWidthOf(*in.cfg);
    const int page_px_h = pagePxHeightOf(*in.cfg);
    const float units_to_px = zoom / kFontScalePerPt;
    const bool band = band_lo_pt && band_hi_pt;

    const auto t_fill0 = Clock::now();
    if (band) {
        // 擦除范围向外扩 1 像素：相邻两条带的边界取两行中线，留 1 像素免得接缝上留一条线。
        const float y0 = std::max(0.0f, (*band_lo_pt) * zoom - 1.0f);
        const float y1 = std::min(static_cast<float>(page_px_h), (*band_hi_pt) * zoom + 1.0f);
        target.fillRect(Rect{0.0f, y0, static_cast<float>(page_px_w), std::max(0.0f, y1 - y0)},
                        in.cfg->page_fill);
    } else {
        // 整页：页内是一片页面底色。页间隙不在这里 —— 它归宿主（两层纸底之间
        //   的那条缝），页位图只要页本身。
        target.fillRect(
            Rect{0.0f, 0.0f, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
            in.cfg->page_fill);
    }
    // 页边框每次都补：上面那一笔底色会把它擦掉一条。连页下不画 —— 没有页，就没有页的边。
    if (in.cfg->draw_page_border && !in.cfg->page.continuous) {
        target.strokeRect(
            Rect{0.0f, 0.0f, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
            in.cfg->page_border, 1.0f);
    }
    // 裁剪标记同理每次都补：分带重画时那一带的底色会把它擦掉，而它只由这一段代码画。
    //   八条短线（四角各两条）够便宜，不必按带筛。
    if (in.cfg->show_margin_marks && !in.cfg->page.continuous) {
        const PageMargins& pm = in.cfg->page.margins;
        const float x0 = std::round(pm.left * zoom);
        const float y0 = std::round(pm.top * zoom);
        const float x1 = std::round(static_cast<float>(page_px_w) - pm.right * zoom);
        const float y1 = std::round(static_cast<float>(page_px_h) - pm.bottom * zoom);
        // 臂长固定 18 磅；边距本身比它窄时按边距的一半收（标记要留在页边距里，
        //   越进正文就变成压字的脏墨了）。
        const float narrow = std::min(std::min(pm.left, pm.right), std::min(pm.top, pm.bottom));
        const float arm_pt = std::min(kMarginMarkPt, narrow * 0.5f);
        const float len = std::round(arm_pt * zoom);
        if (len >= 1.0f && x1 - x0 > 2.0f * len && y1 - y0 > 2.0f * len) {
            const Color c = in.cfg->margin_mark_color;
            // 每个角两条臂，都朝页边距那一侧伸（裁切标记的老规矩：线在成品之外）。
            //   伸进正文里会贴着第一个字的左上角，看着像一撇脏墨。
            //   朝外那一条多画一格，两个臂才在最角上那一点接住，不留 1 像素缺口。
            auto hArm = [&](float x, float y, bool left) {
                target.fillRect(Rect{left ? x - len : x, y, len + (left ? 1.0f : 0.0f), 1.0f}, c);
            };
            auto vArm = [&](float x, float y, bool up) {
                target.fillRect(Rect{x, up ? y - len : y, 1.0f, len + (up ? 1.0f : 0.0f)}, c);
            };
            hArm(x0, y0, true);
            vArm(x0, y0, true);
            hArm(x1, y0, false);
            vArm(x1, y0, true);
            hArm(x0, y1, true);
            vArm(x0, y1, false);
            hArm(x1, y1, false);
            vArm(x1, y1, false);
        }
    }
    if (cost != nullptr) cost->fill_ms += msSince(t_fill0);

    // 水印：衬在正文下方的那一层。画在格线与字形之前 —— 它是纸上的底纹，
    //   正文要压在它上面（Word 的"衬于文字下方"同一条）。连页下也画：条带就是一页。
    drawWatermark(target, in, page_index, band_lo_pt, band_hi_pt);

    // 连页接缝：行按"行顶所在的条带"归属，一行最多跨一条边界，所以溢出只朝下流。
    //   上一条带的末行越过带底的那一截，要在本带的顶部再画一遍 —— 两条带各画自己
    //   看得见的那半，拼起来才是完整的一行。分页模式下不会发生（断页把行整个挪走）。
    const bool cont = in.cfg->page.continuous;
    const float slab_pt = in.cfg->page.slabHeightPt();
    // 位移量取位图高度（整像素），不取"带高 x 缩放"：后者带小数（A4 下 842 磅 x 1.333
    //   是 1122.7，而位图 1123 行），合上去之后段落标记的横笔正好落在两行之间 ——
    //   抗锯齿把它摊成两行浅灰，看着比别的标记粗一档。位图第 0 行就是本带顶端，
    //   减一整页高才是两带之间的真位移。
    const float spill_dy = -static_cast<float>(page_px_h);
    const bool spill = cont && page_index > 0;

    // 表格格线：画在页位图里（它不随光标变），在字形之前 —— 线压不到字上。
    //   每个格只画自己的外框，于是合并格内部不会多出几条线；跨页切开的那条边要补画
    //   （表格接着长在下一页，不是被裁掉），连页的条带切口不画（那不是页边界）。
    //
    //   分页处有两种断法，两种都要有那条边：一种是行被切开（cont_top / cont_bottom 记着），
    //   另一种是整行挪到下一页。后一种里"页脚最后一行"没有任何人给它画下边（下一行在
    //   下一页），于是表格在换页处看着没封口 —— 本片是本页最后一片时由它自己补上。
    //   上边不用管：每一片都画自己的上边（见下面那条），续页第一行本来就有。
    auto emitTables = [&](int page, float dy, bool spill_only) {
        if (in.layout->placed_tables.empty()) return;
        for (const PlacedTable& pt : in.layout->placed_tables) {
            for (size_t pi = 0; pi < pt.placed.size(); ++pi) {
                const PlacedRow& pr = pt.placed[pi];
                if (pr.page != page) continue;
                if (spill_only && !(pr.y + pr.h > slab_pt)) continue;
                // 下一片在别的页（或没有下一片）⇒ 这一片的下边没人画，得自己画。
                //   连页不算：那里的"下一片"只差一条条带，切口不是页边界。
                const bool seam_bottom =
                    (!cont) && (pi + 1 >= pt.placed.size() || pt.placed[pi + 1].page != page);
                const float top = pr.y * zoom + dy;
                const float bot = top + pr.h * zoom;
                if (bot <= top) continue;
                int row_seen = -1;
                int grid_col = 0;
                for (const LayoutCell& lc : pt.cells) {
                    if (lc.row != pr.row) continue;
                    if (row_seen != lc.row) {
                        row_seen = lc.row;
                        grid_col = 0;
                    }
                    const int c0 = grid_col;
                    grid_col += std::max(lc.col_span, 1);
                    const int c1 = grid_col;
                    if (c0 >= static_cast<int>(pt.col_x.size())) continue;
                    const int c1s = std::min(c1, static_cast<int>(pt.col_x.size()) - 1);
                    const float left = (pt.x + pt.col_x[static_cast<size_t>(c0)]) * zoom;
                    const float right = (pt.x + pt.col_x[static_cast<size_t>(c1s)]) * zoom;
                    if (right <= left) continue;
                    const int xl = static_cast<int>(std::lround(left));
                    const int xr = static_cast<int>(std::lround(right));
                    const int yt = static_cast<int>(std::lround(top));
                    const int yb = static_cast<int>(std::lround(bot));
                    if (xr <= xl || yb <= yt) continue;
                    // 先取整再画：fillRect 按 floor/ceil 取像素范围，浮点坐标下
                    //   "宽 1 磅"会被放大成两个像素 —— 那正是"边框比颜色更粗"的真因。
                    const float fxl = static_cast<float>(xl);
                    const float fyt = static_cast<float>(yt);
                    const float fh = static_cast<float>(yb - yt);
                    const float fw = static_cast<float>(xr - xl);
                    // 线一律画在边界坐标上（右边界就是 xr、下边界就是 yb），不要往格内
                    //   缩一格：缩了之后"本格的右边"与"邻格的左边"会落在相邻像素上，
                    //   中间那几条列边界就成了 2 像素宽，而外框仍是 1 像素 ——
                    //   看起来就是"竖线比横线粗"。
                    target.fillRect(Rect{fxl, fyt, 1.0f, fh}, in.cfg->table_line);
                    target.fillRect(Rect{static_cast<float>(xr), fyt, 1.0f, fh},
                                    in.cfg->table_line);
                    const bool skip_cut = cont;
                    // 上边：每一片都画自己的上边（表的顶边、每一行的分隔线都靠它）。
                    //   连页的条带切口不算（那里的"上边"是同一行接下去的断点）。
                    if (!(skip_cut && pr.cont_top))
                        target.fillRect(Rect{fxl, fyt, fw, 1.0f}, in.cfg->table_line);
                    // 下边：切出来的片段要补；本页最后一片也要补（下一行在下一页时没人给它画）；
                    //   表尾那一行照旧要画。
                    const bool last_row = (pr.row == pt.rows - 1);
                    if (!(skip_cut && pr.cont_bottom) &&
                        (pr.cont_bottom || last_row || seam_bottom))
                        target.fillRect(Rect{fxl, static_cast<float>(yb), fw, 1.0f},
                                        in.cfg->table_line);
                }
            }
        }
    };

    // 页位图里有页面底色、格线、字形、原子框，最后是表单标记（见函数末尾）。
    //
    // 同一字体 / 颜色 / 朝向的字形攒成一段，一次交给光栅器（见 Bitmap::drawGlyphs）：
    //   逐字形调用时，光栅器那笔固定开销（Skia 侧要新建 paint、装配一遍字形运行）
    //   全摊在一个字上 —— 整页重画变成 10 ms 量级就是这么来的。
    //   段内的次序不能动：它就是绘制次序，重叠字形靠它决定压盖。
    auto emitGlyphs = [&](int page, float dy, bool spill_only) {
        std::vector<uint32_t> run_ids;
        std::vector<float> run_x;
        std::vector<float> run_y;
        hb_font_t* run_font = nullptr;
        Color run_color{};
        float run_rot = 0.0f;
        bool run_italic = false;
        auto flush_run = [&]() {
            if (run_ids.empty()) return;
            target.drawGlyphs(run_font, run_ids.data(), run_x.data(), run_y.data(),
                              static_cast<int>(run_ids.size()), units_to_px, run_color, run_rot,
                              run_italic);
            run_ids.clear();
            run_x.clear();
            run_y.clear();
        };
        // 下划线按"同一行同一段的连续字形"合成一条线：逐字形画会让每个字之间留一条
        //   抗锯齿缝，看起来是一串短划。段与段之间靠 underline 标记断开（同一行里
        //   下划线文本中间夹一段非下划线，线必须断在交界处）。
        int ul_line = -1;
        float ul_x0 = 0.0f;
        float ul_x1 = 0.0f;
        float ul_y = 0.0f;
        float ul_size = 0.0f;
        Color ul_color{};
        auto flush_ul = [&]() {
            if (ul_line < 0) return;
            if (ul_x1 > ul_x0) {
                // 位置与厚度都按这一段的字号算：线落在基线下方约 0.12 em，粗约 0.055 em，
                //   再细会碎、再粗会像删除线。
                // 粗细按字号算，并且取整到整像素：不取整时 1.28 px 会横跨两行像素，
                //   看上去比实际粗一倍（而且大字号小字号都是两行，看着像"粗细固定"）。
                const float thick = std::max(1.0f, std::round(ul_size * zoom * 0.04f));
                // 位置比 Word 再低一点点：贴着字底会把相邻字的底部连成一条。
                const float y = std::round((ul_y + ul_size * 0.14f) * zoom + dy);
                const float x0 = std::round(ul_x0 * zoom);
                const float x1 = std::round(ul_x1 * zoom);
                if (x1 > x0) target.fillRect(Rect{x0, y, x1 - x0, thick}, ul_color);
            }
            ul_line = -1;
        };
        // 这一页有没有"要裁到行盒"的行。绝大多数文档没有，于是逐字形只多一次布尔比较，
        //   一次 save/restore 都不做 —— 裁字是固定值行距那条少见的路上才付的钱。
        bool any_clip = false;
        for (int li : in.layout->linesOnPage(page)) {
            if (in.layout->lines[static_cast<size_t>(li)].clip_text) {
                any_clip = true;
                break;
            }
        }
        int clip_line = -1;
        // 进来时的裁剪栈深度。换行、收尾都无条件退回到这一层：裁剪一旦少退一次，
        //   位图的画布会一直带着它，往后每一拍画出来的字都缺一截。
        const int clip_mark = target.clipMark();
        // 只过这一页的字形（见 LayoutResult::page_glyphs）。
        //   整篇遍历 + 判页号：2000 段文档约 8 万个字形，每画一页都要过一遍，
        //   反推约 2 ms/页 —— 比真正画这一页的字形（742 个约 0.9 ms）还贵。
        for (int k : in.layout->glyphsOnPage(page)) {
            const PlacedGlyph& pg = in.layout->glyphs[static_cast<size_t>(k)];
            if (!pg.font) continue;
            // 接缝那一趟只取越过带底的字形；其余情况取整页。
            //   判据用"墨迹可能触到带底"，不是"基线越过带底"：基线上方还有整个字身，
            //   基线在带底之上一点点的字，下半截照样会越过接缝 —— 那一截只由这一趟画，
            //   漏掉就是接缝处少一行字。字形的墨迹不会超出以基线为中心的一个字身，
            //   所以按字身外扩是它的上界（多取的几个由位图自己裁掉）。
            if (spill_only && !(pg.y_pt + pg.size_pt > slab_pt - 1.0f)) continue;
            // 换行就得换裁剪框：先把手上一批画掉，再进出一次裁剪栈。行高比墨迹矮时
            //   字要裁到行盒里（固定值行距），装得下的行不进栈。
            if (any_clip && pg.line != clip_line) {
                clip_line = pg.line;
                const LineBox& lb = in.layout->lines[static_cast<size_t>(pg.line)];
                // 上一行的裁剪到此为止：先把攒着的字形画掉，再退回进入时的深度。
                flush_run();
                flush_ul();
                target.restoreToMark(clip_mark);
                if (lb.clip_text) {
                    target.pushClipRect(Rect{0.0f, lb.slotLoPt() * zoom + dy,
                                             static_cast<float>(target.width()),
                                             lb.advance_pt * zoom});
                }
            }
            // 画带时按字形的纵向位置筛：行盒的块轴范围就是它的 ascent/descent，
            //   所以"落在带内"就是"属于这条带"。带向外扩了 1 像素，相邻带在中线相接。
            //   接缝那一趟要按平移之后的坐标筛（它画在本带的顶部）。
            const float y_pt = pg.y_pt + dy / zoom;
            if (band && (y_pt < *band_lo_pt - 1.0f || y_pt > *band_hi_pt + 1.0f)) continue;
            // 下划线：同一行内把连续带下划线的字形合成一条；换行或遇到不带下划线的
            //   字形时把上一段合上。右端取这个字形自己的 advance —— 按 0.6 em 估的话，
            //   行末那个汉字（1 em 宽）总会短掉四成。
            if (pg.underline) {
                if (ul_line >= 0 && pg.line != ul_line) flush_ul();
                if (ul_line < 0) {
                    ul_line = pg.line;
                    ul_x0 = pg.x_pt;
                    ul_y = pg.y_pt;
                    ul_size = pg.size_pt;
                    ul_color = pg.color;
                }
                const float adv = pg.advance_pt > 0.0f ? pg.advance_pt : pg.size_pt * 0.6f;
                ul_x1 = pg.x_pt + adv;
            } else {
                flush_ul();
            }
            // 选择类的记号由引擎自己画（方框 + 对勾 / 圆圈），而且要用记号色 ——
            //   它可能与该段文字的颜色不同，所以不能混进这一批按字色画掉。
            bool mark_on = false;
            bool mark_square = false;
            Color mark_color{};
            if (!pg.vertical && choiceMarkerOf(in, pg, &mark_on, &mark_square, &mark_color)) {
                flush_run();
                paintChoiceMarker(target, in, pg, mark_on, mark_square, mark_color, dy);
                continue;
            }
            hb_font_t* f = pg.font->hb();
            if (!run_ids.empty() && (f != run_font || pg.color != run_color ||
                                     pg.rotate_deg != run_rot || pg.italic != run_italic)) {
                flush_run();
            }
            if (run_ids.empty()) {
                run_font = f;
                run_color = pg.color;
                run_rot = pg.rotate_deg;
                run_italic = pg.italic;
            }
            run_ids.push_back(pg.glyph_id);
            run_x.push_back(pg.x_pt * zoom);
            run_y.push_back(pg.y_pt * zoom + dy);
        }
        flush_run();
        flush_ul();
        target.restoreToMark(clip_mark);
    };

    // 行内原子的占位框。与字形一样只过这一页的行（见 LayoutResult::page_lines），
    //   也照样按行带筛：一条带的边界就是行的块轴范围，框不会跨出它自己那一行。
    auto emitAtoms = [&](int page, float dy, bool spill_only) {
        for (int li : in.layout->linesOnPage(page)) {
            const LineBox& lb = in.layout->lines[static_cast<size_t>(li)];
            if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
            const float lo_pt = lb.block_lo_pt + dy / zoom;
            const float hi_pt = lb.block_hi_pt + dy / zoom;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                const LineCluster& c = lb.clusters[ci];
                if (c.atom == InlineAtom::None) continue;
                // 框在行内轴上的末端 = 下一个簇的起点（最后一个簇用行末）。
                const float a1 = (ci + 1 < lb.clusters.size())
                                     ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                                     : lb.inline_origin_pt + lb.inline_end_pt;
                drawAtomBox(target, in, lb, c, a1, dy);
            }
        }
    };

    // 浮动图。衬于文字下方的那一档画在字形之前，其余（四周型 / 上下型 / 浮于文字上方）
    //   画在字形与原子之后 —— 这个次序就是"环绕方式"在呈现上的全部含义。
    auto emitFloats = [&](int page, float dy, bool behind) {
        for (const PlacedFloat& pf : in.layout->placed_floats) {
            if (pf.page != page) continue;
            if ((pf.wrap == ImageWrap::Behind) != behind) continue;
            // 拖动移动的预览：图按位移画在新位置，文字先留在原处（松手才绕排）。
            float fx = pf.x;
            float fy = pf.y;
            floatPreviewPos(*in.cfg, in.float_preview, pf, &fx, &fy);
            const float lo_pt = fy + dy / zoom;
            const float hi_pt = lo_pt + pf.h;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            const Rect box{fx * zoom, lo_pt * zoom, pf.w * zoom, pf.h * zoom};
            if (!(box.w > 0.0f) || !(box.h > 0.0f)) continue;
            const doc::Node* n = doc::findNode((*in.doc), static_cast<NodeId>(pf.handle));
            const ImageSpec* im = (n != nullptr) ? std::get_if<ImageSpec>(&n->payload) : nullptr;
            const ImageResource* res = (im != nullptr && !im->resource_id.empty())
                                           ? in.images->find(im->resource_id)
                                           : nullptr;
            if (res != nullptr) {
                target.drawImage(imageResourceKey(res->id), res->version, res->rgba.data(),
                                 res->width, res->height, box);
            } else {
                // 资源没注册（文件刚打开）：画一个浅框占位，与行内图同一口径。
                target.fillRect(box, in.cfg->atom_fill);
                target.strokeRect(box, in.cfg->atom_border, 1.0f);
            }
        }
    };

    const auto t_glyph0 = Clock::now();
    emitFloats(page_index, 0.0f, true);
    emitTables(page_index, 0.0f, false);
    emitGlyphs(page_index, 0.0f, false);
    emitAtoms(page_index, 0.0f, false);
    emitFloats(page_index, 0.0f, false);
    if (spill) {
        emitFloats(page_index - 1, spill_dy, true);
        emitTables(page_index - 1, spill_dy, true);
        emitGlyphs(page_index - 1, spill_dy, true);
        emitAtoms(page_index - 1, spill_dy, true);
        emitFloats(page_index - 1, spill_dy, false);
    }
    if (cost != nullptr) cost->glyph_ms += msSince(t_glyph0);

    // 表单标记放在最后：括号要压在原子框之上（它是内容的最后一层），
    //   小点压在内容之上 —— 一个方块盖住几个字的角，比起被字盖掉、看着像脏点要好。
    //   编辑标记先画：它是注释性的浅灰，被括号或小点压住不心疼。
    {
        const auto t_mark0 = Clock::now();
        drawEditingMarks(target, in, page_index, band_lo_pt, band_hi_pt, 0.0f, false);
        if (spill)
            drawEditingMarks(target, in, page_index - 1, band_lo_pt, band_hi_pt, spill_dy, true);
        drawControlMarks(target, in, page_index, band_lo_pt, band_hi_pt, 0.0f, false);
        if (spill)
            drawControlMarks(target, in, page_index - 1, band_lo_pt, band_hi_pt, spill_dy, true);
        if (cost != nullptr) cost->glyph_ms += msSince(t_mark0);
    }
}
}  // namespace heditor
