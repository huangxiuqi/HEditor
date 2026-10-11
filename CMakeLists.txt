cmake_minimum_required(VERSION 3.20)
project(HEditor LANGUAGES CXX C)

# ─────────────────────────────────────────────────────────────
#  一套源码，两个 target：native / wasm32
#
#  这不是"先写原生再移植"，而是从第一天就同时编。
#  凡是只在原生编得过的东西，会立刻在 wasm target 上暴露出来。
# ─────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(HEDITOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# HarfBuzz 源码根：submodule 路径优先，回退迁移期的版本号目录。
#   权威来源是 submodule（版本 = gitlink 的 commit，由 scripts/deps_init.ps1 钉到 tag）。
#   旧目录只为兼容手工解压的既有工作区，两者内容未必相同 —— submodule 若停在默认分支
#   尖端就与 tag 不同，而"取到哪一份"会改变整形结果，所以配置时打印实际用的路径。
set(HEDITOR_HB_ROOT ${HEDITOR_ROOT}/third_party/harfbuzz)
if(NOT EXISTS ${HEDITOR_HB_ROOT}/src/harfbuzz.cc)
  set(HEDITOR_HB_ROOT ${HEDITOR_ROOT}/third_party/harfbuzz-14.6.0)
  if(EXISTS ${HEDITOR_HB_ROOT}/src/harfbuzz.cc)
    message(STATUS "HarfBuzz: 迁移期旧目录 ${HEDITOR_HB_ROOT}（可跑 scripts/deps_init.ps1 换成 submodule）")
  endif()
else()
  message(STATUS "HarfBuzz: submodule ${HEDITOR_HB_ROOT}")
endif()

set(HEDITOR_ASSET_DIR ${HEDITOR_ROOT}/assets)

# 构建时间戳：编进 wasm/native 里，验证台把它显示出来。
#   "改了没生效"要先能回答"页面到底跑的哪一版" —— 光看磁盘上的文件时间不够：
#   浏览器完全可能拿的是旧副本。脚本不传时退回 dev。
set(HEDITOR_BUILD_STAMP "dev" CACHE STRING "构建时间戳（构建脚本注入）")

if(NOT EXISTS ${HEDITOR_HB_ROOT}/src/harfbuzz.cc)
  message(FATAL_ERROR
    "缺少 HarfBuzz 源码：${HEDITOR_HB_ROOT}\n"
    "请先执行 scripts/deps_init.ps1（拉 submodule 并钉到 tag 14.6.0）；\n"
    "或手工把 harfbuzz 源码放到 third_party/harfbuzz-14.6.0。")
endif()

option(HEDITOR_BUILD_TESTS "构建断言型单测" ON)

# ─────────────────────────────────────────────────────────────
#  Skia 光栅后端（可选开关）
#
#  打开后 core/render 的实现从 bitmap.cpp 换成 bitmap_skia.cpp：
#  铺面积与字形都改由 Skia 的 SkCanvas / SkFont 落下去（见 bitmap.h 顶部）。
#
#  静态库由 scripts/build_skia_wasm.ps1 产出 —— 它是 gn + ninja + emsdk 的
#  构建，只在 wasm 上有（native 侧要另配 MSVC 工具链，暂未做）。
#  因此这个开关**只在 EMSCRIPTEN 下有效**，native 侧一律走自写后端，
#  正好留下"另一把尺子"：同一份文档、同一份字体，两套后端可以对着看。
# ─────────────────────────────────────────────────────────────
set(HEDITOR_SKIA_ROOT "${HEDITOR_ROOT}/.toolchain/skia/src/skia-main" CACHE PATH "Skia 源码树")
set(HEDITOR_SKIA_LIB "${HEDITOR_SKIA_ROOT}/out/wasm/libskia.wasm.a" CACHE FILEPATH "wasm 版 libskia（gn/ninja 产出的命名带 .wasm 中缀）")
option(HEDITOR_USE_SKIA "用 Skia 做光栅化（仅 wasm；需先构建 libskia.a）" OFF)

if(HEDITOR_USE_SKIA AND NOT EMSCRIPTEN)
  message(FATAL_ERROR
    "HEDITOR_USE_SKIA=ON 目前只支持 wasm target（现有的 Skia 静态库是 wasm 版）。\n"
    "native 侧请保持 OFF —— 它同时是 Skia 版的对照实现。")
endif()

