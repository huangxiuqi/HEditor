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
#   "改了没生效"要先能回答"页面到底跑的哪一版"。这里取的是**配置时刻**
#   （每次 cmake --preset 都是新的；CI 里可用环境变量 HEDITOR_BUILD_STAMP 覆盖成 commit 号）。
#   取配置时刻而不是构建时刻，是为了让"没改源码的构建"停在第 0 步 ——
#   否则每跑一次 cmake --build 都要重编那几个引用它的 TU 并重新链接。
#   ★ 代价：产物文件的 mtime（构建时刻）与它**不可比**，所以别拿两者相减判"旧副本"
#   （差值等于"构建比配置晚了多久"）。页面用 heditor_wasm 的 POST_BUILD 写出的
#   heditor.buildstamp 当旁证 —— 它与编进 wasm 的那个值同源。
if(DEFINED ENV{HEDITOR_BUILD_STAMP})
  set(HEDITOR_BUILD_STAMP "$ENV{HEDITOR_BUILD_STAMP}")
else()
  string(TIMESTAMP HEDITOR_BUILD_STAMP "%Y-%m-%d %H:%M:%S")
endif()

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
# 调试档：开断言、栈溢出检查与源码映射（见 wasm 链接参数里那一段说明）。
option(HEDITOR_WASM_DEBUG "wasm 构建带断言/栈检查/源码映射（调试用，release 不开）" OFF)

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
    if(HEDITOR_WASM_DEBUG)
      # 调试档要"编译期"就带完整调试信息：source map 靠 DWARF 的行号表生成。
      #   实测 -g2 不够 —— 编译命令里确实带了 -g2（41/42 个 TU），但映射表里 318 个
      #   源文件仍是 emsdk 自带的那些，本工程一个 .cpp 都不在。换成 -g 才拿得到。
      target_compile_options(${tgt} PRIVATE -g)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# ─────────────────────────────────────────────────────────────
#  头文件依赖：走 Ninja 的 /showIncludes 通道（CMake + Ninja + MSVC 的标准路径）
#
#  ★ 不要在这里挂 OBJECT_DEPENDS（把 target 目录下的头全挂到每个源文件上）：
#    它无条件重编 —— 改一个 core 头要重编 37 个 TU、实测 ~380 s，而按真实依赖只需要 9 个；
#    它还会把依赖通道的故障整个盖住（通道断了它照样全绿）。2026-10-13 撤过一次，别挂回来。
#    这条通道的前提与排查手法记在 .workbuddy/memory/ENV.md。
# ─────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────
#  HarfBuzz（用官方聚合源 src/harfbuzz.cc：一个翻译单元装下整个库）
#  放在两个 target 之间共享，保证原生与 wasm 用的是同一份源码、同一组宏。
# ─────────────────────────────────────────────────────────────
add_library(heditor_harfbuzz STATIC ${HEDITOR_HB_ROOT}/src/harfbuzz.cc)
target_include_directories(heditor_harfbuzz SYSTEM PUBLIC ${HEDITOR_HB_ROOT}/src)
set_target_properties(heditor_harfbuzz PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# HarfBuzz 的"源文件"其实是整个 src/：harfbuzz.cc 的 69 条 include 全是 .cc，
# 再由它们包进 .hh —— 这些边由 /showIncludes 记着（换 HarfBuzz 版本、或改它自己的头，
# 这个聚合 TU 必须重编，否则编出来的是上一份源码）。它不 include 任何 core 头。

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
  src/core/doc/edit_capability.cpp
  src/core/doc/node.cpp
  src/core/doc/flow.cpp
  src/core/doc/para.cpp
  src/core/doc/invariants.cpp
  src/core/doc/compile.cpp
  src/core/doc/char_format.cpp
  src/core/doc/para_format.cpp
  src/core/doc/style.cpp
  src/core/doc/table.cpp
  src/core/doc/length.cpp
  src/core/doc/normalize.cpp
  src/core/doc/range.cpp
  src/core/doc/fragment.cpp
  src/core/doc/subtree.cpp
  src/core/doc/control_spec.cpp
  src/core/doc/section_spec.cpp
  src/core/doc/field_spec.cpp
  src/core/doc/image_spec.cpp
  src/core/doc/formula_spec.cpp
  src/core/doc/control_index.cpp
  src/core/doc/control_api.cpp
  src/core/doc/validation.cpp
  src/core/doc/steps.cpp
  src/core/doc/transaction.cpp
  src/core/doc/revision.cpp
  src/core/doc/serialize.cpp
  src/core/text/font.cpp
  src/core/text/font_source.cpp
  src/core/text/shaper.cpp
  src/core/text/fallback.cpp
  ${HEDITOR_RENDER_SRC}
  src/core/layout/layout.cpp
  src/core/engine/engine.cpp
  src/core/engine/caret.cpp
  src/core/engine/edit_command.cpp
  src/core/engine/engine_layout.cpp
  src/core/engine/engine_paint.cpp
  src/core/engine/engine_audit.cpp
  src/core/engine/engine_diag.cpp
)
target_include_directories(heditor_core PUBLIC ${HEDITOR_ROOT}/src)
target_link_libraries(heditor_core PUBLIC heditor_harfbuzz)

