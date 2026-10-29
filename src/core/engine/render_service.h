/**
 * 渲染服务：拥有页位图缓存与页场景构建入口。
 *
 * 渲染侧的所有权在这一层：它持有 PageCache 并转发 buildPageScene，页场景的具体画法
 * 仍在 page_scene.cpp。
 */
#pragma once

#include "core/engine/page_cache.h"
#include "core/engine/page_scene.h"
#include "core/engine/render_input.h"

namespace heditor {

/**
 * 渲染服务：页缓存与页场景入口的所有者。
 */
class RenderService {
public:

    /**
     * 页位图缓存。
     */
    PageCache pages;

    /**
     * 构建一页的语义场景。
     * @param in 渲染输入
     * @param page 页号
     * @returns 这一页的场景
     */
    PageScene buildScene(const RenderInput& in, int page) const { return buildPageScene(in, page); }
};

}  // namespace heditor
