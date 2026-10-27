/**
 * 图片：资源注册、插入、命中、选中与改尺寸。
 *
 * 与表格、控件分成不同文件，理由是这三件事各自的状态不同：表格靠结构表、控件靠区间树，
 * 图片只有"资源表 + 节点上的尺寸"。命中和拖拽框要读排版结果，但那是只读查询。
 *
 * 选中一张图片是对象选区，与文本选区互斥：文本选区一变就清掉它（见 projectCaret）。
 * 画面上那个框与八个把手是瞬态层（与光标、选区同一层），只叠在呈现用的像素上，
 * 不进页位图、不进导出 —— 否则打印出来会带一圈把手。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/engine/engine.h"

#include "core/engine/payload_schema.h"
#include "core/layout/layout.h"
#include "core/rev/fragment.h"
#include "core/rev/rev.h"
#include "core/utils/hash.h"
#include "core/utils/units.h"

namespace heditor {

namespace {

/**
 * 把手边长（磅）。屏幕上按缩放放大，于是任何缩放档下都是同一个视觉大小。
 */
constexpr float kImageHandlePt = 7.0f;

/**
 * 命中把手的容差（磅）。把手本身只有 7 磅，再收 3 磅才好点 —— 它有一半长在图外。
 */
constexpr float kImageHandleSlackPt = 3.0f;

/**
 * 拖拽框的线宽（磅）。恒为 1 个设备像素的效果由缩放前的磅值近似，不做取整 ——
 * 它在瞬态层，不参与逐像素判据。
 */
constexpr float kImageFramePt = 1.0f;

/**
 * 按文档顺序收集图片节点。表格里、控件里的图片都算 —— 它们同样能被点到。
 * @param n 子树根
 * @param out 追加收集结果
 */
void collectImages(const rev::Ref& n, std::vector<NodeId>* out) {
    if (!n) return;
    if (n->kind == NodeKind::Image) out->push_back(n->id);
    for (const rev::Ref& c : n->children) collectImages(c, out);
}

/**
 * 一个图片簇在页面坐标下的盒子（磅）。
 * @param lb 行盒
 * @param c 簇
 * @param ci 簇在这一行里的下标
 * @param out 写回矩形
 * @return true = 这是个图片簇，几何可算
 */
bool imageClusterRect(const LineBox& lb, const LineCluster& c, size_t ci, Rect* out) {
    if (c.atom != InlineAtom::Image) return false;
    const float a0 = lb.inline_origin_pt + c.inline_pt;
    const float a1 = (ci + 1 < lb.clusters.size())
                         ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                         : lb.inline_origin_pt + lb.inline_end_pt;
    if (!(a1 > a0) || !(c.atom_h_pt > 0.0f)) return false;
    const float top = lb.bracket_axis_pt - c.atom_h_pt;
    if (lb.vertical) {
        *out = Rect{top, a0, c.atom_h_pt, a1 - a0};
    } else {
        *out = Rect{a0, top, a1 - a0, c.atom_h_pt};
    }
    return true;
}

/**
 * 八个把手的中心（页面坐标，磅）：左上起顺时针，与 ImageHit::handle 的序号一一对应。
 * @param r 图片的页面矩形
 * @param out 写回八个中心点
 */
void imageHandleCenters(const Rect& r, float* out) {
    const float cx = r.x + r.w * 0.5f;
    const float cy = r.y + r.h * 0.5f;
    const float x1 = r.x + r.w;
    const float y1 = r.y + r.h;
    const float xs[8] = {r.x, cx, x1, x1, x1, cx, r.x, r.x};
    const float ys[8] = {r.y, r.y, r.y, cy, y1, y1, y1, cy};
    for (int i = 0; i < 8; ++i) {
        out[i * 2] = xs[i];
        out[i * 2 + 1] = ys[i];
    }
}

/**
 * 一个矩形拆成四条细边（设备像素），供瞬态层按"一堆矩形"统一叠加。
 * @param r 矩形（设备像素）
 * @param t 线宽（设备像素）
 * @param out 追加四条边的矩形
 */
void appendFrameBars(const Rect& r, float t, std::vector<Rect>* out) {
    if (!(r.w > 0.0f) || !(r.h > 0.0f)) return;
    const float w = std::max(r.w, t);
    const float h = std::max(r.h, t);
    out->push_back(Rect{r.x, r.y, w, t});
    out->push_back(Rect{r.x, r.y + h - t, w, t});
    out->push_back(Rect{r.x, r.y, t, h});
    out->push_back(Rect{r.x + w - t, r.y, t, h});
}

/**
 * 文档纵坐标 -> 块轴纵坐标（磅）。
 *
 * 页号按宿主摆页用的像素步距折，与 hitTestPt 同一口径：磅步距是未取整的页高，
 *   用它会随页号累积误差（2000 段文档第 160 页上偏下几十像素）。
 * @param cfg 引擎配置（取缩放与页度量）
 * @param page_px_h 页位图高度（设备像素）
 * @param gap_px 页间隙（设备像素）
 * @param y_doc 文档纵坐标（磅）
 * @returns 块轴纵坐标（分页下 = 页号 x 页高 + 页内；连页下是那个绝对 y）
 */
