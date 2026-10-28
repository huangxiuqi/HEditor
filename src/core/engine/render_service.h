/**
 * 渲染服务：拥有页位图缓存与页场景构建入口。
 *
 * 阶段五把渲染侧的所有权从 EditorEngine 里移出来；这一片先让服务持有 PageCache 并转发
 * buildPageScene，页场景的具体画法仍在 page_scene.cpp。瞬态层与光栅入口随后再搬。
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
