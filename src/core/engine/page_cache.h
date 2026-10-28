/**
 * 页位图缓存与脏矩形账：一页一份像素，加"这一页哪几块像素变了"。
 *
 * 为什么把它从引擎里分出来：这两样东西的失效条件完全由"画出来是什么"决定（页场景与
 * 光栅签名），而引擎的其余部分是"文档是什么"。混在一个类里时，新增一个光栅配置项要
 * 同时改签名、清缓存、清脏矩形三处，漏一处的症状是"改了之后屏幕不变"。
 *
 * 缓存是唯一的像素存放处：换窗口、换缩放都不作废它（判据是页场景根与光栅签名），
 * 于是滚过一页不会把已经画好的页重画一遍。脏矩形是"自上次取走以来累积"，与像素分开 ——
 * 像素可以长期留着，脏矩形只活到宿主取走那一刻。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/engine/overlay.h"
#include "core/render/bitmap.h"
#include "core/utils/types.h"

namespace heditor {

struct PageScene;

/**
 * 一页的光栅结果：页内坐标（原点 = 页左上角），不含页间隙。
 */
struct PageSlot {
    /**
     * 页像素（页内坐标，不含页间隙）。
     */
    Bitmap px;

    /**
     * 这一份像素对应哪一版页场景。
     *
     * 场景根相同就说明“这一页画出来是什么”没变，像素可以复用。存整份场景而不只存根，
     * 因为算脏矩形要按项比新旧两版的 version 与 bounds。
     */
    std::shared_ptr<const PageScene> scene;

    /**
     * 构建这份场景时的输入键：缩放、页度量、控件只读位、浮动图预览等。
     *
     * 与光栅签名分开：签名管“画出来是什么”，这个键管“场景还算不算数”。键相同且
     * 这一拍没有重排时，直接复用场景，不必重新哈希整页的字形。
     */
    uint64_t scene_input_key = 0;

    /**
     * 这一份像素是在哪一版"光栅配置"下画的。
     *
     * 页场景只哈希内容，看不见缩放、页度量这些"画出来是什么"的共同参数。把它们的哈希
     * 记在这里，比一次就是不新鲜 —— 新增一个光栅配置项时只要加进光栅签名，所有页自动
     * 失效，不必再维护一张"记得清缓存"的清单。
     */
    uint64_t raster_sig = 0;

    /**
     * 上一次导出时叠在这一页上的覆盖层。
     *
     * 它只是"上一次记过哪几块"的账，用来算新旧差集 —— 像素本身不在页位图里，
     * 所以没有"擦"这一步：下一次导出从干净的页位图重来。
     */
    OverlayScene overlay;

    /**
     * 这一格装着页没有（没有就不用比对指纹）。
     */
    bool valid = false;
};

/**
 * 页缓存本体：页位图 + 脏矩形 + 光栅签名。
 */
class PageCache {
public:

    /**
     * 光栅签名换了一版就作废全部脏矩形（旧矩形描述的是旧像素）。
     *
     * @param sig 按当前配置算出来的签名
     * @returns 签名变了没有
     */
    bool setSignature(uint64_t sig);

    /**
     * 当前光栅签名。
     * @returns 签名
     */
    uint64_t signature() const { return sig_; }

    /**
     * 缓存里的这一页，没有就返回空。
     * @param page 页号
     * @returns 这一格，可能为空
     */
    PageSlot* find(int page);

    /**
     * 同上，只读。
     * @param page 页号
     * @returns 这一格，可能为空
     */
    const PageSlot* find(int page) const;

    /**
     * 取这一页的格子，没有就建一个。
     * @param page 页号
     * @returns 这一格
     */
    PageSlot& at(int page);

    /**
     * 缓存里有没有这一页。
     * @param page 页号
     * @returns 有没有
     */
    bool contains(int page) const;

    /**
     * 缓存的页数。
     * @returns 页数
     */
    std::size_t size() const { return slots_.size(); }

    /**
     * 清掉全部页位图与脏矩形。
     */
    void clear();

    /**
     * 只清脏矩形，页位图留着。
     */
    void clearDirty();

    /**
     * 丢掉这一页（页位图与它的脏矩形一起）。
     * @param page 页号
     */
    void erase(int page);

    /**
     * 按"离窗口最远"淘汰，窗口内的页一页都不丢。
     *
     * 不在这里做"整篇窗口就整份清掉"那种省内存的优化：页位图是导出像素的唯一来源，
     * 丢了这一段，窗口里就有几页拿不出像素。
     *
     * @param first_page 窗口首页
     * @param count 窗口页数
     */
    void trim(int first_page, int count);

    /**
     * 这一页的像素还作数吗：有格子、有场景、尺寸对得上、光栅签名与场景根都等于当前版本。
     *
     * @param page 页号
     * @param scene 这一页当前的场景
     * @param px_w 页宽（设备像素）
     * @param px_h 页高（设备像素）
     * @returns 能不能直接上屏
     */
    bool fresh(int page, const PageScene& scene, int px_w, int px_h) const;

    /**
     * 这一页有没有一份可读的像素：不比较场景，只看格子有效、签名与尺寸。
     *
     * 查询类入口（pagePixels / prewarm）用它；它们不负责判断内容是否最新，
     * 只负责“不给一份过期的像素”。
     *
     * @param page 页号
     * @param px_w 页宽（设备像素）
     * @param px_h 页高（设备像素）
     * @returns 能不能读
     */
    bool usable(int page, int px_w, int px_h) const;

    /**
     * 记一块"这一页的像素变了"（累积到被取走为止）。
     *
     * 合并规则：相交或相接的并成一个（光标擦旧画新就是这种），相距远的不并 ——
     * 并了会把中间一大片没变的像素也传上去。条数超过 16 时退回一个并集：
     * 一页几十个表单标记时逐条传反而更贵，而这一条只是"上界"，宁可多传不可少传。
     *
     * @param page 页号
     * @param r 页内像素矩形
     */
    void noteDirty(int page, const Rect& r);

    /**
     * 取走这一页累积的脏矩形（取完即清）。
     * @param page 页号
     * @returns 脏矩形，可能为空
     */
    std::vector<Rect> takeDirty(int page);

    /**
     * 这一页有没有攒着的脏矩形（读后不清）。
     * @param page 页号
     * @returns 有没有
     */
    bool hasDirty(int page) const;

    /**
     * 有脏矩形的页号（升序）。
     * @returns 页号表
     */
    std::vector<int> dirtyPages() const;

    /**
     * 全部页格，供呈现时遍历（窗口外的页要清瞬态账）。
     * @returns 页号到页格的表
     */
    std::unordered_map<int, PageSlot>& slots() { return slots_; }

    /**
     * 同上，只读。
     * @returns 页号到页格的表
     */
    const std::unordered_map<int, PageSlot>& slots() const { return slots_; }

private:

    /**
     * 页位图，按页号索引。
     */
    std::unordered_map<int, PageSlot> slots_;

    /**
     * 每页待上屏的脏矩形（页内像素坐标），自上次取走以来累积。
     */
    std::unordered_map<int, std::vector<Rect>> dirty_;

    /**
     * 当前光栅配置的签名（见 PageSlot::raster_sig）。
     */
    uint64_t sig_ = 0;
};

}  // namespace heditor