float docToFlowY(const EngineConfig& cfg, int page_px_h, int gap_px, float y_doc) {
    const float zoom = cfg.zoom;
    const int step_px = page_px_h + gap_px;
    if (!(zoom > 0.0f) || step_px <= 0) return y_doc;
    const float y_px = y_doc * zoom;
    const int page = std::max(0, static_cast<int>(std::floor(y_px / static_cast<float>(step_px))));
    const float local = (y_px - static_cast<float>(page) * static_cast<float>(step_px)) / zoom;
    // 连页下的"页"是渲染条带（页高 + 页间隙），分页下就是页高 —— 排版层用的是同一套。
    const float stride = cfg.page.continuous ? cfg.page.slabHeightPt() : cfg.page.height_pt;
    return static_cast<float>(page) * stride + local;
}

}  // namespace

bool EditorEngine::registerImage(const std::string& id, int width, int height, const uint8_t* rgba,
                                 size_t bytes, std::string* error) {
    if (!images_.put(id, width, height, rgba, bytes, error)) return false;
    // 资源变了但文档没变：页指纹看不出来，只有光栅签名里的资源变更计数能让页位图作废。
    //   不重光栅的话，画面上一直是上一张图（或者占位框）。
    raster_sig_ = rasterSignatureNow();
    page_dirty_rects_.clear();
    return true;
}

bool EditorEngine::registerImageAndRepaint(const std::string& id, int width, int height,
                                           const uint8_t* rgba, size_t bytes, std::string* error) {
    if (!registerImage(id, width, height, rgba, bytes, error)) return false;
    repaint();
    return true;
}

bool EditorEngine::dropImage(const std::string& id) {
    if (!images_.erase(id)) return false;
    // 后端按资源身份缓存了解码结果，表里删了它也不会自己消失 —— 那份缓存按 key 存，
    //   键是资源 id 的哈希，删掉资源之后就再没人能找回它。
    Bitmap::forgetImage(imageResourceKey(id));
    raster_sig_ = rasterSignatureNow();
    page_dirty_rects_.clear();
    return true;
}

bool EditorEngine::dropImageAndRepaint(const std::string& id) {
    if (!dropImage(id)) return false;
    repaint();
    return true;
}

bool EditorEngine::insertImage(const ImageSpec& spec) {
    if (!loaded_) return false;
    return edit_.insertImage(clampImageToContainer(spec));
}

ImageSpec EditorEngine::clampImageToContainer(const ImageSpec& spec) const {
    ImageSpec out = spec;
    if (!session_.has_value()) return out;
    // 只看单元格：格的宽度是排版算出来的，宿主拿不到（它只知道版心宽度）。
    //   段落里的图由宿主按版心限宽，这一层不重复判 —— 版心不是插图的上限
    //   （缩进、分栏都会改变实际可用宽度，那不是这一条要解决的事）。
    const int para = caret().para;
    const LayoutCell* cell = layoutCellAt(para, nullptr);
    if (cell == nullptr || cell->col_span <= 0) return out;
    float cell_w = 0.0f;
    for (const PlacedTable& t : layout_.placed_tables) {
        const LayoutCell* hit = nullptr;
        for (const LayoutCell& c : t.cells) {
            if (c.block_count <= 0) continue;
            if (para < c.first_block || para >= c.first_block + c.block_count) continue;
            if (hit == nullptr || c.block_count < hit->block_count) hit = &c;
        }
        if (hit == nullptr) continue;
        if (hit->cell + hit->col_span >= static_cast<int>(t.col_x.size())) continue;
        cell_w = t.col_x[static_cast<size_t>(hit->cell + hit->col_span)] -
                 t.col_x[static_cast<size_t>(hit->cell)] - 2.0f * kTableCellPadXPt;
        break;
    }
    if (!(cell_w > 8.0f)) return out;

    // 现在的显示宽度：作者给了绝对值就用它，否则按像素与 dpi 推（与 resolveAtomBox 同一口径）。
    const float dpi = spec.dpi > 0 ? static_cast<float>(spec.dpi) : 96.0f;
    const bool has_px = spec.px_w > 0 && spec.px_h > 0;
    float w = 0.0f;
    if (spec.width.kind == LengthKind::Abs)
        w = static_cast<float>(emuToPt(spec.width.abs_emu));
    else if (has_px)
        w = static_cast<float>(spec.px_w) * 72.0f / dpi;
    if (!(w > cell_w)) return out;

    // 超出格宽就按比例缩到格宽：直接照原尺寸排出去，图上会盖住右邻格与表格边框。
    const float scale = cell_w / w;
    out.width = Length::abs(ptToEmu(cell_w));
    if (spec.height.kind == LengthKind::Abs) {
        out.height = Length::abs(ptToEmu(emuToPt(spec.height.abs_emu) * scale));
    }
    return out;
}

bool EditorEngine::insertImageAndRender(const ImageSpec& spec) {
    if (!insertImage(spec)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

const std::vector<NodeId>& EditorEngine::imageNodeIds() const {
    const uint64_t gen = session_.has_value() ? session_->generation() : 0;
    if (!image_ids_valid_ || gen != image_ids_gen_) {
        image_ids_.clear();
        if (loaded_) collectImages(document().root, &image_ids_);
        image_ids_gen_ = gen;
        image_ids_valid_ = true;
    }
    return image_ids_;
}

bool EditorEngine::imageBox(int index, EditorEngine::ImageBox* out) const {
    if (out == nullptr) return false;
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.index != index) continue;
        *out = b;
        return true;
    }
    return false;
}

