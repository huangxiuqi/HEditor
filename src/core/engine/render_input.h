/**
 * 绘制的输入：一页画出来需要知道的一切，全部是只读指针。
 *
 * 为什么单独成结构：绘制若直接读引擎的十来个私有成员，"画什么"与"引擎存了什么"就粘在
 * 一起，绘制既搬不出去，也没法在不建引擎的情况下验一张页位图。这份结构就是那条边界 ——
 * 填它的只有一个地方（EditorEngine::renderInput），读它的只有绘制。
 *
 * 字段全是必需项，不留可省略的空档：少填一项就是一处空指针，绘制第一行就会崩，
 * 不会静默画出一张缺东西的页。
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_mode.h"
#include "core/doc/node_id.h"
#include "core/doc/section_spec.h"
#include "core/engine/control_geometry.h"
#include "core/engine/engine_config.h"
#include "core/engine/image_store.h"
#include "core/layout/layout.h"
#include "core/text/font.h"

namespace heditor {

/**
 * 浮动图拖动预览的瞬态位移。
 *
 * 拖动中"图在哪、文字让不让开"是两件事：四周型 / 上下型会把位移叠进排版再真重排，衬于
 * 文字上下方那两档不占版面、只挪绘制。所以位移本身就是绘制要的一份输入（见
 * floatPreviewPos），而不是某个排版结果的字段。
 */
struct FloatPreview {
    /**
     * 预览中的图片节点（kInvalidNodeId = 没有预览）。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 瞬态位移（磅），叠在排版给出的位置上。
     */
    float dx_pt = 0.0f;

    /**
     * 同上，纵向。
     */
    float dy_pt = 0.0f;

    /**
     * 位移已经排进版面（四周型 / 上下型）。
     *
     * 为真时不能再叠一次：那种预览下 placed_floats 就是预览位置，再叠一遍图会跑到两倍远
     * 的地方，而文字只让开了一倍。
     */
    bool placed = false;
};

/**
 * 一页的绘制输入。
 */
struct RenderInput {
    /**
     * 当前修订（控件记录、图片资源标识都从它读）。
     */
    const doc::Doc* doc = nullptr;

    /**
     * 排版结果（行盒、字形、表格与浮动图的位置）。
     */
    const LayoutResult* layout = nullptr;

    /**
     * 筛过的块流：行内对象表与控件占位簇的锚点在这里。
     */
    const std::vector<BlockSpec>* blocks = nullptr;

    /**
     * 控件集合下标到节点 id 的映射（绘制控件标记时按它找记录）。
     */
    const std::vector<NodeId>* control_node_ids = nullptr;

    /**
     * 图片资源表（原子的位图字节在它这儿）。
     */
    const ImageStore* images = nullptr;

    /**
     * 控件区间树（选择类记号按它找"这一枚字落在哪个控件上"）。
     */
    const ControlSet* controls = nullptr;

    /**
     * 控件几何（括号、提示文字、小点按它取矩形）。
     */
    const ControlGeometry* control_geometry = nullptr;

    /**
     * 字体库（水印挑脸、原子框里的说明文字）。
     *
     * 不是 const：挑脸与取字形要往字体实例的缓存里写。
     */
    FontLibrary* fonts = nullptr;

    /**
     * 引擎配置（缩放、颜色、页度量、标记档位）。
     */
    const EngineConfig* cfg = nullptr;

    /**
     * 编辑模式：只读态不画控件标记与段落标记。
     */
    EditMode mode = EditMode::Document;

    /**
     * 本节的水印（绘制按节取，一页一值）。
     */
    WatermarkSpec watermark;

    /**
     * 段落标记与换行标记此刻画不画。
     *
     * 由引擎按"档位 + 模式"算好：绘制不该知道只读态与档位的关系，它只需要"画不画"。
     */
    bool marks_shown = false;

    /**
     * 浮动图拖动预览的瞬态位移。
     */
    FloatPreview float_preview;
};

/**
 * 一页在像素上的宽度（页宽 × 缩放，至少 1 像素）。
 *
 * 取整规则只有这一份：页位图的尺寸、脏矩形、导出的行距都靠它对齐，各写一遍就会出现
 * "位图 1123 行、脏区算到 1122.7"这种半像素错位。
 *
 * @param cfg 引擎配置
 * @return 像素宽度
 */
inline int pagePxWidthOf(const EngineConfig& cfg) {
    return std::max(1, static_cast<int>(std::lround(cfg.page.width_pt * cfg.zoom)));
}

/**
 * 一页在像素上的高度。
 *
 * @param cfg 引擎配置
 * @return 像素高度
 */
inline int pagePxHeightOf(const EngineConfig& cfg) {
    return std::max(1, static_cast<int>(std::lround(cfg.page.height_pt * cfg.zoom)));
}

/**
 * 一张浮动图此刻该画在哪：排版位置 + 预览位移，再夹进版心。
 *
 * 夹取在这里而不是排版里：拖动中的位置同样要夹，否则衬于文字上下方那两档（不重排、只挪
 * 绘制）会一路拖出纸外，画出来被页边界切掉一半。
 *
 * @param cfg 引擎配置（页度量与版心）
 * @param pv 拖动预览
 * @param pf 排版给出的位置
 * @param x 写回页内左端（磅），可为空
 * @param y 写回页内上端（磅），可为空
 */
inline void floatPreviewPos(const EngineConfig& cfg, const FloatPreview& pv, const PlacedFloat& pf,
                            float* x, float* y) {
    float px = pf.x;
    float py = pf.y;
    if (!pv.placed && pv.id != kInvalidNodeId && pv.id == static_cast<NodeId>(pf.handle)) {
        px += pv.dx_pt;
        py += pv.dy_pt;
    }
    clampFloatBoxToTextArea(cfg.page, pf.w, pf.h, &px, &py);
    if (x != nullptr) *x = px;
    if (y != nullptr) *y = py;
}

}  // namespace heditor