if(HEDITOR_USE_SKIA AND NOT EXISTS ${HEDITOR_SKIA_LIB})
  message(FATAL_ERROR
    "HEDITOR_USE_SKIA=ON 但找不到 Skia 静态库：${HEDITOR_SKIA_LIB}\n"
    "先跑： powershell -ExecutionPolicy Bypass -File scripts\\build_skia_wasm.ps1")
endif()

if(HEDITOR_USE_SKIA)
  set(HEDITOR_RENDER_SRC src/core/render/bitmap_skia.cpp)
else()
  set(HEDITOR_RENDER_SRC src/core/render/bitmap.cpp)
endif()

# ── 编译选项：一律按 target 下发，绝不 add_compile_options / add_compile_definitions 全局 ──
#    （全局下发会污染 add_subdirectory 进来的第三方子工程）
function(heditor_target_flags tgt)
  if(MSVC)
    # /utf-8 是必需的：源码里有中文注释与 UTF-8 字符串字面量
    target_compile_options(${tgt} PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus)
  elseif(EMSCRIPTEN)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -fno-exceptions)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# ── 头文件依赖追踪：本机（中文 VS + CMake 4.1 + Ninja + C++20）唯一可靠的路径 ──
#
# ★ 这个坑很值得写清楚，因为它的症状是「一切看起来都正常」。
#
#   两条依赖通道**同时**失效，而且都不报错：
#
#   ① /showIncludes 通道：cl 用 /showIncludes 报依赖，ninja 拿一条
#      msvc_deps_prefix 字符串去匹配那行输出。而两边的**字节**不一样：
#
#          cl.exe 通过管道吐出的  : \xd7\xa2\xd2\xe2: \xb0\xfc\xba\xac\xce\xc4\xbc\xfe:   (CP936)
#          CMake 写进 rules.ninja : \xe6\xb3\xa8\xe6\x84\x8f: ...                          (UTF-8)
#
#      （注意：在 PowerShell 里 `cl > f.txt` 看到的"UTF-8"是 PowerShell 自己
#      解码后再编码的结果，掩盖了真实字节。要拿真字节必须用子进程抓 stdout。）
#
#      逐字节比对必然不等 → ninja 把那些行当普通输出打印（所以日志里满屏
#      「注意: 包含文件:」），一条依赖都没记下（`ninja -t deps` 全是 `#deps 0`）。
#
#   ② dyndep 通道：CMake 4.1 只要 CMAKE_CXX_STANDARD >= 20，就会给 MSVC 生成
#      带 `-scanDependencies $DYNDEP_INTERMEDIATE_FILE` 的规则 —— **即使**
#      CMAKE_CXX_SCAN_FOR_MODULES 是 OFF（实测：C++17 工程生成的是普通规则，
#      加两行 C++20 就变成 dyndep 规则）。看起来像是有了更可靠的依赖来源，
#      但生成的 CXX.dd 里只有 `build <obj>: dyndep` 这种空行，
#      **一个头文件都没有** —— CMake 的 dyndep 只映射**模块**依赖。
#      （顺带：它还会让 harfbuzz 那个大 TU 白扫一遍。）
#
#   两条通道都不记头文件 → 改头文件不触发重编译 → 用旧类布局编出来的 .obj
#   与新布局的 .lib 混链 → 段错误（不是编译错误）。
#
#   ★ 这不是本构建目录的残留状态，是**确定性的**：最小复现（一个干净工程，
#     一个被两个 TU include 的头，除此之外什么都不加）：
#           set(CMAKE_CXX_STANDARD 20) + MSVC + Ninja
#           → ninja -t deps 报 #deps 0
#           → 改那个头的函数体，`ninja` 说 "no work to do"，一个都不重编
#           → 跑出来的 exe 还是旧行为
#     也就是说，**新克隆一份下来照样是坏的**。
#
#   实测代价：给 EditStats 加一个 bool 字段后，shaping_test.cpp 没重编，
#   链接出的 heditor_tests.exe 直接段错误 —— 而它打印的仍是「55 项断言，失败 0 项」。
#   这类问题不会以编译错误的形式出现，它只会让"测试通过"变成一句不可信的话。
#
#   试过并否掉的替代方案：
#     · 开 C++20 模块扫描（CMP0155 NEW + CXX_SCAN_FOR_MODULES ON）→ 见上面 ②；
#     · CMAKE_DEPFILE_FLAGS_CXX=/showIncludes- → 对 Ninja 的 MSVC 规则**毫无影响**
#       （两个变体生成的命令行逐字节相同）；而且规则里 `-showIncludes` 出现在
#       $FLAGS **之后**，所以按 target 下发的 `/showIncludes-` 也覆盖不了它。
#
#   最后采用的办法：干脆不依赖任何自动依赖通道，把该 target 自己的头文件
#   直接挂到每个源文件上（OBJECT_DEPENDS）。粒度粗（改一个头文件会重编该 target
#   的全部 TU），但它是**确定的**：不依赖本地化字符串、不依赖代码页、不依赖
#   CMake 版本。本项目每个 target 只有几个到十几个 TU，粗一点换"绝不会静默
#   用旧对象"，值。
#
#   ★ 这个办法有回归测试：scripts/verify_header_deps.ps1
#     它把 engine.h 的 mtime 推一下（内容不动），然后按**文件 mtime**断言
#     11 个 TU 全重编、5 个链接产物全重做、harfbuzz 的 obj 原封不动；
#     再把 HarfBuzz 自己的某个头的 mtime 推一下，断言它的聚合 TU 必须重编
#     （这两条方向相反，缺一条就会漏掉"改 HarfBuzz 不重编"那种静默复用）。
#     （为什么按 mtime 而不解析 ninja 输出：ninja 的控制台输出在管道里会丢尾巴，
#      实测一次 17 步的构建只捕获到 [10/17]；`ninja -n` 也因为 CONFIGURE_DEPENDS
#      的 glob stamp 而不给计划。mtime 是文件系统事实，不经过文本层。）
#     已证伪过：把本函数短路后它变红（11 个 TU 未重编，退出码 2），
#     恢复后回到全绿（当前 8 项）。改这里之后请跑它。
#
#   作用域：只对 MSVC 生效。clang/emscripten 的 depfile 通道本来是好的
#   （实测 wasm 侧某个 .o 记了 803 条依赖，精确到具体头文件），
#   所以那里 `return()` 直通，保留细粒度增量。
function(heditor_track_header_deps tgt)
  if(NOT MSVC)
    return()   # clang/gcc 走 depfile，本来就正常
  endif()
  set(_hdrs "")
  foreach(_d IN LISTS ARGN)
    # .hh 是 C++ 内部头的常见后缀（HarfBuzz 整个 src/ 用它），.cc 也要收：
    # 聚合源会把 .cc 直接 include 进来，它们是依赖而不是无关文件。
    file(GLOB_RECURSE _g CONFIGURE_DEPENDS
         "${_d}/*.h" "${_d}/*.hpp" "${_d}/*.hh" "${_d}/*.cc")
    list(APPEND _hdrs ${_g})
  endforeach()
  if(NOT _hdrs)
    return()
  endif()
  get_target_property(_srcs ${tgt} SOURCES)
  set(_own "")
  foreach(_s IN LISTS _srcs)
    if(_s MATCHES "\\.(cpp|cc|cxx)$")
      list(APPEND _own ${_s})
    endif()
  endforeach()
  set_property(SOURCE ${_own} PROPERTY OBJECT_DEPENDS "${_hdrs}")