std::vector<EditorEngine::ImageBox> EditorEngine::images() const {
    std::vector<ImageBox> out;
    if (!loaded_) return out;
    const std::vector<NodeId>& ids = imageNodeIds();
    if (ids.empty()) return out;

    std::unordered_map<int, std::vector<PageRect>> rects;
    for (const LineBox& lb : layout_.lines) {
        for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
            const LineCluster& c = lb.clusters[ci];
            Rect r;
            if (!imageClusterRect(lb, c, ci, &r)) continue;
            PageRect pr;
            pr.page = lb.page;
            pr.r_pt = r;
            rects[c.owner_control].push_back(pr);
        }
    }

    // 浮动图在行盒里没有几何（它不占版面），矩形从排版的 placed_floats 取。
    for (const PlacedFloat& pf : layout_.placed_floats) {
        PageRect pr;
        pr.page = pf.page;
        float fx = pf.x;
        float fy = pf.y;
        imagePreviewPos(pf, &fx, &fy);
        pr.r_pt = Rect{fx, fy, pf.w, pf.h};
        rects[static_cast<int>(pf.handle)].push_back(pr);
    }

    out.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const rev::Node* n = rev::findNode(document(), ids[i]);
        if (n == nullptr) continue;
        const auto* im = std::get_if<ImageSpec>(&n->payload);
        if (im == nullptr) continue;
        ImageBox box;
        box.index = static_cast<int>(i);
        box.id = ids[i];
        box.resource_id = im->resource_id;
        const auto it = rects.find(static_cast<int>(ids[i]));
        if (it != rects.end()) {
            box.rects = it->second;
            if (!box.rects.empty()) {
                box.w_pt = box.rects.front().r_pt.w;
                box.h_pt = box.rects.front().r_pt.h;
            }
        }
        box.resource_ready = images_.find(box.resource_id) != nullptr;
        // 档位是三个字段的投影（行内 / 四周 / 上下 / 下方 / 上方），宿主只看这一个数。
        box.layout = (im->anchor == ImageAnchor::Inline)
                         ? 0
                         : (im->wrap == ImageWrap::Square
                                ? 1
                                : (im->wrap == ImageWrap::Behind
                                       ? 3
                                       : (im->wrap == ImageWrap::InFront ? 4 : 2)));
        out.push_back(std::move(box));
    }
    return out;
}

EditorEngine::ImageHit EditorEngine::imageAtPt(float x_pt_doc, float y_pt_doc) const {
    ImageHit hit;
    if (!loaded_ || layout_.lines.empty()) return hit;

    // 文档坐标 → 页号 + 页内磅。折法与 hitTestPt 逐字相同（按宿主摆页的像素步距），
    //   两处各写一套的话，页号一多就会各自累出不同的偏差。
    const float zoom = cfg_.zoom;
    const int page_px_h = pagePxHeight();
    const int step_px = page_px_h + pageGapPx();
    int page = 0;
    float local_y = y_pt_doc;
    if (step_px > 0 && zoom > 0.0f) {
        const float y_px = y_pt_doc * zoom;
        page = std::max(0, static_cast<int>(std::floor(y_px / static_cast<float>(step_px))));
        const float local_px = y_px - static_cast<float>(page) * static_cast<float>(step_px);
        if (local_px < 0.0f || local_px > static_cast<float>(page_px_h)) return hit;
        local_y = local_px / zoom;
    }

    // 节点 id → 文档顺序下标。命中只要这个映射，不必把每张图的页面矩形都算出来。
    const std::vector<NodeId>& ids = imageNodeIds();
    auto indexOf = [&ids](int node_id) {
        for (size_t i = 0; i < ids.size(); ++i) {
            if (static_cast<int>(ids[i]) == node_id) return static_cast<int>(i);
        }
        return -1;
    };

    int body_hit = -1;

    // 浮动图优先：它可能盖在文字上，而行盒里根本没有它的几何。
    for (const PlacedFloat& pf : layout_.placed_floats) {
        if (pf.page != page) continue;
        float fx = pf.x;
        float fy = pf.y;
        imagePreviewPos(pf, &fx, &fy);
        const Rect r{fx, fy, pf.w, pf.h};
        float centers[16];
        imageHandleCenters(r, centers);
        const float half =
            std::min(kImageHandlePt * 0.5f + kImageHandleSlackPt, std::min(r.w, r.h) * 0.3f);
        for (int h = 0; h < 8; ++h) {
            const float dx = std::fabs(x_pt_doc - centers[h * 2]);
            const float dy = std::fabs(local_y - centers[h * 2 + 1]);
            if (dx <= half && dy <= half) {
                hit.index = indexOf(static_cast<int>(pf.handle));
                hit.handle = h;
                return hit;
            }
        }
        if (x_pt_doc >= r.x && x_pt_doc <= r.x + r.w && local_y >= r.y && local_y <= r.y + r.h) {
            if (body_hit < 0) body_hit = indexOf(static_cast<int>(pf.handle));
        }
    }

    // 把手先于图身：把手一半在框外，先判图身的话，贴边那几磅永远拖不动。
    for (const int li : layout_.linesOnPage(page)) {
        const LineBox& lb = layout_.lines[static_cast<size_t>(li)];
        for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
            const LineCluster& c = lb.clusters[ci];
            Rect r;
            if (!imageClusterRect(lb, c, ci, &r)) continue;
            float centers[16];
            imageHandleCenters(r, centers);
            // 把手半径压到图身的三成：小图上八个把手会连成一片，图身就再也点不到了
            //   （框只有几个磅宽时，把手加容差比图还大）。
            const float half =
                std::min(kImageHandlePt * 0.5f + kImageHandleSlackPt, std::min(r.w, r.h) * 0.3f);
            // 命中区就是画出来的那个方块，不沿边线拉伸：拉伸会让整条边框都变成把手，
            //   贴着边线想落光标的地方再也落不进去。
            for (int h = 0; h < 8; ++h) {
                const float dx = std::fabs(x_pt_doc - centers[h * 2]);
                const float dy = std::fabs(local_y - centers[h * 2 + 1]);
                if (dx <= half && dy <= half) {
                    hit.index = indexOf(c.owner_control);
                    hit.handle = h;
                    return hit;
                }
            }
            const bool inside =
                x_pt_doc >= r.x && x_pt_doc <= r.x + r.w && local_y >= r.y && local_y <= r.y + r.h;
            if (inside && body_hit < 0) body_hit = indexOf(c.owner_control);
        }
    }
    hit.index = body_hit;
    return hit;
}

