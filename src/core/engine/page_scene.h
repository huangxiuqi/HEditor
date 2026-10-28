/**
 * 页场景：一页画出来需要哪些项，以及每一项的版本与页内范围。
 *
 * 这一层替代的是页/行/表/浮动图四组手写指纹：绘制输入被枚举成一份可比较的值，页位图
 * 只是它的光栅缓存。当前阶段先把它用作失效判据；绘制仍由 page_raster 的 drawPage 完成，
 * 后续再把 drawPage 拆成按场景重放。
 *
 * 坐标一律是页内磅。范围给的是“这一项可能画到的外接矩形”，宁可多报不可少报。
 */
#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "core/engine/render_input.h"
#include "core/utils/types.h"

struct hb_font_t;

namespace heditor {

class Bitmap;
struct PageRasterCost;

/**
 * 场景层：只表达绘制次序与分组，不表达具体画法。
 */
enum class SceneLayer : uint8_t {
    Background = 0,
    PageBorder,
    MarginMark,
    Watermark,
    TableLine,
    Glyph,
    Atom,
    Image,
    ControlMark,
    EditingMark,
};

/**
 * 场景项的种类。同一种 layer 下用 kind 区分不同的绘制来源。
 */
enum class SceneKind : uint16_t {
    PageFill = 0,
    PageBorder,
    MarginMarks,
    Watermark,
    ImageState,
    TableRow,
    GlyphLine,
    Float,
    ControlMarks,
    EditingMarks,
};

/**
 * 场景项身份：跨两轮比较“这是不是同一项”。
 *
 * owner 是稳定身份（行下标、表 handle、图片 handle），sub 是同一 owner 下的序号；
 * spill 标记连续模式下从上一页溢到本页的那一份。
 */
struct SceneItemKey {
    /**
     * 绘制层。
     */
    SceneLayer layer = SceneLayer::Background;

    /**
     * 同一层里的绘制来源。
     */
    SceneKind kind = SceneKind::PageFill;

    /**
     * 稳定身份（行下标、表 handle、图片 handle）。
     */
    uint64_t owner = 0;

    /**
     * 同一 owner 下的序号。
     */
    uint32_t sub = 0;

    /**
     * 是否是上一页溢到本页的那一份。
     */
    bool spill = false;

    /**
     * 身份逐字段相等。
     */
    bool operator==(const SceneItemKey& o) const = default;
};

/**
 * 一个场景项：身份、这一项全部绘制输入的哈希、页内范围。
 */
struct SceneItem {
    /**
     * 这一项的身份。
     */
    SceneItemKey key;

    /**
     * 这一项全部绘制输入的哈希。
     */
    uint64_t version = 0;

    /**
     * 页内范围（磅）。
     */
    Rect bounds_pt;
};

/**
 * 一条实心矩形指令。
 */
struct FillRectOp {
    /**
     * 目标矩形。
     */
    Rect r;

    /**
     * 颜色。
     */
    Color color;
};

/**
 * 一条矩形描边指令。
 */
struct StrokeRectOp {
    /**
     * 目标矩形。
     */
    Rect r;

    /**
     * 颜色。
     */
    Color color;

    /**
     * 线宽。
     */
    float thickness = 1.0f;
};

/**
 * 一条实心圆指令。
 */
struct FillCircleOp {
    /**
     * 圆心 x。
     */
    float cx = 0.0f;

    /**
     * 圆心 y。
     */
    float cy = 0.0f;

    /**
     * 半径。
     */
    float radius = 0.0f;

    /**
     * 颜色。
     */
    Color color;
};

/**
 * 一条圆环指令。
 */
struct StrokeCircleOp {
    /**
     * 圆心 x。
     */
    float cx = 0.0f;

    /**
     * 圆心 y。
     */
    float cy = 0.0f;

    /**
     * 半径。
     */
    float radius = 0.0f;

    /**
     * 颜色。
     */
    Color color;

    /**
     * 线宽。
     */
    float thickness = 1.0f;
};

/**
 * 一条三角形指令。
 */
struct FillTriangleOp {
    /**
     * 三个顶点（x0,y0,x1,y1,x2,y2）。
     */
    float p[6] = {};

    /**
     * 颜色。
     */
    Color color;
};

/**
 * 一条折线指令。
 */
struct StrokePolylineOp {
    /**
     * 顶点（x0,y0,x1,y1,...）。
     */
    std::vector<float> pts;

    /**
     * 颜色。
     */
    Color color;