# ── JSON（模型层序列化，1.10）────────────────────────────────────
#   nlohmann/json 3.12.0，单头文件库、MIT。只被 src/core/doc/serialize.cpp 用，
#   所以是 PRIVATE：nlohmann 的类型不进任何公共头，换库不影响别处的签名。
#   取回方式与校验写在 scripts/deps_init.ps1 与 docs/依赖与工具链管理.md。
set(HEDITOR_JSON_INCLUDE ${HEDITOR_ROOT}/third_party/nlohmann/single_include)
if(NOT EXISTS "${HEDITOR_JSON_INCLUDE}/nlohmann/json.hpp")
  message(FATAL_ERROR
    "缺少 third_party/nlohmann/single_include/nlohmann/json.hpp。"
    "跑 scripts/deps_init.ps1 取回（见 docs/依赖与工具链管理.md）。")
endif()
target_include_directories(heditor_core PRIVATE ${HEDITOR_JSON_INCLUDE})

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
# ★ 这个静态库对头依赖尤其敏感：demo_documents.cpp 直接构造 BlockSpec（改 layout.h
#   的字段就是改它的类布局），而它被所有测试链着。一旦它的 .obj 没跟着重编，后果不是
#   编译错误，而是"它的 .obj 用旧布局、测试的 .obj 用新布局"混链 —— 症状是析构时随机
#   段错误，而断言已经全过（实测：给 BlockSpec 加一个 vector 成员后，model_test 打到
#   0.4 就崩）。这条在 2026-10-12 真的发生过：heditor_demo 当时漏挂了头依赖。

if(HEDITOR_USE_SKIA)
  # ── 与 libskia.a 对齐的编译期契约（一个都不能少）────────────────
  #
  # ★ 宏必须与 Skia 自己编译时**逐一对齐**：这些宏参与公共头里的类型定义
  #   （对齐方式、是否编译 SIMD 路径、YUVA 打包格式的存在性……）。
  #   漏一个不会报错，只会让两边对同一个结构体有两种理解 ——
  #   症状是运行期读到脏数据或直接 trap，而且极难定位。
  #   这份清单是从 Skia 的编译命令行（out/wasm 的 ninja 日志）里逐条抄下来的，
  #   不是猜的。
  # ★ 这份清单里**没有** SKVX_DISABLE_SIMD：那份 libskia 是本工程用 -msimd128
  #   编出来的（见 scripts/build_skia_wasm.ps1 顶部坑 ⑤），Skia 侧不再定义它。
  #   这里若凭空补上，主程序会按标量版理解 SkVx、而库里是 SIMD 版 —— 正是上面
  #   说的"同一个结构体两种理解"，属于 ODR 违规。
  target_compile_definitions(heditor_core PUBLIC
    HEDITOR_USE_SKIA=1
    SK_CODEC_DECODES_BMP
    SK_CODEC_DECODES_WBMP
    SK_ENABLE_PRECOMPILE
    SK_ENABLE_YUVA_PACKED_422
    SK_FORCE_8_BYTE_ALIGNMENT
    SK_DISABLE_TRACING
  )
  # wasm SIMD 要对主程序也开着：Skia 的公共头（SkVx.h 等）按 __wasm_simd128__
  #   走不同实现，只有一边有就是上一条的另一种形态。连编译命令一起对齐才算数。
  target_compile_options(heditor_core PUBLIC -msimd128)
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
    # 环境必须把 worker 列进去：只写 web 时胶水层会把 ENVIRONMENT_IS_WORKER 判成
    #   常数 false、并整段删掉 importScripts 相关的分支（实测那版胶水里
    #   ENVIRONMENT_IS_WORKER=false、importScripts 出现 0 次）。那样的产物在 Worker 里
    #   仍可能实例化成功 —— 但那是走运（web 分支恰好不碰 document，且我们显式给了
    #   locateFile 绕过默认的脚本目录推导），不是配置对了。
    -sENVIRONMENT=web,worker
    -sALLOW_MEMORY_GROWTH=1
    # 初始内存是**起步值**，不是上限（ALLOW_MEMORY_GROWTH=1 会按需增长）。
    #
    # 为什么关心它：wasm 的内存是**每实例一份**的 —— 一个页面开 N 个编辑器就有 N 份。
    # 实测（2026-10-14）：64MB 起点时每实例恒占 64MB；16MB 起点时，灌完 16.9MB 的
    #   中文字体是 36.3MB（期间增长两次，且 HEAPU8 视图随增长更新正常），
    #   两个实例同时装载并渲染互不干扰，浏览器探针 256 项全绿。
    # 调高能少几次增长，调低能让多实例更省内存。真正的大头是字体（十几 MB 的 CJK 字体，
    #   每实例一份）—— 要压内存先做字体子集化，比调这个数更有效。
    -sINITIAL_MEMORY=16MB
    -sSTACK_SIZE=2MB
    # 字体按需读要有文件系统：宿主把字体字节写进 MEMFS，引擎按路径按需取块。
    #   默认的 -sFILESYSTEM=0 会让 fopen 直接失败，所以这里必须放开。
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,FS
  )

  if(HEDITOR_USE_SKIA)
    # libskia 是本工程用 -msimd128 编的，而 wasm-ld 要求所有对象文件与库的
    #   target_features 一致 —— 少这一条会报
    #   "linking module flags 'target_features': IDs have conflicting values"。
    target_link_options(heditor_wasm PRIVATE -msimd128)
  endif()

  # 调试档（HEDITOR_WASM_DEBUG=ON，预设 wasm-debug）：断言、栈溢出检查、源码映射。
  #   release 档一个都不带 —— 它们都要付体积与速度的代价，是排查时才需要的东西。
  #   为什么要显式写 ASSERTIONS：它在优化构建里默认是关的（默认只在 -O0 开）。
  #   STACK_OVERFLOW_CHECK 会随 ASSERTIONS=1 默认为 1，写出来是为了不依赖那条隐含规则。
  #   -g2 保函数名与行号（不影响优化）；-gsource-map 让浏览器把 wasm 栈映射回 C++ 源码。
  #   用途：trap（越界、非法指令、栈溢出）时能拿到可读的栈与源码位置，而不是一句
  #   "RuntimeError: memory access out of bounds"。共享堆那种"越界写进邻居不 trap"的情形
  #   它抓不到（那要 ASan），但至少把"响的那一半"变得可读。
  if(HEDITOR_WASM_DEBUG)
    target_link_options(heditor_wasm PRIVATE
      -g
      -gsource-map
      -sASSERTIONS=1
      -sSTACK_OVERFLOW_CHECK=1)
  endif()

  # 供宿主脚本定位产物
  set_target_properties(heditor_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web)

  # 构建戳旁证：把"这次构建用的是哪一次配置的戳"写到产物旁边。
  #   页面判"跑的是不是磁盘上这一份"要拿同一种时刻对照：wasm 里编的是配置时刻，
  #   而产物文件的 mtime 是构建时刻，两者不可比。这份旁证与编进 wasm 的值同源，
  #   且只在构建成功时才刷新 —— 构建失败时它保持旧值，于是页面对照出来的仍是"一致"。
  file(WRITE ${CMAKE_BINARY_DIR}/heditor_buildstamp.txt "${HEDITOR_BUILD_STAMP}\n")
  add_custom_command(TARGET heditor_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_BINARY_DIR}/heditor_buildstamp.txt
            $<TARGET_FILE_DIR:heditor_wasm>/heditor.buildstamp
    COMMENT "写构建戳旁证 heditor.buildstamp")