void EditorEngine::collectImageUi(int page, std::vector<Rect>* out) const {
    if (out == nullptr) return;
    out->clear();
    if (image_sel_ == kInvalidNodeId || mode_ != EditMode::Document) return;
    const float zoom = cfg_.zoom;
    if (!(zoom > 0.0f)) return;

    const float half = kImageHandlePt * 0.5f * zoom;
    const float bar = std::max(1.0f, kImageFramePt * zoom);

    // 浮动的图框不在行盒里，几何从 placed_floats 取；预览（拖动改尺寸 / 拖动移动）同样要跟手。
    for (const PlacedFloat& pf : layout_.placed_floats) {
        if (pf.page != page) continue;
        if (static_cast<NodeId>(pf.handle) != image_sel_) continue;
        float fx = pf.x;
        float fy = pf.y;
        imagePreviewPos(pf, &fx, &fy);
        Rect r{fx, fy, pf.w, pf.h};
        if (image_preview_.id == image_sel_) {
            const int hd = image_preview_.handle;
            const bool grow_left = (hd == 0 || hd == 6 || hd == 7);
            const bool grow_up = (hd == 0 || hd == 1 || hd == 2);
            const float right = r.x + r.w;
            const float bottom = r.y + r.h;
            if (grow_left) r.x = right - image_preview_.w_pt;
            if (grow_up) r.y = bottom - image_preview_.h_pt;
            r.w = image_preview_.w_pt;
            r.h = image_preview_.h_pt;
        }
        const Rect px{r.x * zoom, r.y * zoom, r.w * zoom, r.h * zoom};
        appendFrameBars(px, bar, out);
        float centers[16];
        imageHandleCenters(r, centers);
        for (int h = 0; h < 8; ++h) {
            out->push_back(Rect{centers[h * 2] * zoom - half, centers[h * 2 + 1] * zoom - half,
                                half * 2.0f, half * 2.0f});
        }
    }
    for (const int li : layout_.linesOnPage(page)) {
        const LineBox& lb = layout_.lines[static_cast<size_t>(li)];
        for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
            const LineCluster& c = lb.clusters[ci];
            if (static_cast<NodeId>(c.owner_control) != image_sel_) continue;
            Rect r;
            if (!imageClusterRect(lb, c, ci, &r)) continue;
            // 拖动中：框按"松手会变成多大"画，锚点由把手的方位定 —— 拖左上角时
            //   右下角不动，框往左上长。页位图里那张图还是旧尺寸（预览本来就不重排）。
            if (image_preview_.id == image_sel_) {
                const int hd = image_preview_.handle;
                const bool grow_left = (hd == 0 || hd == 6 || hd == 7);
                const bool grow_up = (hd == 0 || hd == 1 || hd == 2);
                const float right = r.x + r.w;
                const float bottom = r.y + r.h;
                if (grow_left) r.x = right - image_preview_.w_pt;
                if (grow_up) r.y = bottom - image_preview_.h_pt;
                r.w = image_preview_.w_pt;
                r.h = image_preview_.h_pt;
            }
            const Rect px{r.x * zoom, r.y * zoom, r.w * zoom, r.h * zoom};
            appendFrameBars(px, bar, out);
            float centers[16];
            imageHandleCenters(r, centers);
            for (int h = 0; h < 8; ++h) {
                out->push_back(Rect{centers[h * 2] * zoom - half, centers[h * 2 + 1] * zoom - half,
                                    half * 2.0f, half * 2.0f});
            }
        }
    }
}

void EditorEngine::restoreImageSelection(NodeId id) {
    if (id == kInvalidNodeId) return;
    for (const NodeId x : imageNodeIds()) {
        if (x != id) continue;
        image_sel_ = id;
        return;
    }
}

