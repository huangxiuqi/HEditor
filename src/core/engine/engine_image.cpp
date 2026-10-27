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
        pr.r_pt = Rect{pf.x, pf.y, pf.w, pf.h};
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
        const Rect r{pf.x, pf.y, pf.w, pf.h};
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

    // 浮动的图框不在行盒里，几何从 placed_floats 取；预览（拖动改尺寸）同样要跟手。
    for (const PlacedFloat& pf : layout_.placed_floats) {
        if (pf.page != page) continue;
        if (static_cast<NodeId>(pf.handle) != image_sel_) continue;
        Rect r{pf.x, pf.y, pf.w, pf.h};
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

/**
 * 拖拽移动的起手判定：图片（已选中、点在图身上）优先，其次是"点落在当前文字选区里"。
 */
int EditorEngine::dragKindAt(const TextPos& p, int side, const ImageHit& ih) const {
    if (!loaded_) return 0;
    // 图片那半：按在已选中的图的图身上（不是把手上）才是"搬这张图"。
    //   把手要缩放，按住它再拖是改变尺寸，不是搬家。
    if (ih.index >= 0 && ih.handle < 0 && image_sel_ != kInvalidNodeId) {
        for (const ImageBox& b : images()) {
            if (b.index != ih.index) continue;
            if (b.id == image_sel_) return 2;
            break;
        }
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
bool EditorEngine::moveImageToPt(int index, float x_pt_doc, float y_pt_doc) {
    if (!session_.has_value()) return false;
    NodeId id = kInvalidNodeId;
    ImageSpec spec;
    bool found = false;
    for (const ImageBox& b : images()) {
        if (b.index != index) continue;
        const rev::Node* n = rev::findNode(document(), b.id);
        if (n == nullptr) return false;
        const auto* s = std::get_if<ImageSpec>(&n->payload);
        if (s == nullptr) return false;
        id = b.id;
        spec = *s;
        found = true;
        break;
    }
    if (!found) return false;
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

bool EditorEngine::moveImageToPtAndRender(int index, float x_pt_doc, float y_pt_doc) {
    if (!moveImageToPt(index, x_pt_doc, y_pt_doc)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

/**
 * 改版式：把菜单上的四档翻译成"锚定方式 + 环绕方式"两个字段。
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
        return edit_.setImageSpec(b.id, next);
    }
    return false;
}

bool EditorEngine::setImageLayoutAndRender(int index, int layout) {
    if (!setImageLayout(index, layout)) return false;
    // 浮动与否改的是断行，必须重排。
    relayout();
    repaintAfterEdit();
    return true;
}

}  // namespace heditor
