/**
 * 覆盖层：光标、选区与图片手柄的瞬态矩形。
 *
 * 它与 PageScene 分开：内容页的像素不因光标闪烁而失效，覆盖层按自己的根判断变没变。
 * 当前仍由引擎在导出时把矩形合成到页位图上；矩形本身已经是独立的一层。
 */
#pragma once

#include <cstdint>
#include <vector>

#include "core/utils/types.h"

namespace heditor {

/**
 * 覆盖项的层序：选区在下、图片手柄居中、光标在上。
 */
enum class OverlayLayer : uint8_t {
    Selection = 0,
    ImageUi,
    Caret,
};

/**
 * 一个覆盖矩形（页内像素坐标）。
 */
struct OverlayItem {
    /**
     * 这一项属于哪一层。
     */
    OverlayLayer layer = OverlayLayer::Selection;

    /**
     * 页内像素矩形。
     */
    Rect rect;
};

/**
 * 一页的覆盖场景。items 按绘制序排列，root 是这一层的值指纹。
 */
struct OverlayScene {
    /**
     * 覆盖项，按绘制序排列。
     */
    std::vector<OverlayItem> items;

    /**
     * 覆盖层的配色指纹：光标色、选区色、图片手柄色。
     *
     * 颜色不属于任何一个矩形，但它决定矩形里画出来的像素。不并进根的话，改配色就是
     *   "矩形没动、像素该变"的一拍 —— 差集算出来是空的，脏区随之为空，画布停在旧色上。
     */
    uint64_t style = 0;

    /**
     * 这一层的值指纹；根相同就不必算新旧差集。
     */
    uint64_t root = 0;
};

}  // namespace heditor