bool EditorEngine::selectImage(int index) {
    if (!loaded_ || mode_ != EditMode::Document) return false;
    // 下标按文档顺序，不是"返回数组里的第几项"：两者只在有节点查不到时才会分家，
    //   而那种分家会让宿主选中另一张图（静默的错对象）。
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.index != index) continue;
        // 换了一张图：上一步没走完的尺寸预览跟着作废（它的锚点与把手都是那一张的）。
        if (image_preview_.id != b.id) clearImageResizePreview();
        image_sel_ = b.id;
        return true;
    }
    return false;
}

bool EditorEngine::selectImageAndRender(int index) {
    if (!selectImage(index)) return false;
    repaint();
    return true;
}

void EditorEngine::clearImageSelection() {
    image_sel_ = kInvalidNodeId;
    clearImageResizePreview();
}

bool EditorEngine::clearImageSelectionAndRender() {
    if (image_sel_ == kInvalidNodeId && image_preview_.id == kInvalidNodeId) return false;
    clearImageSelection();
    repaint();
    return true;
}

int EditorEngine::selectedImage() const {
    if (image_sel_ == kInvalidNodeId) return -1;
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.id == image_sel_) return b.index;
    }
    return -1;
}

bool EditorEngine::resizeImage(int index, float w_pt, float h_pt, bool continue_run) {
    if (!loaded_) return false;
    if (!(w_pt > 0.0f) || !(h_pt > 0.0f)) return false;
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.index != index) continue;
        // 尺寸真的落地了：预览的使命结束（松手那一下正是"预览尺寸 = 真尺寸"）。
        clearImageResizePreview();
        return edit_.resizeImage(b.id, w_pt, h_pt, continue_run);
    }
    return false;
}

bool EditorEngine::resizeImageAndRender(int index, float w_pt, float h_pt, bool continue_run) {
    if (!resizeImage(index, w_pt, h_pt, continue_run)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::previewImageResize(int index, float w_pt, float h_pt, int handle) {
    if (!(w_pt > 0.0f) || !(h_pt > 0.0f)) return false;
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.index != index) continue;
        // 幂等按磅比：拖动每帧都会调用一次，没变就不重贴（省下一整页 blit）。
        if (image_preview_.id == b.id && image_preview_.handle == handle &&
            std::fabs(image_preview_.w_pt - w_pt) < 0.01f &&
            std::fabs(image_preview_.h_pt - h_pt) < 0.01f) {
            return false;
        }
        image_preview_.id = b.id;
        image_preview_.w_pt = w_pt;
        image_preview_.h_pt = h_pt;
        image_preview_.handle = handle;
        return true;
    }
    return false;
}

bool EditorEngine::clearImageResizePreview() {
    if (image_preview_.id == kInvalidNodeId) return false;
    image_preview_ = ImagePreview{};
    return true;
}

bool EditorEngine::previewImageResizeAndRender(int index, float w_pt, float h_pt, int handle) {
    if (!previewImageResize(index, w_pt, h_pt, handle)) return false;
    // 只呈现：预览框在瞬态层上，页位图按缓存命中，不进重排。
    repaint();
    return true;
}

const std::vector<LayoutFloat>& EditorEngine::floatsForLayout() {
    if (!image_preview_.placed || image_preview_.id == kInvalidNodeId) return floats_;
    floats_preview_ = floats_;
    for (LayoutFloat& f : floats_preview_) {
        if (f.handle != image_preview_.id) continue;
        f.dx_pt += image_preview_.dx_pt;
        f.dy_pt += image_preview_.dy_pt;
        break;
    }
    return floats_preview_;
}

void EditorEngine::imagePreviewPos(const PlacedFloat& pf, float* x, float* y) const {
    float px = pf.x;
    float py = pf.y;
    // 已经排进版面的位移不再叠一次：那种预览下 placed_floats 就是预览位置，
    //   再叠一遍图会跑到两倍远的地方（而文字只让开了一倍）。
    if (!image_preview_.placed && image_preview_.id != kInvalidNodeId &&
        image_preview_.id == static_cast<NodeId>(pf.handle)) {
        px += image_preview_.dx_pt;
        py += image_preview_.dy_pt;
    }
    // 拖动中的位置同样夹进版心：这里不夹的话，衬于文字上下方那两档（不重排、只挪绘制）
    //   会一路拖出纸外，画出来被页边界切掉一半。
    clampFloatBoxToTextArea(cfg_.page, pf.w, pf.h, &px, &py);
    if (x != nullptr) *x = px;
    if (y != nullptr) *y = py;
}

