/**
 * 探针：打印 EditorEngine 与 EditStats 的尺寸及关键字段偏移，供人工比对。
 *
 * 头文件依赖漏掉时会出现"某个 .obj 用旧布局、库用新布局"的 ODR 破坏，表现为随机段错误
 * 而非编译错误；拿两个编译单元的 sizeof / offsetof 一比就能现形。
 * 它不进 ctest：两个 TU 读的是磁盘上同一份头文件，只要都是刚编的必然一致，而"某个 TU 是
 * 旧对象"在进程内不可观测。查这类问题要看构建图：ninja -C build/native -t deps 打出每个
 * obj 依赖了哪些头；碰一下 engine.h 再构建，重编的该是真正 include 它的那些 obj。
 *
 * 用途：尺寸或偏移出现可疑变化时跑一下拿权威数值，再与反汇编或 .pdb 对照。
 */
#include <cstddef>
#include <cstdio>

#include "core/engine/engine.h"

int main() {
    // 重定向到文件时防丢缓冲（同 shaping_test.cpp 的理由）
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("sizeof(EditStats)      = %zu\n", sizeof(heditor::EditStats));
    std::printf("sizeof(EditorEngine)   = %zu\n", sizeof(heditor::EditorEngine));
    std::printf("offsetof(layout_ran)   = %zu\n", offsetof(heditor::EditStats, layout_ran));
    std::printf("offsetof(repainted_pixels) = %zu\n",
                offsetof(heditor::EditStats, repainted_pixels));
    std::printf("offsetof(caret_h_px)   = %zu\n", offsetof(heditor::EditStats, caret_h_px));
    return 0;
}