endfunction()

# ─────────────────────────────────────────────────────────────
#  HarfBuzz（用官方聚合源 src/harfbuzz.cc：一个翻译单元装下整个库）
#  放在两个 target 之间共享，保证原生与 wasm 用的是同一份源码、同一组宏。
# ─────────────────────────────────────────────────────────────
add_library(heditor_harfbuzz STATIC ${HEDITOR_HB_ROOT}/src/harfbuzz.cc)
target_include_directories(heditor_harfbuzz SYSTEM PUBLIC ${HEDITOR_HB_ROOT}/src)
set_target_properties(heditor_harfbuzz PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# HarfBuzz 的"源文件"其实是整个 src/：harfbuzz.cc 的 69 条 include 全是 .cc，
# 再由它们包进 .hh。所以它必须挂上自己的头文件依赖 —— MSVC 侧没有任何自动依赖通道，
# 不挂的话，换 HarfBuzz 版本（或改它任何一个 .cc/.hh）都会静默复用旧对象，
# 于是编出来的是上一份源码，而构建一句提示都不给。
# 这里只覆盖 HarfBuzz 自己的头；core 的头不在其中，所以改 core 不会重编这个巨型 TU
# （那是有意的取舍，见上面 heditor_track_header_deps 的说明）。
heditor_track_header_deps(heditor_harfbuzz ${HEDITOR_HB_ROOT}/src)

if(EMSCRIPTEN)
  # 单线程 wasm：关掉互斥量相关代码，减包体
  target_compile_definitions(heditor_harfbuzz PUBLIC HB_NO_MT)
elseif(MSVC)
  target_compile_options(heditor_harfbuzz PRIVATE /utf-8 /W0 /bigobj)
else()
  target_compile_options(heditor_harfbuzz PRIVATE -w)
endif()
if(MSVC)
  target_compile_options(heditor_harfbuzz PRIVATE /utf-8 /W0 /bigobj)
endif()

# ─────────────────────────────────────────────────────────────
#  算法核（平台无关：原生与 wasm 共用）
# ─────────────────────────────────────────────────────────────
add_library(heditor_core STATIC
  src/core/doc/document.cpp
  src/core/doc/control.cpp
  src/core/doc/edit_mode.cpp
  src/core/doc/edit_policy.cpp
  src/core/doc/edit_capability.cpp
  src/core/doc/node.cpp
  src/core/doc/flow.cpp
  src/core/doc/para.cpp
  src/core/doc/invariants.cpp
  src/core/doc/compile.cpp
  src/core/doc/char_format.cpp
  src/core/doc/para_format.cpp
  src/core/doc/style.cpp
  src/core/doc/length.cpp
  src/core/doc/normalize.cpp
  src/core/doc/range.cpp
  src/core/doc/fragment.cpp
  src/core/doc/subtree.cpp
  src/core/doc/control_spec.cpp
  src/core/doc/control_index.cpp
  src/core/doc/control_api.cpp
  src/core/doc/validation.cpp
  src/core/doc/steps.cpp
  src/core/doc/transaction.cpp
  src/core/text/font.cpp
  src/core/text/shaper.cpp
  src/core/text/fallback.cpp
  ${HEDITOR_RENDER_SRC}
  src/core/layout/layout.cpp
  src/core/engine/engine.cpp
  src/core/engine/engine_layout.cpp
  src/core/engine/engine_paint.cpp
  src/core/engine/engine_audit.cpp
  src/core/engine/engine_diag.cpp
)
target_include_directories(heditor_core PUBLIC ${HEDITOR_ROOT}/src)
target_link_libraries(heditor_core PUBLIC heditor_harfbuzz)

# 演示与测试共用的固定文书。单独成库是为了不让核心库带上演示数据 ——
#   核心库提供的是能力，样例内容是喂给它的输入（见 docs/正式化架构与迁移路线.md 4.7）。
#   include 目录给到仓库根，于是各处写 #include "demo/demo_documents.h"。
add_library(heditor_demo STATIC demo/demo_documents.cpp)
target_include_directories(heditor_demo PUBLIC ${HEDITOR_ROOT})
target_link_libraries(heditor_demo PUBLIC heditor_core)
# ★ 每个 target 都要走这个函数，漏掉它只在 MSVC 上炸：源码无 BOM，
#   少了 /utf-8 就按 cp936 读，症状是"中文注释那一行报语法错"（见函数定义处的说明）。
heditor_target_flags(heditor_demo)
heditor_target_flags(heditor_core)
heditor_track_header_deps(heditor_core ${HEDITOR_ROOT}/src/core)

if(HEDITOR_USE_SKIA)
  # ── 与 libskia.a 对齐的编译期契约（一个都不能少）────────────────
  #
  # ★ 宏必须与 Skia 自己编译时**逐一对齐**：这些宏参与公共头里的类型定义
  #   （对齐方式、是否编译 SIMD 路径、YUVA 打包格式的存在性……）。
  #   漏一个不会报错，只会让两边对同一个结构体有两种理解 ——
  #   症状是运行期读到脏数据或直接 trap，而且极难定位。
  #   这份清单是从 Skia 的编译命令行（out/wasm 的 ninja 日志）里逐条抄下来的，
  #   不是猜的。
  target_compile_definitions(heditor_core PUBLIC
    HEDITOR_USE_SKIA=1
    SK_CODEC_DECODES_BMP
    SK_CODEC_DECODES_WBMP
    SK_ENABLE_PRECOMPILE
    SK_ENABLE_YUVA_PACKED_422
    SKVX_DISABLE_SIMD
    SK_FORCE_8_BYTE_ALIGNMENT
    SK_DISABLE_TRACING
  )
  # Skia 的公共头是按 "include/core/xxx.h" 这种**从源码根开始**的写法互相 include 的，
  # 所以根目录必须进 include path；另外各 public 子目录也加上，方便直接 include 文件名。
  target_include_directories(heditor_core PUBLIC
    ${HEDITOR_SKIA_ROOT}
    ${HEDITOR_SKIA_ROOT}/include/core
    ${HEDITOR_SKIA_ROOT}/include/config
    ${HEDITOR_SKIA_ROOT}/include/effects
    ${HEDITOR_SKIA_ROOT}/include/pathops
    ${HEDITOR_SKIA_ROOT}/include/ports
    ${HEDITOR_SKIA_ROOT}/include/utils
  )
  # 让使用者（宿主 target）也能拿到 HEDITOR_SKIA_LIB，链接时不用重复写路径
  target_link_libraries(heditor_core INTERFACE ${HEDITOR_SKIA_LIB})
endif()

# ─────────────────────────────────────────────────────────────
#  宿主
# ─────────────────────────────────────────────────────────────
if(EMSCRIPTEN)

  add_executable(heditor_wasm src/wasm/wasm_main.cpp)
  target_link_libraries(heditor_wasm PRIVATE heditor_core heditor_demo)
  target_compile_definitions(heditor_wasm PRIVATE HEDITOR_BUILD_STAMP="${HEDITOR_BUILD_STAMP}")
  heditor_target_flags(heditor_wasm)
  set_target_properties(heditor_wasm PROPERTIES OUTPUT_NAME "heditor" SUFFIX ".js")

  # 不 preload 字体：字体由 JS fetch 后交给引擎（生产环境本就如此）
  target_link_options(heditor_wasm PRIVATE
    --bind
    -O3
    -sMODULARIZE=1
    -sEXPORT_NAME=CreateHEditor
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=2MB
    -sFILESYSTEM=0
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32
  )
  # 供宿主脚本定位产物
  set_target_properties(heditor_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web)

else()

  add_executable(heditor_native src/app/native_main.cpp)
  target_link_libraries(heditor_native PRIVATE heditor_core heditor_demo)
  target_compile_definitions(heditor_native PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}"
                                                HEDITOR_BUILD_STAMP="${HEDITOR_BUILD_STAMP}")
  heditor_target_flags(heditor_native)
  heditor_track_header_deps(heditor_native ${HEDITOR_ROOT}/src/app ${HEDITOR_ROOT}/src/core)

  if(HEDITOR_BUILD_TESTS)
    enable_testing()
    add_executable(heditor_tests tests/shaping_test.cpp)
    target_link_libraries(heditor_tests PRIVATE heditor_core heditor_demo)
    target_compile_definitions(heditor_tests PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}")
    heditor_target_flags(heditor_tests)
    heditor_track_header_deps(heditor_tests ${HEDITOR_ROOT}/src/core ${HEDITOR_ROOT}/tests)
    add_test(NAME shaping_and_layout COMMAND heditor_tests)

    # 档 1：编辑 / 光标几何 / 命中测试 / 增量呈现
    add_executable(heditor_edit_tests tests/edit_test.cpp)
    target_link_libraries(heditor_edit_tests PRIVATE heditor_core heditor_demo)
    target_compile_definitions(heditor_edit_tests PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}")
    heditor_target_flags(heditor_edit_tests)
    heditor_track_header_deps(heditor_edit_tests ${HEDITOR_ROOT}/src/core ${HEDITOR_ROOT}/tests)
    add_test(NAME edit_and_caret COMMAND heditor_edit_tests)

    # 文档模型：节点、Pos 与流位置的双向换算（0.1）、段落分段（0.2）、
    # 不变量检查器（0.3）、编译到引擎输入（0.4）。
    # 链 heditor_demo：0.4 的判据是"编译结果与现有 BlockSpec 路径逐字段一致"，
    # 参照物就是演示文书那条路径（sampleDocument），不能自造一份对照。
    add_executable(heditor_model_tests tests/model_test.cpp)
    target_link_libraries(heditor_model_tests PRIVATE heditor_core heditor_demo)
    heditor_target_flags(heditor_model_tests)
    heditor_track_header_deps(heditor_model_tests ${HEDITOR_ROOT}/src/core ${HEDITOR_ROOT}/tests)
    add_test(NAME doc_model COMMAND heditor_model_tests)

    # 诊断探针（**不进 ctest**）：打印 EditStats / EditorEngine 的尺寸与字段偏移。
    # 用途与"为什么没法自动判定"写在 tests/probe_engine_size.cpp 顶部。
    add_executable(heditor_abi_probe tests/probe_engine_size.cpp)
    target_link_libraries(heditor_abi_probe PRIVATE heditor_core)
    heditor_target_flags(heditor_abi_probe)
    heditor_track_header_deps(heditor_abi_probe ${HEDITOR_ROOT}/src/core ${HEDITOR_ROOT}/tests)
  endif()

endif()