bool EditorEngine::previewImageMove(int index, float dx_pt, float dy_pt) {
    if (!loaded_) return false;
    const std::vector<ImageBox> all = images();
    for (const ImageBox& b : all) {
        if (b.index != index) continue;
        // 行内图在文字流里：挪它必须重排（后面的字要让位），没有"只挪绘制位置"这回事。
        if (b.layout == 0) return false;
        // 幂等按磅比：拖动每帧都会调用一次，没变就不重排（省下一整趟排版与光栅）。
        if (image_preview_.id == b.id && std::fabs(image_preview_.dx_pt - dx_pt) < 0.01f &&
            std::fabs(image_preview_.dy_pt - dy_pt) < 0.01f) {
            return false;
        }
        image_preview_.id = b.id;
        image_preview_.handle = -1;
        image_preview_.dx_pt = dx_pt;
        image_preview_.dy_pt = dy_pt;
        // 衬于文字下方 / 浮于文字上方不占版面：位移只影响绘制，排一次是白花。
        if (b.layout == 3 || b.layout == 4) {
            image_preview_.placed = false;
            for (const PageRect& pr : b.rects) {
                const int p = pr.page;
                if (p < 0) continue;
                if (static_cast<size_t>(p) < float_fp_new_.size()) {
                    float_fp_new_[static_cast<size_t>(p)] = floatPieceDigest(p);
                }
                markPageDirty(p);
            }
            return true;
        }
        image_preview_.placed = true;
        // 拖动中就把文字排开：位移进浮动图表，整篇重排一次。
        //   只挪绘制位置的话省下的是排版，代价是图一路压着字走、松手才跳一下 ——
        //   而"文字跟着让开"正是环绕这个东西本身。这一趟必须整篇：浮动框的纵向位置
        //   取决于它前面排了多高，续排拿不到那个上下文。
        markAllLayoutDirty();
        relayout();
        return true;
    }
    return false;
}

bool EditorEngine::clearImagePreviewAndRender() {
    // 拖动预览改的是版面：丢掉预览就等于"图回到文档里的位置"，必须重排一次才算回去。
    const bool placed = image_preview_.placed;
    if (!clearImageResizePreview()) return false;
    if (placed) {
        markAllLayoutDirty();
        relayout();
    }
    repaint();
    return true;
}

/**
 * 拖拽移动的起手判定：图片（已选中、点在图身上）优先，其次是"点落在当前文字选区里"。
 */
int EditorEngine::dragKindAt(const TextPos& p, int side, const ImageHit& ih) const {
    if (!loaded_) return 0;
    // 图片那半：按在图身上（不是把手上）就是"搬这张图"，选没选中都算 ——
    //   先点一下选中再拖，那第二下会落在连击窗口里，按"已选中"卡的话，
    //   想拖一张图就得先等半秒。把手要缩放，按住它再拖是改尺寸，不是搬家。
    //
    //   行内图与浮动图分开编号：行内图在文字流里，拖动要指出"插到哪两个字之间"；
    //   浮动图不占流，没有插入点，图本身跟着指针走。
    if (ih.index >= 0 && ih.handle < 0) {
        const std::vector<ImageBox> all = images();
        for (const ImageBox& b : all) {
            if (b.index != ih.index) continue;
            return (b.layout == 0) ? 2 : 3;
        }
        return 2;
    }
    // 文字那半：按在选区内部才是"搬这段字"。
    //   起点那一格不算内部 —— 从选区开头起手往外拖，用户表达的是"从这里重新选"，
    //   把这一下判成搬家，就再也拖不出反向选区（判据里"拖出去再拖回来收成空"
    //   正是这一手）。
    //   para 为 -1 是"这一点没有文字落点"，那就只剩搬图那一半。
    //   整格选区不搬：搬一串格子没有语义（表结构不是"一段文字"），照常拖选。
    if (p.para >= 0 && caret_.hasSelection() && caret_.cellBlocks().empty()) {
        const Range sel = caret_.selection();
        // 选区只要压到表格里的块就不搬。跨格选的文字（撤销整格删除之后恢复的正是
        //   这一形态）落不到"搬家"上：搬进另一格要把表结构一起搬，搬进正文要把
        //   表格拆开 —— 两条都没有语义，命令层会拒绝，这里就不该起手。
        for (size_t i = 0; i < blocks_full_.size() && i < block_flow_.size(); ++i) {
            if (blocks_full_[i].cell_path.empty()) continue;
            const FlowPos lo = block_flow_[i];
            const FlowPos hi = lo + block_flow_span_[i];
            if (hi > sel.begin() && lo < sel.end()) return 0;
        }
        const FlowPos f = flowPosAtSide(p.para, p.offset, side);
        if (f > sel.begin() && f <= sel.end()) return 1;
    }
    return 0;
}

/**
 * 把当前文字选区搬到点上。目标位置按命中折出来的流位置算 —— 与落光标同一把尺子。
 */
bool EditorEngine::moveSelectionToPt(float x_pt_doc, float y_pt_doc) {
    if (!session_.has_value()) return false;
    if (!caret_.hasSelection()) return false;
    TextPos p{};
    int side = 0;
    // clamp：拖到页外也要能落，否则"拖着往外一推就取消"。
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p, nullptr, &side, nullptr, nullptr, true)) return false;
    const FlowPos dest = flowPosAtSide(p.para, p.offset, side);
    const Range sel = caret_.selection();
    const rev::ClipCapture cap =
        rev::captureRange(session_->doc(), flowVisibility(), sel.begin(), sel.end());
    if (!cap.ok || cap.nodes.empty()) return false;
    return edit_.moveRange(cap.lo, cap.hi, dest, cap.nodes);
}

