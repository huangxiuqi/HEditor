/**
 * 排版服务：拥有整篇排版结果与整形缓存。
 *
 * 排版资源的所有权在这一层：它持有 LayoutResult 与 ShapingCache，引擎通过
 * layout_service_ 访问，排版算法本身仍在 layout.cpp。
 */
#pragma once

#include "core/layout/layout.h"

namespace heditor {

/**
 * 排版服务：排版结果与整形缓存的所有者。
 */
class LayoutService {
public:

    /**
     * 整篇排版结果。
     */
    LayoutResult layout;

    /**
     * 段级整形缓存。
     */
    ShapingCache cache;
};

}  // namespace heditor