else()

  add_executable(heditor_native src/app/native_main.cpp)
  target_link_libraries(heditor_native PRIVATE heditor_core heditor_demo)
  target_compile_definitions(heditor_native PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}"
                                                HEDITOR_BUILD_STAMP="${HEDITOR_BUILD_STAMP}")
  heditor_target_flags(heditor_native)

  if(HEDITOR_BUILD_TESTS)
    enable_testing()
    add_executable(heditor_tests tests/shaping_test.cpp)
    target_link_libraries(heditor_tests PRIVATE heditor_core heditor_demo)
    target_compile_definitions(heditor_tests PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}")
    heditor_target_flags(heditor_tests)
    add_test(NAME shaping_and_layout COMMAND heditor_tests)

    # 档 1：编辑 / 光标几何 / 命中测试 / 增量呈现
    add_executable(heditor_edit_tests tests/edit_test.cpp)
    target_link_libraries(heditor_edit_tests PRIVATE heditor_core heditor_demo)
    target_compile_definitions(heditor_edit_tests PRIVATE HEDITOR_NATIVE_ASSET_DIR="${HEDITOR_ASSET_DIR}")
    heditor_target_flags(heditor_edit_tests)
    add_test(NAME edit_and_caret COMMAND heditor_edit_tests)

    # 文档模型：节点、Pos 与流位置的双向换算（0.1）、段落分段（0.2）、
    # 不变量检查器（0.3）、编译到引擎输入（0.4）。
    # 链 heditor_demo：0.4 的判据是"编译结果与现有 BlockSpec 路径逐字段一致"，
    # 参照物就是演示文书那条路径（sampleDocument），不能自造一份对照。
    add_executable(heditor_model_tests tests/model_test.cpp)
    target_link_libraries(heditor_model_tests PRIVATE heditor_core heditor_demo)
    heditor_target_flags(heditor_model_tests)
    add_test(NAME doc_model COMMAND heditor_model_tests)

    # 诊断探针（**不进 ctest**）：打印 EditStats / EditorEngine 的尺寸与字段偏移。
    # 用途与"为什么没法自动判定"写在 tests/probe_engine_size.cpp 顶部。
    add_executable(heditor_abi_probe tests/probe_engine_size.cpp)
    target_link_libraries(heditor_abi_probe PRIVATE heditor_core)
    heditor_target_flags(heditor_abi_probe)
  endif()

endif()