bool EditorEngine::moveSelectionToPtAndRender(float x_pt_doc, float y_pt_doc) {
    if (!moveSelectionToPt(x_pt_doc, y_pt_doc)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

/**
 * 把一张图片搬到点上。搬完之后节点换了 id，对象选区收起来 —— 光标落在图片之后。
 */
bool EditorEngine::moveImageToPt(int index, float x_pt_doc, float y_pt_doc, bool relative,
                                 float dx_pt, float dy_pt) {
    if (!session_.has_value()) return false;
    NodeId id = kInvalidNodeId;
    ImageSpec spec;
    bool found = false;
    // 版心里的框宽高：夹取要用它，而 spec 存的是宿主给的尺寸（进格之后还要按格宽限一次）。
    float w_pt = 0.0f;
    float h_pt = 0.0f;
    for (const ImageBox& b : images()) {
        if (b.index != index) continue;
        const rev::Node* n = rev::findNode(document(), b.id);
        if (n == nullptr) return false;
        const auto* s = std::get_if<ImageSpec>(&n->payload);
        if (s == nullptr) return false;
        id = b.id;
        spec = *s;
        w_pt = b.w_pt;
        h_pt = b.h_pt;
        found = true;
        break;
    }
    if (!found) return false;
    // 浮动图的拖动是"挪偏移"，不是"换锚点"：换锚点会让图在段落之间跳，
    //   而它本来就可以停在任意位置（衬于文字下方那种更没处可锚）。
    if (spec.anchor == ImageAnchor::Float) {
        float anchor_flow = 0.0f;
        bool anchored = false;
        for (const LayoutFloat& f : floats_) {
            if (f.handle != id) continue;
            for (const LineBox& lb : layout_.lines) {
                if (lb.paragraph != f.para) continue;
                const float stride =
                    cfg_.page.continuous ? cfg_.page.slabHeightPt() : cfg_.page.height_pt;
                const float at = static_cast<float>(lb.page) * stride + lb.block_lo_pt;
                if (!anchored || at < anchor_flow) {
                    anchor_flow = at;
                    anchored = true;
                }
            }
            break;
        }
        if (!anchored) return false;
        ImageSpec next = spec;
        if (relative) {
            // 拖动浮动图：偏移跟着指针走，锚点不动 —— 图不会在段落之间跳。
            next.dx_pt = spec.dx_pt + dx_pt;
            next.dy_pt = spec.dy_pt + dy_pt;
        } else {
            next.dx_pt = x_pt_doc - cfg_.page.margins.left;
            next.dy_pt = docToFlowY(cfg_, pagePxHeight(), pageGapPx(), y_pt_doc) - anchor_flow;
        }
        // 存进去的偏移也要夹进版心。预览那一侧已经夹过了（排版与绘制都过同一个函数），
        //   这里不夹就会把出界的那一段存进文档 —— 下次拖动要先空走那一段图才动。
        {
            const float stride =
                cfg_.page.continuous ? cfg_.page.slabHeightPt() : cfg_.page.height_pt;
            const float flow = anchor_flow + next.dy_pt;
            const int pg = static_cast<int>(std::floor(flow / stride));
            float lx = cfg_.page.margins.left + next.dx_pt;
            float ly = flow - static_cast<float>(pg) * stride;
            clampFloatBoxToTextArea(cfg_.page, w_pt, h_pt, &lx, &ly);
            next.dx_pt = lx - cfg_.page.margins.left;
            next.dy_pt = (static_cast<float>(pg) * stride + ly) - anchor_flow;
        }
        if (!edit_.setImageSpec(id, next)) return false;
        // 位置真的落地了：预览的使命结束（松手那一下正是"预览位置 = 真位置"）。
        clearImageResizePreview();
        markAllLayoutDirty();
        clearImageSelection();
        return true;
    }
    // 行内图吃落点：插到文字流的哪里，只能由指针那一点命中出来。
    TextPos p{};
    int side = 0;
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p, nullptr, &side, nullptr, nullptr, true)) return false;
    const FlowPos dest = flowPosAtSide(p.para, p.offset, side);
    if (!edit_.moveImage(id, spec, dest)) return false;
    // 块内容哈希看不见"图片换了节点号"：删了又插之后文本一模一样，损伤区间算出来是空的，
    //   增量排版于是原地不动，行盒里的图片簇还指着已经被删掉的那个节点 —— 画面上图还在
    //   老位置、选中框却在新位置。搬图是低频动作，整篇重排一次最省心。
    markAllLayoutDirty();
    clearImageSelection();
    return true;
}

std::string EditorEngine::dropSpotJson(float x_pt_doc, float y_pt_doc) const {
    TextPos p{};
    int side = 0;
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p, nullptr, &side, nullptr, nullptr, true)) {
        return pk::checked(pk::drop_spot::kSpec, "null");
    }
    int page = -1;
    Rect r{};
    if (!caret_.caretRectFor(p, &page, &r)) return pk::checked(pk::drop_spot::kSpec, "null");
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    // 磅（页内）：宿主按自己的密度折成 CSS，不必在这边再乘一次缩放。
    // w 是光标自己的宽度：落点指示要和编辑光标长得一模一样，宽度只能问它要 ——
    //   宿主按自己的缩放乘一遍，两边才会落在同样的设备像素上。
    o << "{\"page\":" << page << ",\"x\":" << (r.x + r.w * 0.5f) << ",\"y\":" << r.y
      << ",\"h\":" << r.h << ",\"w\":" << r.w << ",\"para\":" << p.para
      << ",\"offset\":" << p.offset << "}";
    return pk::checked(pk::drop_spot::kSpec, o.str());
}

