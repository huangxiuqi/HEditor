/**
 * 控件几何：控件区间 ↔ 页内矩形的只读视图。
 *
 * 它是一份查询，不是一份状态：三个指针指向别处（控件集合、排版结果、owner 索引），
 * 自己一个字节都不存。所以它不需要失效账 —— 谁持有它，谁就得保证那三样在用的那一刻是
 * 当前那一版。
 *
 * 为什么单独成类：这段几何原先长在 EditorEngine 上，于是"按控件区间算出矩形"这件事被
 * 绘制、命中、宿主接口各写了一遍（格线、括号、小点、悬停框、命中测试），而它们对"左右
 * 边缘怎么取"的口径并不一致 —— 右边缘若也取"hi 那个簇的起点"，框会整整少一个字宽。
 * 收到一处之后，这类偏差只剩下一种可能：这个函数本身写错了。
 */
#pragma once

#include <unordered_map>
#include <vector>

#include "core/doc/control.h"
#include "core/layout/layout.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 控件几何的只读视图。
 */
class ControlGeometry {
public:

    /**
     * 摆好三个指针。默认构造出来是"没绑"，任何查询都返回空 —— 不崩，但也画不出东西。
     *
     * 之所以允许空绑：引擎的成员声明早于这些容器就绪（构造顺序），绑是后面的事。
     *
     * @param controls 控件集合
     * @param layout 排版结果
     * @param owner_index 节点 id → 对外控件下标
     */
    void bind(const ControlSet* controls, const LayoutResult* layout,
              const std::unordered_map<int, int>* owner_index);

    /**
     * 占位簇上的 owner 句柄 → 对外控件下标。
     *
     * 簇里记的是节点 id（序号会在插入之后整体平移，而 id 是身份），对外的口径始终是
     * 下标，所以"拿下标去和簇的 owner_control 比"的地方都要先过这里。
     *
     * @param owner 产生端句柄
     * @return 控件下标；找不到为 -1
     */
    int indexOfOwner(int owner) const;

    /**
     * 一个控件覆盖的页内矩形（每行一段）。
     *
     * 空控件没有码点，矩形量不出来 —— 除非它画着提示文字：那一段就是它的几何，宿主画
     * 悬停框、贴浮层、算小点的位置都要它。没有提示的空控件仍是零宽，照旧不给矩形（给了
     * 就是把相邻内容框进去）。
     *
     * @param index 控件下标
     * @return 矩形表（可能为空）
     */
    std::vector<PageRect> rects(int index) const;

private:

    /**
     * 控件集合（所有权不在本类）。
     */
    const ControlSet* controls_ = nullptr;

    /**
     * 排版结果（行盒与簇，几何唯一来源）。
     */
    const LayoutResult* layout_ = nullptr;

    /**
     * 节点 id → 对外下标。
     */
    const std::unordered_map<int, int>* owner_index_ = nullptr;
};

}  // namespace heditor