    /**
     * 线宽。
     */
    float width = 1.0f;
};

/**
 * 一个字形指令。
 */
struct DrawGlyphOp {
    /**
     * 字体。
     */
    hb_font_t* font = nullptr;

    /**
     * 字形下标。
     */
    uint32_t glyph_id = 0;

    /**
     * 原点 x。
     */
    float x = 0.0f;

    /**
     * 原点 y。
     */
    float y = 0.0f;

    /**
     * 单位换算。
     */
    float units_to_px = 0.0f;

    /**
     * 颜色。
     */
    Color color;

    /**
     * 旋转角度。
     */
    float rotate_deg = 0.0f;
};

/**
 * 一批字形指令。
 */
struct DrawGlyphsOp {
    /**
     * 字体。
     */
    hb_font_t* font = nullptr;

    /**
     * 字形下标。
     */
    std::vector<uint32_t> ids;

    /**
     * 原点 x。
     */
    std::vector<float> xs;

    /**
     * 原点 y。
     */
    std::vector<float> ys;

    /**
     * 单位换算。
     */
    float units_to_px = 0.0f;

    /**
     * 颜色。
     */
    Color color;

    /**
     * 旋转角度。
     */
    float rotate_deg = 0.0f;

    /**
     * 是否合成倾斜。
     */
    bool italic = false;
};

/**
 * 一张图片指令。
 */
struct DrawImageOp {
    /**
     * 资源身份。
     */
    uint64_t key = 0;

    /**
     * 资源版本。
     */
    uint32_t version = 0;

    /**
     * 预乘像素（记录时由资源表持有）。
     */
    const uint8_t* rgba = nullptr;

    /**
     * 源宽。
     */
    int src_w = 0;

    /**
     * 源高。
     */
    int src_h = 0;

    /**
     * 目标矩形。
     */
    Rect dst;
};

/**
 * 压入裁剪矩形。
 */
struct PushClipOp {
    /**
     * 裁剪矩形。
     */
    Rect r;
};

/**
 * 退回裁剪栈标记。
 */
struct RestoreMarkOp {
    /**
     * 标记。
     */
    int mark = 0;
};

/**
 * 一条绘制指令。
 */
using PageOp = std::variant<FillRectOp, StrokeRectOp, FillCircleOp, StrokeCircleOp, FillTriangleOp,
                            StrokePolylineOp, DrawGlyphOp, DrawGlyphsOp, DrawImageOp, PushClipOp,
                            RestoreMarkOp>;

/**
 * 一页的场景。items 按绘制序排列；root 是按序混入 key/version/bounds 的 Merkle 根。
 */
struct PageScene {
    /**
     * 场景项，按绘制序排列。
     */
    std::vector<SceneItem> items;

    /**
     * 绘制指令；只有整页记录时才非空。
     */
    std::vector<PageOp> ops;

    /**
     * 按序混入 key/version/bounds 的 Merkle 根。
     */
    uint64_t root = 0;
};

/**
 * 两轮场景的差异。full_page 为真时调用方应整页重画；否则按 rects 记脏。
 */
struct SceneDiff {
    /**
     * 是否必须整页重画。
     */
    bool full_page = false;

    /**
     * 变了的外接矩形（页内磅）。
     */
    std::vector<Rect> rects;

    /**
     * 这些矩形在块轴上的并集下沿（页内磅）。
     */
    float band_lo_pt = 0.0f;

    /**
     * 这些矩形在块轴上的并集上沿（页内磅）。
     */
    float band_hi_pt = 0.0f;
};

/**
 * 按一页的绘制输入生成场景。纯函数：不改引擎状态、不碰像素。
 * @param in 绘制输入
 * @param page 页号
 * @returns 这一页的场景（页号越界时为空场景）
 */
PageScene buildPageScene(const RenderInput& in, int page);

/**
 * 比较新旧两版场景，给出脏矩形与行带范围。
 * @param before 已画场景；nullptr 表示还没有
 * @param after 新场景
 * @param page_bounds 页内范围（磅），用来裁剪脏矩形
 * @returns 差异；full_page 为真时 rects 可忽略
 */
SceneDiff diffPageScene(const PageScene* before, const PageScene& after, const Rect& page_bounds);

/**
 * 按记录下来的绘制指令重放一页。
 * @param target 目标页位图
 * @param scene 场景；ops 为空时什么都不做
 * @param cost 分项耗时；可为 nullptr
 * @param clip 任意矩形裁剪；可为 nullptr
 */
void rasterPageScene(Bitmap& target, const PageScene& scene, PageRasterCost* cost = nullptr,
                     const Rect* clip = nullptr);

}  // namespace heditor