bool EditorEngine::moveImageToPtAndRender(int index, float x_pt_doc, float y_pt_doc, bool relative,
                                          float dx_pt, float dy_pt) {
    if (!moveImageToPt(index, x_pt_doc, y_pt_doc, relative, dx_pt, dy_pt)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

/**
 * 改版式：把菜单上的四档翻译成"锚定方式 + 环绕方式"两个字段。
 *
 * 从行内改成浮动时，位置不动：图在段落中间就在段落中间。否则一次"改成四周型"
 *   会让图跳到段首，读者以为图丢了。做法是按当前几何反算 dx / dy（相对锚段
 *   第一行行顶与内容区左边），排完之后它落的还是原处。
 */
bool EditorEngine::setImageLayout(int index, int layout) {
    if (!loaded_) return false;
    for (const ImageBox& b : images()) {
        if (b.index != index) continue;
        const rev::Node* n = rev::findNode(document(), b.id);
        if (n == nullptr) return false;
        const auto* s = std::get_if<ImageSpec>(&n->payload);
        if (s == nullptr) return false;
        ImageSpec next = *s;
        switch (layout) {
            case 0:
                next.anchor = ImageAnchor::Inline;
                next.wrap = ImageWrap::None;
                break;
            case 1:
                next.anchor = ImageAnchor::Float;
                next.wrap = ImageWrap::Square;
                break;
            case 2:
                next.anchor = ImageAnchor::Float;
                next.wrap = ImageWrap::None;
                break;
            case 3:
                next.anchor = ImageAnchor::Float;
                next.wrap = ImageWrap::Behind;
                break;
            case 4:
                next.anchor = ImageAnchor::Float;
                next.wrap = ImageWrap::InFront;
                break;
            default:
                return false;
        }
        // 浮动 -> 浮动只换环绕方式，偏移一个字节都不动（图就留在原地）；
        //   行内 -> 浮动才需要反算偏移。反算不出来（图还没排过版）就退回 0。
        if (s->anchor != ImageAnchor::Float && next.anchor == ImageAnchor::Float) {
            float dx = 0.0f;
            float dy = 0.0f;
            if (floatOffsetsFor(b.id, b.rects, &dx, &dy)) {
                next.dx_pt = dx;
                next.dy_pt = dy;
            }
        }
        if (!edit_.setImageSpec(b.id, next)) return false;
        // 环绕方式改的是断行，而"图落在哪一页"可能整个变了：整篇重排一次，
        //   别指望块哈希算出来的损伤区间（它看得见行、看不见图的落点）。
        markAllLayoutDirty();
        return true;
    }
    return false;
}

/**
 * 反算一张图"相对锚段第一行行顶与内容区左边"的偏移，用于把它从行内改成浮动。
 * @param id 图片节点
 * @param rects 它当前的几何（页内磅）
 * @param dx 写回横向偏移
 * @param dy 写回纵向偏移
 * @returns 反算成功（图排过版、锚段也有行盒）
 */
bool EditorEngine::floatOffsetsFor(NodeId id, const std::vector<PageRect>& rects, float* dx,
                                   float* dy) const {
    if (dx == nullptr || dy == nullptr || rects.empty()) return false;
    // 锚段：浮动图取编译结果里的锚段，行内图取它所在的那一行。
    int para = -1;
    for (const LayoutFloat& f : floats_) {
        if (f.handle == id) {
            para = f.para;
            break;
        }
    }
    if (para < 0) {
        for (const LineBox& lb : layout_.lines) {
            for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                Rect r;
                if (!imageClusterRect(lb, lb.clusters[ci], ci, &r)) continue;
                if (static_cast<uint64_t>(lb.clusters[ci].owner_control) != id) continue;
                para = lb.paragraph;
                break;
            }
            if (para >= 0) break;
        }
    }
    if (para < 0) return false;
    // 锚段第一行：行盒按块轴自上而下，第一条属于这一段的就是它。
    const LineBox* first = nullptr;
    for (const LineBox& lb : layout_.lines) {
        if (lb.paragraph != para) continue;
        if (first == nullptr) {
            first = &lb;
            continue;
        }
        const float a = static_cast<float>(lb.page) * cfg_.page.height_pt + lb.block_lo_pt;
        const float b0 = static_cast<float>(first->page) * cfg_.page.height_pt + first->block_lo_pt;
        if (a < b0) first = &lb;
    }
    if (first == nullptr) return false;
    const float stride = cfg_.page.continuous ? cfg_.page.slabHeightPt() : cfg_.page.height_pt;
    const float anchor_flow = static_cast<float>(first->page) * stride + first->block_lo_pt;
    const PageRect& pr = rects.front();
    const float img_flow = static_cast<float>(pr.page) * stride + pr.r_pt.y;
    *dx = pr.r_pt.x - cfg_.page.margins.left;
    *dy = img_flow - anchor_flow;
    return true;
}

bool EditorEngine::setImageLayoutAndRender(int index, int layout) {
    if (!setImageLayout(index, layout)) return false;
    // 浮动与否改的是断行，必须重排。
    relayout();
    repaintAfterEdit();
    return true;
}

}  // namespace heditor
