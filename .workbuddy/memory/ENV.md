# 环境与流程（HEditor）

> 2026-10-12 从 `MEMORY.md` 迁出：这里是**可查即用**的构建、验证、源码组织与依赖约定。
> `MEMORY.md` 只留常驻的判据、硬决策、迁移索引与陷阱。

## 文档集 / 参照 / 依赖
- `正式化架构与迁移路线.md` 管分层路线；`文档模型设计.md` 管数据结构语义，**冲突以它为准**（第十五节 0.x/1.x
  是第二套编号：1.7 表格 = 阶段 5、1.10 序列化 = 阶段 6）；`控件边界诊断与方案.md` 是历史取证，不再更新。
- ★ 模型文档反复被外部写回旧版：恢复 `out/文档模型设计.latest.md`，判据是文首行数与关键标记；改长文档**串行**，
  大改前先 `cp` 到 `out/`。
- 参照 **DCWriterLite**（`D://Develop//DCWriterLite`）：**GPLv3**，代码不能进闭源产品，只当需求参照。增量排版
  与可证伪判据是我们独有的。
- 依赖归路按三问定（参与 wasm 构建 / 需一起改宏 / 自带非 CMake）。**不用 vcpkg**；HarfBuzz 走 submodule
  （钉 tag 14.6.0）；FreeType 零引用；Skia 走脚本化归档、没钉 commit；★ **头文件库走脚本化取回**：
  `third_party/nlohmann/` 由 `scripts/deps_init.ps1 -OnlyJson` 取回并校验 SHA-256（不用 git，智能体可跑）。

## 源码组织
- `src/core/`：`text`/`render`/`layout`/`doc`/`engine`；`utils` 放跨层共用纯工具；缺的层**不预建空目录**。
- 模型层在 `src/core/doc/`（节点·流·段落·不变量·编译·区间与步骤·事务·编辑模式与编辑能力·控件·校验·样式·
  表格·域·节·修订·序列化）；`Emu` 与换算在 `src/core/utils/units.h`。★ **`edit_policy.h/.cpp` 已退役**
  （2026-10-13）——模式是 `edit_mode.h` 的 `EditMode`、判定是 `edit_capability.h` 的
  `canEditAt`/`canEditRange`；引擎自己持 `mode_` 字段，不再有 `policy_` 对象。
- `src/core/engine/`：`engine.h` + `engine_config.h` + `engine_stats.h` + 5 个 .cpp。★ 两阶段拆分：**先拆文件、
  后拆类**。`demo/`（`heditor_demo`）放样例文书；`third_party/` 只有 `harfbuzz`。

## 构建与验证（本机）
- ★ **构建入口是标准 CMake 预设**，不再是包装脚本。`CMakePresets.json` 入库且不含任何本机
  路径；MSVC 环境由 `.\scripts\msvc_env.ps1` 进 —— 它用 vswhere 找实例、调微软
  自己的 `Enter-VsDevShell`（`Common7\Tools\Microsoft.VisualStudio.DevShell.dll`），跟开始
  菜单的「Developer PowerShell for VS 2022」是同一件事。
  ★ 别再从 `cl` 的横幅里读版本（2026-10-30 修）：中文 VS 的 `cl` 把横幅写 **stderr**、
  把「用法:」写 **stdout**，两股流合并顺序不保证 ⇒ 取「第一行」会时而拿到「用法: cl [...]」，
  症状是同一台 shell 里第一次进环境成功、第二次抛「cl 的输出不认识」。版本取
  `$env:VCToolsVersion`（工具集目录名，如 `14.44.35207`，与语言无关）。
  ★ 且 `cl` 那一下探针必须放进降过 EAP 的子作用域：Windows PowerShell 5.1 把原生命令的
  stderr 当错误记录（`2>&1` 与 `*>` 都一样），配 `Stop` 直接中止，`powershell -File` 一路必挂。
  **`CMakeUserPresets.json` 不再使用**（文件已删，`.gitignore` 里那条留着，防止旧副本被提交回来）。
  `cmake --preset native && cmake --build --preset native`（只要编译）、
  `cmake --workflow --preset native`（配置 + 构建 + 跑三个断言套件）、
  `cmake --workflow --preset wasm`。★ 2026-10-13 之前那四个包装脚本
  （`build_native.ps1` / `build_wasm.ps1` / `_msvc_env.ps1` / `verify_header_deps.ps1`）**已全部删除**，
  别再找它们；`cmake --list-presets` 可查全部预设名。
- ★ 改 C++ 后 **native → wasm → `scripts\verify_web.ps1`**，三后端都要过。
- ★ 看日志里的 **"共 N 项断言，失败 0 项"** 与 `exit=`，不看外层命令退出码；PowerShell 工具**吞 stdout** ⇒
  一律 `*>&1 | Out-File -Encoding utf8 <日志>` 再 Read；核对产物时间戳。★ **不先修编码日志就是 mojibake**
  （原生程序写 UTF-8、外壳按 CP936 解回再写成 UTF-8，且有损 ⇒ 搜 `共`/`失败` 一律搜不到）⇒ 先设
  `[Console]::OutputEncoding = [System.Text.Encoding]::UTF8`。★ 但这只动**日志的显示**，别顺手去改控制台的
  活动代码页 —— 那会把构建期一起拖下水（下一条）。
- ★ **头依赖走的是标准通道，本来就是好的**（2026-10-13 的最终结论）：MSVC + Ninja 下 CMake 用 cl 的
  `/showIncludes` 记依赖，不需要自己加任何东西。前缀是**本地化**的（`注意: 包含文件: `），**字节跟随控制台
  输出代码页**；CMake 用同一码页解释并写进 `build/<预设>/CMakeFiles/rules.ninja` 的 `msvc_deps_prefix`，
  ninja 逐字节匹配 ⇒ **configure 与 build 同码页才记**。936 / 65001 两档都能工作（实测都记上），
  **不一致时一条都不记、而且不报错**。本机默认 936：`grep -a msvc_deps_prefix
  build/native/CMakeFiles/rules.ninja` 该是 GBK 的 `d7 a2`。
  ★ 结论：**不要在构建脚本里切代码页**（曾经的"钉 65001 + 跑完还原"是拿项目文件给沙箱限制买单，已撤）。
  ★ 换码页后光重配没用：CMake 把编译器探测结果缓存在 `build/<预设>/CMakeFiles/<版本>/CMakeCXXCompiler.cmake`，
  要删构建目录重配。★ 自查依赖记没记上：`ninja -C build/native -t deps`（2026-10-13 实测 45 个 obj 全有
  记录、0 个 `#deps 0`；`harfbuzz.cc.obj` 328 条）。★ **Emscripten 侧不走这条路**：clang 用 `-MD` depfile，
  wasm 的 `rules.ninja` 里根本没有 `msvc_deps_prefix`，码页与 wasm 无关。
- ★ **旧的那个依赖守卫脚本已删**（`scripts/verify_header_deps.ps1`，2026-10-13）：它守的是
  `OBJECT_DEPENDS` 那个我自己挂上去的粗粒度余量，而余量撤掉后真通道本来就是准的 —— 标准 CMake+Ninja
  工程不需要这种东西（别的项目也没有）。要复现"改 `engine.h` 后该重编谁"：`ninja -C build/native -t deps`
  看依赖表 + 一次真构建即可（实测重编的正好是 include 它的那 9 个 obj，`heditor_demo` /
  `heditor_model_tests` 的 obj 一次都没被碰）。
- ★ 静态服务走 `scripts/dev_server.py`（发 `no-store`）；浏览器探针 `scripts/verify_web.ps1`（**必须用
  PowerShell 工具，Bash 里调 powershell 会被安全策略直接拦**）。★ 新加 target 必须调
  `heditor_target_flags(<tgt>)`（下发 `/utf-8`）；新加 .cpp 要加进 CMakeLists 的 `heditor_core` 源列表。
- ★ **性能诊断入口 `scripts/diag_perf.mjs`**（2026-10-13 新建）：一次跑完规模梯度（200/1000/3000 段）、
  演示场景 6/7、三个落点（首/中/末段）、光标闪烁一拍、**连敲帧间隔分位 + 单拍操作分位 + 宿主收尾**、
  **画布可见 vs 不可见对照**、**扫 zoom 的"合成代价 vs 画布面积"曲线**、
  **连续两次回车各自重画了多少像素**（标脏类的 bug 只有这里看得见）、**连按 30 次回车**（用户那个动作）。
  ★ **跨轮比绝对耗时不可信**：同一个 build 连跑，场景 7 的装载分别是 171 / 245 / 331 ms
  （装载路径一个字都没改）—— 判"省了多少"要读同一次操作内部的分项，或同轮内的对照组。
  它读四处账：`__heditor.editStats`（引擎：layout/pageFp/paint/脏页/重排段/窗口页）、
  `__heditor.stats`（绘制三段 paintFillMs/paintGlyphMs/paintPresentMs）、
  `__heditor.blitStats`（宿主上屏四段 + `windowMs`/`settleMs`，见 `app.js` 的 `lastBlit`）、
  `__heditor.viewport`（宿主与引擎各自认为窗口是哪几页）。
  ★ **帧间隔（rAF 间隔）才是"手感"**，它包含浏览器合成那一段；引擎计时与 `putImageData` 都测不到合成
  —— `putImageData` 是**惰性**的，对 73 Mpx 画布与 1 Mpx 画布都返回 0.000 ms，别拿它判画布大小的影响。
  ★ 起 Chrome 要按 `verify_web.ps1` 那套参数（`--headless=new` + 独立 `--user-data-dir` + `--disable-gpu`）。
- ★ **视口窗口的取证入口 `scripts/diag_window_diff.mjs`**（2026-10-13 新建）：打印 `selfCheck` 的
  差异包围盒、窗口首页/页数、宿主画布清单与位置、脏页清单、光标页 —— 查"屏幕 ≠ 引擎帧"时先跑它，
  一眼看出是哪一页、哪一段没贴上。
- ★ **宿主侧画布结构**：`#stage` 滚动容器 + `#pages`（撑出全文高度）+ 窗口内每页一张 `<canvas>`。
  引擎的帧是**视口窗口**（`setViewportAnchor`），不是整篇 —— 见 `memory/MIGRATION.md`
  "呈现的粒度"一节与 `TRAPS.md` 第 20、21 条。
- ★ 性能判据比 `EditStats::dirty_page_count`（确定性）：整篇 17 页、敲 1 字 1 页、平移复用 10 页。"某路径少做了事"
  的断言落在**做了多少**（页数/段数/像素）上，不落在**花了多久**上。
- ★ clang-format 入口 `scripts/format.ps1`（VS 自带 19.1.5）。**全仓并非全绿**（历史漂移，model_test 约 90 行）
  ⇒ **不要整仓格式化**，只保证新写/改到的文件合规，测试文件用 `clang-format -i -lines=a:b` 只格式化新区。
  ★ `.clang-format-ignore` 排除 out/ ⇒ 格式探针**必须放 `src/` 或 `tests/` 下**。★ 新建 .ps1 必须带 **UTF-8 BOM**。
  ★ **`-Paths` 是数组，别用 `-File` 传**：`powershell -File format.ps1 -Paths a,b,c` 会把三个路径压成一个
  字符串，脚本只"检查 1 个文件"并静默报合规（还带一句 `no such file or directory`）—— 必须
  `powershell -ExecutionPolicy Bypass -Command "& '.\scripts\format.ps1' -Check -Paths @('a','b')"`。
  ★ **只格式化新区**的工具：`out/_fmt_ranges.py`（`git diff -U0` 的新增行 ∩ clang-format XML 的
  offset→行号；XML 的 offset 是**字节**偏移、且用单引号）。

## 构建成本（本机实测，2026-10-13）

本机：**Intel i7-8550U**（4 核 8 线程、1.8 GHz 酷睿 U 低压）、16 GB。**别拿台式机的直觉估这里的时间。**

- ★ **构建是增量的**：无改动时 `ninja` 报 **0 个计划步骤、0.8 s**。不是每次全量。
- **改一个 .cpp**：约 **13 s**（含 6 个链接产物）。
- ★ **改一个头文件只重编真正 include 它的 TU**（`OBJECT_DEPENDS` 那层粗粒度余量 2026-10-13 已撤）。
  实测 `engine.h` → **9 个 obj**（`document.h` 17、`compile.h` 11、`edit_capability.h` 13、`node.h` 31、
  `char_format.h` 32、`units.h` 36 —— 从 `ninja -t deps` 反查）。★ 撤余量之前是**全部 37 个 TU、~380 s**，
  那 380 s 是"配置一错就静默坏"的保险费，现在不交了：判据是 `ninja -t deps` 能看到具体头。
- **`HEDITOR_BUILD_STAMP` 每次配置都变**（`string(TIMESTAMP)`，可用环境变量或 `-D` 覆盖）⇒
  `native_main.cpp` / `wasm_main.cpp` **每次构建都重编**，wasm 侧还要多跑一次 emcc 链接。这是
  "页面跑的到底是哪一版当场可判"有意换来的，别改成缓存持久（那会让时间戳停在首次配置）。
- **`cmake --workflow --preset wasm` 稳态 98 s**：只有 2 个构建步骤（`wasm_main.cpp.o` + emcc 链接），
  时间几乎全在链接上。
- **`cmake --workflow --preset native` 稳态 81 s**（配置 2 s + 构建 2 步 + 套件 60.6 s，另加
  `cmake --build` 自己的 `GLOB CONFIGURE_DEPENDS` 预检查）。构建那 2 步恒定是
  `native_main.cpp.obj` 重编 + 链接（由上面那条时间戳决定）；`tests/probe_engine_size.cpp` 只在
  自己源码变了时才编 —— `heditor_abi_probe` 不吃那个宏。测试预设带 `verbosity: verbose` ⇒ 日志约
  1800 行，但**三个"共 N 项断言，失败 0 项"都在里面**（94 / 858 / 809）。
  ★ 套件耗时抖动大：`heditor_edit_tests` 实测 30 s（首次）与 57 s（日志被 verbose 捕获那次）。
- ★ **切换构建入口那一次会全量重编**（旧 `build/wasm` 是旧脚本配的，命令行变了）：实测 wasm 首次 205 s
  （15 个 obj，含 69 个 .cc 聚合成一个 TU 的 `harfbuzz.cc.obj`），之后回到 98 s。
- ★ **并行跑两个后端会互相拖慢**（都吃满这 8 个逻辑核）：实测 native 独占 54 s，与 wasm 同时跑时涨到
  **6 分 34 秒**（约 4~7 倍）。两个预分别同时跑。

## 硬性约束
- **不执行任何 git 写入命令**（只读 status/log/diff/show/rev-parse/ls-files/stash list 可用）；不动 `.git`。
- `.workbuddy/memory/` 只追加（`MEMORY.md` 例外：就地整理，改前先备份到 `out/_MEMORY.md.bak-<日期>`）；
  `out/` 下带 `_` 前缀的取证与备份文件不删除。
- 后台 PowerShell 偶发无输出失败 ⇒ 长命令前台长超时 + 重定向到文件。
- 删除受 safe-delete 守卫管辖：**回执不可信，判据永远是复核**（`Remove-Item` 报错但文件可能真的没了；
  多路径一次删也可能只删掉第一个，要逐个删 + `ls` 复核）。

---

## wasm 的内存与"同页多实例"（2026-10-14 实测）

**三层边界**（一个页面里谁是谁）：

| 层 | 位置 | 是否单例 |
| --- | --- | --- |
| `window.__heditor` | `web/app.js:1092` | 演示页的挂载点，产品里可以完全不挂 |
| 宿主层 | `web/app.js`（1564 行） | **单例写法**：顶层 14 个状态变量（`mod`/`ready`/`busy`/`caretOn`/`blinkTimer`/`pageCanvases`…）+ 顶层抓死的 DOM（`$('stage')`/`$('pages')`/`$('ime')`） |
| 引擎层 | `src/wasm/wasm_main.cpp` 的 `g_engine`/`g_cfg`（70 个导出函数都操作它） | 模块级全局，**但每个 wasm 实例一份** |

**引擎层已天然支持多实例**：构建用了 `-sMODULARIZE=1 -sEXPORT_NAME=CreateHEditor`，
每次 `CreateHEditor()` 会实例化一份独立的 wasm 模块（独立线性内存）。实测两份：
`same_module_object:false`、各自装载互不覆盖、配置互不影响（一个 `undoLimit=7` 另一个仍 100）。
**C++ 侧一行都不用改**。

**资源账**（诊断脚本 `out/_diag_multi.mjs`，跑法 `verify_web.ps1 -Probe out\_diag_multi.mjs`）：

| 指标 | 实测 |
| --- | --- |
| 实例化耗时 | 61 ~ 66 ms（第二次不比第一次快） |
| 每实例内存 @ `INITIAL_MEMORY=64MB` | 64 MB（从不增长） |
| 每实例内存 @ `INITIAL_MEMORY=16MB` | **36.3 MB**（灌完 16.9MB 中文字体；期间增长两次） |
| 两实例同时装载 + 渲染 | 正常，各 17 码点、各 1191×1684 帧 |
| 增长之后 `HEAPU8` 视图 | 自动更新（emscripten 处理正确，不是坑） |

`-sINITIAL_MEMORY` 已从 **64MB 改为 16MB**（浏览器探针 256 项全绿）：它是**起步值**、
会按需增长，调低能让多实例更省，单实例也受益。

**要压内存先压字体**：中文字体 `NotoSansSC-VF.ttf` 是 **16.9 MB**，占每实例内存的 47%，
而且是每实例一份（字节要 `allocBuffer` + `HEAPU8.set` 拷进各自堆）。字体子集化（常用字
子集约 1~3 MB）比调 `INITIAL_MEMORY` 有效得多。

**待办**：把 `web/app.js` 重构成 `createEditor(container, options)` 工厂（状态收进闭包、
DOM 在容器内建、演示页仍挂 `window.__heditor` 保持探针不变）。C++ 侧不需要改动。

### 大文档（2000 段 × 20 字）的内存账 —— 以及一个 20 倍的坑（2026-10-14 实测）

| 路径 | 帧尺寸 | 帧面积 | 每实例堆 | 装载 + 首次排版 |
| --- | --- | --- | --- | --- |
| **不设视口**（引擎产出整篇帧） | 1191 × 109540 | **130.46 Mpx** | **1031.6 MB** | 1583 ms |
| **窗口 3 页**（宿主正常路径） | 1191 × 5108 | **6.08 Mpx** | **82.6 MB** | **115 ms** |
| 窗口化后滚到第 30 页 | 1191 × 5108（不变） | 6.08 Mpx | 82.6 MB（**不涨**） | — |
| 两个实例都按宿主路径装 2000 段 | — | — | **165.2 MB（线性叠加）** | — |

拆开看每实例 82.6 MB 的构成：字体 16.9 + （引擎底 + 排版结构 + 编译产物）约 41.4 + 窗口帧 24.3
（6.08 Mpx × 4 字节）。同期"只灌字体、不装文档"是 36.3 MB，所以**2000 段文档本身约 46 MB**。

★ **这就是那条 20 倍的坑**：整篇帧与窗口帧是同一份文档、同一个引擎，只差宿主有没有调
`setViewportAnchor(first, count)`。**默认是"整篇"** —— 单页文档看不出问题，多页大文档直接把
一个实例撑到 1 GB（而且绘制 130 Mpx 要多花 1.4 秒）。宿主 `app.js` 在 boot 时设了
（`setViewportAnchor(0, MAX_WINDOW=3)`，见 app.js:270，滚动时按 `w.first` 更新）——
**产品宿主自己写的时候极容易漏掉这一步**，漏了不会报错，只会内存与耗时同时爆掉。

**结论（多实例 × 大文档）**：按宿主路径走，每实例约 82.6 MB，两个实例 165 MB，线性叠加；
窗口化之后滚页不增长。真正要盯的是①字体（16.9 MB/实例，可子集化）②视口必须设。

### 参照项目 DCWriterLite 的字体与实例模型（2026-10-14 查证）

代码位置：`D:/Develop/DCWriterLite`（.NET 9 / Blazor WASM，GPLv3，只作思想参照）。
关键文件：`wwwroot/_framework/PageContentDrawer.js`（绘制）、`DCTools20221228.js`（度量）、
`Source/DCSoft.Writer.Core/Controls/WriterControl.cs`（控件）。

**它的字体路线：字不在 wasm 里，在浏览器里，而且按需下载、全页共享。**
`PageContentDrawer.EnsureFontInstall(fontNames, element)` 是全部机制：

1. 只看"这份文档用到的字体名"（入参 `fontNames`）—— 不在文档里的字体一个字都不加载；
2. 从全局表 `window.__DCFontSourceList`（字体名 → URL）查地址，**没配 URL 的就当系统字体用**，
   不下载（所以项目里一个字体文件都没有，`find -iname "*.ttf"` 为空）；
3. 配了 URL 的：先遍历 `document.fonts` 看有没有，没有才 `new FontFace(name, 'url("…")')` +
   `document.fonts.add(ff)` + `ff.load()` —— **注册进浏览器进程级的字体集**；
4. `ff.load().then(...)` 里投一个 `WriterControl_Task`（`TypeName: "FontLoaded"`）触发重排重绘
   —— 异步补加载完成后要重画，否则先用回退字体画的那一版就被当成定稿了。

度量与绘制都走浏览器：`measureText`（带 `charWidthsCache`，按 fontFamily + style/weight 缓存
字宽，出自 Chart.js 的度量模块）、`fillText`。字体探测（枚举本机可用字体给下拉框用）用经典的
span 宽度对比法（`createSpanWithFonts(fontToDetect, baseFont)` 遍历 `baseFonts`）。

**它的实例模型：单 runtime + 多对象**。Blazor WASM 只有一个 .NET 实例（框架形态决定的），
多个编辑器是同一个 runtime 里的多个 `WriterControl`（`: System.Windows.Forms.Control`）对象；
`private static int _CtlInstanceCounter` 是实例计数器。于是"同页很多实例"的内存 =
N 份文档 DOM + N 份画布，**共享一份 runtime、共享一份浏览器字体**。

**与本项目的差别（路线选择，不是谁对谁错）**：

| | DCWriterLite | 本项目 |
| --- | --- | --- |
| 字体存放 | `document.fonts`（浏览器，进程级共享） | wasm 线性内存（每实例一份） |
| 字体加载 | 按文档用到的名字，按需异步下载 | 宿主灌字节；demo 现在无条件装 3 个（20 MB） |
| 缺字体 | 回退系统字体 + 异步补加载后重绘 | 无回退、无补加载（装完才开始） |
| 度量/绘制 | Canvas measureText / fillText | Skia + FreeType（wasm 内自绘） |
| 排版确定性 | 依赖终端环境与系统字体 | 逐字节确定（"关缓存逐字节等价"判据的前提） |
| 实例 | 单 runtime + 多对象（天然共享） | 每实例一份 wasm（方案 A）；或句柄池（方案 B） |

★ **换它那套等于放弃确定性排版**：度量交给浏览器就没法保证"同一文档在任何环境逐像素相同"，
竖排/蒙文连写这类要自己 shaping 的也做不了。**能学的是三条**（都不动渲染路线）：
① 按文档实际用到的字体加载（现在 demo 无条件装 3 个，产品应改为按声明加载）；
② 字体子集化（16.9 MB → 1~3 MB，收益最大）；③ 若将来支持"文档声明字体、按需拉取"，
要配套异步补加载 + 加载完成重排（对标它的 `FontLoaded` 任务）。

**实例数量决定架构**：个位数实例 → 方案 A（现状可用、C++ 零改动）；十几二十个 →
方案 B（一份 wasm 装 N 个引擎，字体也能共享，代价是 70 个导出函数加句柄参数）。

### 字体：共享 vs 子集化（2026-10-14 定）

**共享一份字体，方案 A 做不到、方案 B 能做到 —— 而且比想象的小。**
方案 A（每实例一份 wasm）下不行：wasm 线性内存互不可见，`addFaceFromMemory` 必然把字节拷进
各自堆；何况 `FontLibrary fonts_` 现在是引擎的**成员变量**（值语义，`engine.h:906`）。
方案 B 下可以：把持有方式从"成员值"改成"指向一份共享库"即可 ——
★ 关键在于**排版层本来就是按传入的库工作**：`itemizeByFont(FontLibrary& lib, cps, count, chain, size)`
收的就是引用，所以共享字体**不用动排版层**，只改引擎的持有方式。

**子集化对可编辑文档有真实的缺字风险**（用户指出）：文档是动态的，用户随时可能输入子集外的字。
现状已有的两层保护：
① **回退链**（`src/core/text/fallback.h`）：`itemizeByFont` 用 `hb_font_get_nominal_glyph` 在整形
之前按 cmap 精确判断"chain 里第一个能提供字形的字体"，`chain[0]` 是主字体；`needsOwnGlyph` 保证
组合标记/变体选择符跟随基字（蒙文 FVS 尤其关键）。这段的注释自己就举了"NotoSansSC 有 ¹²³ 却没有
U+2070"的例子 —— 它本来就是为"单字体覆盖不全"设计的。
② **可证伪的缺字指标** `missing_glyph_count`：跑完整条回退链之后仍是 `.notdef` 的字形数
（`layout.h:513`），已暴露为 `statsJson.missingGlyphCount`，探针里有判据。

**待补的一处能力**：缺字现在只有**计数**，没有"缺哪些码点"的清单 ——
要做"缺字驱动补加载"（对标 DCWriter 的 `FontLoaded` 任务）必须先把它暴露出来。

**取舍（按场景定，不再无条件推荐子集化）**：

| 场景 | 该用哪条 |
| --- | --- |
| 只读 / 归档 / 打印（内容封闭，字符集已知） | 子集化（16.9 MB → 1~3 MB），缺字风险可控 |
| 可编辑文档 + 实例数是**个位数** | 方案 A + **按文档实际用到的字体集加载**（不做子集化） |
| 可编辑文档 + **十几二十个实例** | **方案 B 共享字体** —— 比子集化更彻底，且**没有缺字问题** |
| 将来要"文档声明字体、按需拉取" | 分包 + 缺字驱动补加载 + 加载完成重排 |

### 多实例"能共用 / 能借"的实测账（2026-10-14，诊断 `out/_diag_share.mjs`）

| 想共享的东西 | 行不行 | 实测 |
| --- | --- | --- |
| wasm 字节预取一次、多实例复用（`wasmBinary`） | **行** | 每实例实例化 **85 → 33 ms**（plain 74/99 ms 对 36/31 ms） |
| 共享 `WebAssembly.Module`（`instantiateWasm` 钩子） | **不行** | 两次尝试均失败：一次 embind 冲突，一次回调不被调用而挂起（20s 超时） |
| 字体字节：JS 侧一份 → 给所有实例 `addFaceFromMemory` | 行，但只省网络 | 每实例仍要 memcpy 16.9 MB 进**各自的堆**（5 ms 拷贝 + 7 ms 解析），内存不共享 |
| **借用浏览器已加载字体的内存** | **原理上不行** | 见下 |
| 真正共享字体 | 只有方案 B（多引擎在同一 wasm 实例内，共用一份 `FontLibrary`） | — |

★ **为什么借不到浏览器的字体内存**（三条，都是结构性的）：
① 浏览器为渲染加载的字体活在浏览器进程的字体引擎里，**不通过 JS 暴露字节** —— `FontFace` 只给
`status` 与 `load()`，`document.fonts` 也拿不到 `ArrayBuffer`；
② wasm 是沙箱，**只能读写自己的线性内存**，没有"指向浏览器字体内存的指针"这种东西 ——
不是缺 API，是内存模型不共享；
③ 我们能把字体灌进去，靠的是宿主 `fetch` 到**网络/磁盘上的字节**再拷进 wasm；
浏览器自己那份（供 CSS/Canvas 渲染）是另一份独立拷贝。
⇒ **除非改走浏览器渲染路线（放弃自绘与确定性排版），否则字体只能在 wasm 堆里。**

★ **另一个必须记住的坑**：`CreateHEditor(arg)` 的**参数对象会被消费**（它就是 emscripten 的 Module，
工厂把状态写回去）。两次实例化复用同一个对象 ⇒ 第二次直接抛
`BindingError: Cannot register multiple overloads of a function with the same number of arguments`。
每次都新建对象才行。`web/app.js` 现在是字面量写法，天然满足；多实例宿主必须自己守住这条。
（这个坑在诊断里先被误读成"共享 module 不行"，实际是参数复用 —— 两件事分开测才看清。）

### 补测：按需落地之后，"共用"能到什么程度（2026-10-14，`out/_diag_fontshare.mjs`）

- **同一个 `Uint8Array` 交给两个实例的 MEMFS（`canOwn=true`）是零拷贝的**：实测两边 FS 节点持有的
  就是同一个 ArrayBuffer（`same_buffer: true`）。`canOwn=true` 时 Emscripten 直接持有、不拷贝
  ⇒ **字体字节本体可以"JS 侧一份、多实例共用"**。
- **按需装载后每实例 wasm 堆停在 16 MB**（骨架 0.4 + 块缓存 ≤3.3，连 `INITIAL_MEMORY` 都没撑破）；
  整份装载的对照实例是 **36.3 MB**。三个实例（2 按需 + 1 整份）合计 68.3 MB，三个都整份是 108.9 MB。
- ⇒ 方案 A 与方案 B 的内存账重算：**A 只比 B 多 3.7 MB/实例**（那是"必须各一份的工作集"），
  而 B 要付 70 个导出函数加句柄 + 分区 arena + 诊断退化。⇒ 方案 A 配按需基本追平 B。

### DCWriter 的实例模型：**一个** wasm 堆，不是每实例一个（2026-10-14 查证）

证据在 `Source/WriterControlForWASM_JavaScript.cs`：

```csharp
private static List<WeakReference<WriterControlForWASM>> _ControlInstances = new ...;

[JSInvokable]
public static DotNetObjectReference<WriterControlForWASM>
CreateWriterControlForWASM(string containerElementID) {
    var ctl = new WriterControlForWASM();          // ← 同一 runtime 里的普通对象
    ctl._ContainerElementID = containerElementID;  // ← 每实例绑一个 DOM 容器
    _ControlInstances.Add(new WeakReference<...>(ctl));
    return ctl._ReferenceAtJS;
}
```

Blazor WASM 只有一个 .NET runtime = **一个 wasm 实例 = 一块线性内存**；`WriterControlForWASM`
都是这块堆上的普通对象，用一个**静态弱引用表**登记，按 `containerElementID` 区分。
每实例独占的只有"文档 DOM 树 + 一块 DOM canvas"；runtime、程序集代码、字体（浏览器侧）、
静态缓存全部共享。⇒ **它就是本项目"方案 B"的形态**（一份 wasm 里 N 个引擎对象）。

★ **但它有一个我们不具备的前提**：.NET 是**内存安全**的（GC + 边界检查），一个实例出错是
抛异常；而我们的引擎是 C++ 裸指针，**越界写会静默破坏邻居** —— 在同一块堆里跑 N 个引擎，
这类 bug 极难定位（症状是"另一个编辑器的文档莫名其妙坏了"）。
所以比"方案 A / B"时必须把**隔离性**单独列一栏：

| | DCWriter（单堆） | 本项目方案 A（每实例一 wasm） | 本项目方案 B（单堆句柄池） |
| --- | --- | --- | --- |
| wasm 实例 / 线性内存 | 1 | N | 1 |
| 引擎代码与静态数据 | 1 份 | N 份（代码 V8 可共享，数据段各一份） | 1 份 |
| 字体 | 浏览器侧 1 份 | N × 16.9 MB | 1 份（共用 `FontLibrary`） |
| 每实例独占 | 文档 DOM + canvas | 文档树 + 编译产物 + 窗口位图 + 字形缓存 | 同左 |
| 崩溃影响面 | **全崩**（同一块内存） | 只崩自己 | 全崩 |
| 内存安全兜底 | 有（.NET 运行时） | — | **无**（C++ 裸指针） |

### 假设分析：换成浏览器 measureText + fillText 会废掉什么（2026-10-14）

换的不是"渲染后端"，而是**字形与像素的控制权**。逐层看：

| 层 | 换完之后 |
| --- | --- |
| 文档模型（树 / 流位置 / 控件 / 三模式 / 撤销 / 序列化） | ✅ **完整保留**（项目最大的一块：模型文档 2698 行、`model_test` 809 项） |
| 断行与行盒（算法与数据结构） | ⚠️ **框架保留，度量来源换掉**（`LineBox` / 簇表 / 断行规则还在，advance 改用浏览器给） |
| 增量**排版**（段级续排、后缀复用、脏段） | ✅ 保留（与度量来源无关，只与"某段重排了"有关） |
| shaping（HarfBuzz / 簇 / 连写 / 蒙文 FVS） | ❌ **作废**（`heditor_tests` shaping 55 项判据全部失去意义） |
| 光栅（Skia / 字形缓存 / `drawGlyphs` 批量入口） | ❌ **整个作废** |
| 增量**光栅**（行带重画、窗口位图、画布池） | ❌ 作废（那"回车 48.7 → 22.9 ms"的三笔钱） |
| 帧判据（逐字节、增量==从零重画、`auditFrame`） | ❌ **跨环境一致性没了**（同环境重复绘制仍可测，但"任何环境同一份像素"不成立） |
| native 宿主（单测与审计） | ⚠️ 只剩一半：能测模型与排版结构，**测不了渲染** |

**技术边界（为什么不能"只换一半"）**：`measureText` 只回答"这串文字多宽" —— 标准 `TextMetrics`
**不给字形级信息**（没有 glyph id、没有 per-cluster advance）。连写（fi）能拿到**宽度**，拿不到
"这是几个字形、怎么定位"，于是逐字形的光标定位与命中测试做不了；蒙文/阿拉伯的上下文变形同理；
Canvas 2D 也没有竖排文本的原生支持。逐字符调 `measureText` 还会破坏 kerning ——
要补偿得用 n-gram 差分（Chart.js 的 `_measureChar` 正是这么算 `kernedWidth` 的）。

**这是产品定位问题**，判据两条：① 要不要蒙文/复杂文字（要 → 必须自绘 shaping）；
② 要不要逐像素确定的归档/打印（要 → 必须自控光栅）。两条都不要（中文表单 + 屏幕编辑）时，
浏览器渲染路线的性价比高得多 —— DCWriter 就是这么做到企业级的。
折中方案（双后端：屏幕用浏览器、归档/打印走自绘）的代价是"两套渲染的版式一致性"，
那是 Word / Google Docs 都吃过亏的地方，不建议轻易碰。

### DCWriter 支持蒙文竖排吗？——不支持（2026-10-14 查证）

四条证据：
1. **代码里没有书写模式的概念**：`WritingMode` / `TextOrientation` / `writing-mode` 全 0 命中；
   唯一像方向的是 `RightToLeft`（段落方向，双向文本的 LTR/RTL），那是**横排**。
2. **没有任何旋转变换**：`RotateTransform` / `RotateAt` / `.Rotate(` 全 0 命中 —— 竖排必然要用它
   （逐字旋转 90°），一处都没有。
3. **文档与 README 里没有"竖排 / 文字方向 / Vertical"字样**（"vertical" 命中的全是
   `VerticalAlignStyle` 这类**垂直对齐**）。
4. **23 份 demo 病历模板全是横排**（入院记录 / 护理记录 / CT 报告 / 各种评分量表），
   没有一份蒙文；`mongol` 唯一出现处是**系统字体名列表里的 `Mongolian Baiti`**（字体下拉框用）。

★ **而且这不只是"它没做"—— 浏览器 Canvas 路线做蒙文本身就很困难（实测）**：
同一段蒙文 `ᠮᠣᠩᠭᠣᠯ ᠪᠢᠴᠢᠭ`（12 码点）用浏览器 Canvas 画三种（诊断 `out/_diag_mongolian.mjs`，
图 `out/web/mongolian-browser-canvas.png`）：

| 画法 | 结果 |
| --- | --- |
| ① 整串 `fillText` | 连写正确，`measureText` 总宽 **307 px** |
| ② 逐字 `fillText`（每个字符单独整形） | 每个字都是**孤立形式**，宽度合计只有 **82 px**（差 3.7 倍），完全不连 |
| ③ 逐字 `rotate 90°` 竖排 | 字能转，但**还是逐字孤立的**（继承 ② 的问题） |

**这是浏览器路线的根本矛盾**：要正确整形，必须把整串交给浏览器一次画；要逐字形定位
（光标 / 命中测试 / 选区），必须逐字形画 —— 而逐字形画时浏览器看不到上下文，蒙文必然散架。
⇒ 蒙文（尤其侧倒竖排）是这条路线拿不到的能力，不是"多花钱的地方"。
产品判断：要蒙文 ⇒ 浏览器路线出局；只做中文/英文横排 ⇒ 可行。

### Web 端蒙文编辑器的外部参照（2026-10-14 检索）

**最接近我们的：UNS Editor**（`github.com/natsag2000/unesun-sambara`）
Rust + **WebAssembly + cosmic-text（自定义分支）**，**Canvas 自绘** —— 与本项目同一条路线。
- 竖排：从上到下、列自左向右；**拉丁字符自动旋转 90°**
- Unicode：U+1800-U+18AF，**以及 U+11660-U+1167F（蒙古文补充区）**
- 功能：点击定位 / 拖选 / Shift 选区 / 剪贴板 / Ctrl+Alt+V 切横竖 / 字号行高颜色可配 / localStorage
- 依赖写成 `cosmic-text: path: ../cosmic-text` ⇒ **上游 cosmic-text 的蒙文支持不足，是作者自己改的**
  （这一条对我们有参照意义：说明"排版库自带蒙文"这件事现在还不成立）
- 有 Playwright e2e、PLUGIN_API.md、tests/e2e、texts/（测试语料）
- 意义：证明"wasm 内自绘 + 竖排蒙文"这条路线成立，不是只有我们能做

**生态索引：`AnmiTaliDev/awesome-mongol-script`** —— 但 **Text Editors 与 Web Tools 两类都是
"No entries yet"**（连 UNS Editor 都没被收录）⇒ **Web 端蒙文编辑器是公认的空白区**。
清单里值得摘的：
- `paiza` —— 跨平台竖写输入法，**共享 C++20 引擎** + Windows TSF / macOS InputMethodKit / Linux IBus
- `mongol-norm` —— **shape-aware Unicode 规范化器**："保证同一视觉词总是编码成同一个样子"
  （与"编码一致性"直接相关，值得细看）
- `mongol-bichig` —— Hudum 后缀注册表 + 经典罗马化 + Unicode 字符类
- `mongolian`（lianghai 梁海）—— 字体 / 键盘 / OpenType 的资源集
- `gege-converter` / `mongol_code` —— 西里尔↔传统蒙文转换
- 字体只有 `Noto Mongolian` 一个（我们在用）

**最深水的参照：Unimongol Studio**（`doc.furengete.com`，作者 Matt）
- 浏览器编辑器，自称优化到**百万字符**
- **MongolianOriginal** 字体：2555 字形，覆盖传统蒙文 / 满文 / 托忒 / 锡伯，**3500 行 OpenType GSUB**
- 立场激烈：认为 Unicode 蒙文区块**结构性错误**（用控制字符强制视觉结果），于是**自己组织编码**
  （"one code point to one form"），还自造了 titim（词首零辅音头）与一个不可断连接符
- 参照价值：将来要做"整词折行 / 不断词"时，它的编码层思路值得看

**国内路线与真实需求**：
- 学术：基于 **UEditor + CSS `writing-mode: vertical-lr`** 的蒙文在线编辑器（蒙医方剂数据挖掘系统）
  —— 靠 DOM 与 CSS，光标与选区交给浏览器
- 产业：内蒙古德力海（"蒙古文左竖式编辑器"、OpenType/AAT 渲染引擎、多文种跨平台办公套件）
- ★ **可直接当验收表的一条**（内蒙古日报社《中国蒙古语新闻网》年保采购文件原文）：
  "蒙古文版面展示和编辑需符合蒙古文使用习惯，即**必须竖排录入内容、内容标题按整词切分、
  不出现断词，整词折行**，文字内容可正序复制利用"，并要求支持 Edge / Chrome / Firefox / Opera / 360。

**本项目一处待核对**：`U+11660-U+1167F`（蒙古文补充区）—— UNS Editor 明确列了它，
而 `fallback.cpp` 只显式处理了 `U+180E` 与 `U+180B-180D`（FVS）。该区块多为标点符号，
默认路径可能已经够用，但值得用缺字判据核一次。

### UNS Editor 的字体加载与实例模型（2026-10-14 读源码实证）

来源：`src/lib.rs`、`src/editor_core/editor_state.rs`、`index.html`（已存 `out/_unes_index.html`）。

**① 字体加载 —— 与我们的做法完全一样**（`EditorState::new`，async）：

```rust
// Create empty font database and font system (no system fonts in WASM)
let db = fontdb::Database::new();
let mut font_system = FontSystem::new_with_locale_and_db("en-US".into(), db);
let noto_font = Self::load_font_file("fonts/NotoSans-Regular.ttf").await?;
font_system.db_mut().load_font_data(noto_font);
let mongolian_font = Self::load_font_file("fonts/NotoSansMongolian-Regular.ttf").await?;
font_system.db_mut().load_font_data(mongolian_font);
let baiti_font = Self::load_font_file("fonts/monbaiti.ttf").await?;      // Mongolian Baiti
font_system.db_mut().load_font_data(baiti_font);
let emoji_font = Self::load_font_file("fonts/NotoEmoji-Regular.ttf?v=2").await?;
font_system.db_mut().load_font_data(emoji_font);

async fn load_font_file(path: &str) -> Result<Vec<u8>, JsValue> {
    let resp_value = JsFuture::from(window.fetch_with_str(path)).await?;   // wasm 内自己 fetch
    let resp: Response = resp_value.dyn_into()?;
    let array_buffer = JsFuture::from(resp.array_buffer()?).await?;
    let uint8_array = js_sys::Uint8Array::new(&array_buffer);
    let mut font_data = vec![0u8; uint8_array.length() as usize];
    uint8_array.copy_to(&mut font_data);      // ← 拷进 wasm 堆（与我们的 alloc+HEAPU8.set 等价）
    Ok(font_data)
}
```

- **4 个字体无条件全量加载**（Noto Sans / Noto Sans Mongolian / Mongolian Baiti / Noto Emoji），
  没有任何按需机制 —— 与我们 demo 无条件装 3 个是同一个毛病；
- 注释原文：**"no system fonts in WASM"** ⇒ 再次印证"wasm 里没有系统字体，必须自带"；
- 小技巧可抄：字体 URL 带版本号（`NotoEmoji-Regular.ttf?v=2`）做缓存失效；
- 字体族引用用 `Family::Name(&settings.fonts.font_family)`，设置里的"字体名"是 **fontdb 里已注册的
  family 名**，不是 CSS 字体（页面里那个 `#font-preview` 的 CSS 渲染是另一回事）。

**② wasm 堆与实例：一块堆、一个编辑器、多标签是换文本**

- `index.html` 里 `await init({module_or_path})` **只出现一次** ⇒ **一个 wasm 实例 = 一块线性内存**；
- 注释原文（`TabManager` 上方）：
  > Tabs are JS-level state layered on top of **one shared `WasmEditor`, not one `WasmEditor` per tab**.
  > Only the active tab's text/cursor actually live in the WASM buffer; switching tabs snapshots the
  > outgoing tab's live state and loads the incoming one via `set_text`/`set_cursor_position`.
- 于是**切标签会清空撤销历史与查找状态**（`set_text` 在 Rust 侧调 `discard_history()`）——
  它自己写明这是 "an accepted trade-off, not an oversight"；
- `EditorState`（`font_system` + `cache` + `editor` + `history` + `plugins` …）是 per-`WasmEditor` 的，
  `lib.rs` 里**没有任何 static mut / thread_local / OnceCell** ⇒ 若开两个 `WasmEditor` 就是两套字体。

**③ 顺带定位出我们的相对位置**

| | UNS Editor | 本项目 |
| --- | --- | --- |
| 字体 | 4 个全量、per-instance、无按需 | 3 个全量、per-instance、无按需（**同**） |
| 文档模型 | **一个 buffer 换文本**；切标签丢撤销历史 | 每文档一棵真树；撤销栈随文档 |
| 呈现 | **整帧重建 + 整幅 `putImageData`**，只有 `dirty` 决定跳过空闲帧 | 行带重画 + 窗口位图 + 增量后缀复用 |
| 多实例 | **无**（一个 `WasmEditor`，多标签共用） | 有（每次 `CreateHEditor` 一块独立内存） |
| 排版 | cosmic-text（自定义分支加蒙文）+ harfrust + swash | 自建编译/排版 + HarfBuzz + Skia |
| 功能面 | 比我们全：词典、转写、插件体系、查找替换、主题、键位、词语建议 | 尚未做 |

⇒ **"同页多个独立编辑器"这块，三家里只有我们真做了**（DCWriter 是多对象共享一栈，UNS 是单实例
换文本）—— 这里没有现成参照可抄。

### Web 上"字体放硬盘 + 按需加载"有哪些机制（2026-10-14 检索）

**"放硬盘"有四层可选**：

| 机制 | 存在哪 | 特点 |
| --- | --- | --- |
| HTTP 磁盘缓存 | 浏览器自己的 cache 目录 | **自动的**；配 `Cache-Control: immutable` + 文件名带 hash 就基本不再发请求；但可能被逐出/被用户清 |
| CacheStorage（Service Worker） | 磁盘 | 可编程、可版本化、离线可用；受配额限制 |
| **OPFS**（Origin Private File System） | 磁盘（源私有） | **容量接近磁盘、性能接近原生**；Chrome 86+/Edge 86+/Firefox 111+/Safari 15.2+；`createSyncAccessHandle` 只能在 Worker 里用 |
| IndexedDB | 磁盘 | 老方案（EMSCRIPTEN 的 IDBFS 就是它），5~10 MB 量级、慢 |

**wasm 真的能"以文件方式"访问它** —— Emscripten 的 **WASMFS + OPFS 后端**（官方支持）：
链接加 `-sWASMFS -sFORCE_FILESYSTEM=1 -pthread -sPTHREAD_POOL_SIZE=N`，运行时
`wasmfs_create_opfs_backend()` + `wasmfs_create_directory("/dir", 0777, backend)` 挂载，
之后**标准 C++ 文件 I/O 直接可用**（`fopen` / `std::ofstream`）。
- 必须在 `emscripten_set_main_loop_arg` **之前**挂载（WASMFS 内部 promise 需要事件循环）
- WASMFS 内部占一个后台线程，`PTHREAD_POOL_SIZE` 要留余量
- 真实案例：Adobe Photoshop Web 用 OPFS 做本地文件访问；SQLite wasm 有官方 OPFS VFS；wasm-git 的 OPFS 版
- ★ **代价**：`-pthread` ⇒ 需要 `SharedArrayBuffer` ⇒ 部署必须发
  `Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy: require-corp`
  （COEP 会连带影响第三方资源的加载，是个不小的部署约束）

**"按需加载"要分两种，结论完全不同**：
- **按需下载（网络层）**：✓ 成熟。标准做法是 **`@font-face` + `unicode-range` 分包** ——
  Google Fonts 的 CJK 就是把两万多汉字切成上百个分片，浏览器只下载页面用到的那几片。
  ★ 但这套是**浏览器替 CSS 路线实现的**：DCWriter 那种走 CSS 字体的编辑器白拿这个红利；
  **我们走 wasm 自绘，拿不到** —— 分包要自己实现（下哪几包、缺字补拉）。
- **按需读进内存（内存层）**：⚠️ **省不掉**。FreeType/Skia 一旦用某个字体，它的表就会常驻内存；
  WASMFS+OPFS 省的是"启动时预载"而不是"常驻"，而且**每个 wasm 实例仍各读一份**。

**结论**：硬盘持久化是现成的（OPFS/CacheStorage/HTTP 缓存），"按需下载"要自己实现分包；
**但"多个实例共享一份字体内存"这件事，以上机制一个都帮不上** —— 那仍然只有方案 B（同实例多引擎）。

### 字体在内存里是怎么被读的，以及"从硬盘按需读"值不值（2026-10-14 查证）

**我们的持有链（已经是最省的形态，全程只有一份字节）**：

```
宿主 fetch 字节 → FontFace::data_ (std::vector<uint8_t>，整份常驻于 wasm 堆)
                → hb_blob_create(..., HB_MEMORY_MODE_READONLY)   ← 引用，不拷贝
                → hb_face_t                                        ← HarfBuzz 从这里随机读表
                → SkData::MakeWithProc(p, len, releaseHbBlob, blob) ← 引用，不拷贝
                → SkTypeface（按 face + 变体坐标指纹缓存）
```

`bitmap_skia.cpp:212` 起用 `SkData::MakeWithProc` 包住 hb_blob 的数据 —— **HarfBuzz 与 Skia 都是
引用同一份 `data_`，没有第二份拷贝**。所以问题不是"拷贝多"，是**"整份常驻"**。

**"改成从硬盘按需读"在 Skia 路线下走不通**（三条依据）：
1. `SkTypeface` 要求 `SkData` = **完整缓冲**；`makeFromData` 与 `makeFromStream(SkMemoryStream)`
   都是这个前提（后者只是把 SkData 包成流）；
2. 虽然 Skia 有 `SkFontMgr::makeFromStream(SkStreamAsset)`，内部实现是 `SkData::MakeFromStream`
   —— **全量读**；
3. FreeType 自己确实有 `FT_Stream`（可自定义 I/O，能按需读文件），但 **Skia 的 FreeType 后端不用它**。

就算硬做，也**不值**：字体的访问模式是**随机小读**（cmap 查询、逐字形取轮廓），在 wasm+OPFS 下
每次读都要跨 JS 边界（同步访问句柄还只能在 Worker 里）再拷进内存 —— 比整份常驻慢几个数量级。

**真正能"按需"的两条（都不跨边界）**：

① **减少常驻 —— 动态子集**：`hb-subset` 就在 `third_party/harfbuzz/src/`（`hb-subset.h` 等齐全），
   但**聚合源 `harfbuzz.cc` 不含它**（已 grep 确认），要用得单独编那几个 .cc。
   装载时按字符集生成子集（16.9 MB → 1~3 MB），打字缺字再增量补。

② **分片 + 按需装载/卸载 —— 这才是"按需内存"**：
   ★ **基础设施已经就位**：`FontLibrary` 天生多 face，而 `itemizeByFont` 是**按 cmap 精确选字体**的
   回退链 —— 分片天然就是"多个 face"，回退链天然会选到对的那一片，不用改排版层。
   缺的两件事：`FontLibrary` **没有 removeFace**（现在只能 add）、卸载时要清 `bitmap_skia.cpp` 里的
   `typefaces` 缓存与字形缓存。

| 方案 | 改动量 | 效果 |
| --- | --- | --- |
| 构建期子集化 | 小（换字体文件 + 改加载清单） | 16.9 → 1~3 MB，**收益最大** |
| 运行时动态子集 | 中（编 hb-subset + 换 face 流程） | 同上，且自适应任意文档 |
| 分片 + 卸载 | 中（removeFace + 缓存失效 + 构建期切片） | 常驻 = 当前用到的片（真按需） |
| 从硬盘按需读 | 大，且 **Skia 不支持** | 不做 |

### 构建期子集化：原理、实测缺字率与体积（2026-10-14 实测）

**原理**：字体文件是一堆表（cmap 码点→glyph、glyf/CFF 轮廓、hmtx 度量、GSUB/GPOS 整形规则、
fvar/gvar 变体）。体积大头是**轮廓**与**变体 delta**。子集化给定一个码点集合后：
cmap 只留这些码点并把 glyph id 重编号 → glyf/CFF 只留对应轮廓 → hmtx 只留对应度量 →
**递归闭包**（GSUB/GPOS 引用到的字形也要带上，否则连写/替代会散）→ **gvar 按字形裁剪** →
重写 loca/maxp/head 等索引。产出是**合法且功能完整**的独立字体。
工具：`pyftsubset`（fontTools，本机 4.59.1 可用）、`hb-subset`（HarfBuzz 官方，源码在
`third_party/harfbuzz/src/`，但**聚合源 harfbuzz.cc 不含它**）。

**实测（语料 = DCWriterLite 的 23 份真实病历 XML，共 13,480,703 字符）**：

| 项 | 数据 |
| --- | --- |
| 语料去重码点 | **仅 1060 个**（13.48 M 字符） |
| 原字体 | 17.77 MB |
| **GB2312 子集**（7554 码点） | **4.09 MB（23%）**，fvar/gvar/GSUB/GPOS **全保留**，wght 100–900 轴在 |
| 语料子集（1057 码点） | **0.57 MB（3.2%）**，同样表齐、轴在 |
| GB2312 子集的缺字 | **只漏 1 个**：`U+73FA 珺`（人名用字），314 次 / 13.48 M = **0.0023%** |
| 字体本身缺的 266,123 次 | 全是控制字符（`\n` 264k、`\t` 1.8k、BOM、DEL）——**不是真缺字** |

⇒ 构建期子集化在真实中文病历上是**安全**的；`pyftsubset` 默认就保留变体轴与 layout 表。

**输入了子集外的字会怎样（引擎现有行为，属"可发现的失败"）**：
1. 整形前 `itemizeByFont` 用 `hb_font_get_nominal_glyph` 查 cmap → 子集里没有 → **该码点不归它**；
2. 回退链继续找下一个能提供字形的 face；
3. 链走完都没有 → 落回 `chain[0]` 出 **.notdef（豆腐块）**；
4. `missing_glyph_count` +1 → `statsJson.missingGlyphCount` 可查。
⇒ 用户看见豆腐块，宿主能统计到，不会静默错。

**兜底方案（按代价排序）**：
- **A. 全量字体延迟加载**（最小改动）：平时装 GB2312 子集；`missingGlyphCount > 0` 时才拉全量。
  缺字瞬间内存 4 → 21 MB，但只在真遇到生僻字时发生。
- **B. 分包 + 缺字驱动补载**（更省）：子集外那部分再切几片（次常用/生僻/人名地名），只补拉缺的片。
  **前提**：引擎要暴露"**缺哪些码点**"（现在只有计数，是已记的待办），补载后重排。
- **C. 增量子集**（现场从全量抽）：做子集那一下要把整份字体读进内存，不划算。

---

### wasm 调试档（2026-10-14 加）

**跑法**：`cmake --workflow --preset wasm-debug` 构建到 `build/wasm-debug/`；
验证入口加 `-Wasm wasm-debug`（`scripts/edit_probe` 那一套的 PowerShell 包装脚本）。
宿主页用查询参数切换产物目录：`web/index.html?wasm=debug`（`app.js` 的 `WASM_DIR`）。

**内容**：编译期与链接期都 `-g`、链接期 `-gsource-map`、`-sASSERTIONS=1`、`-sSTACK_OVERFLOW_CHECK=1`。
用途是 trap（越界、非法指令、栈溢出）时能拿到可读的 C++ 栈与源码位置，而不是一句
"memory access out of bounds"。

**两个坑**：

① **新 preset 首次配置失败**（`Configuring incomplete, errors occurred!` 与 `failed with: unknown error`）：
   CMake 探测 ninja 时选了 PATH 里**第一个** `D:/Develop/depot_tools/ninja`，而**那个文件不存在**
   （坏链）。release 档不踩，是因为它的 `CMakeCache.txt` 里存着当初探测到的
   `D:\Develop\editor\.toolchain\ninja\ninja.exe`。
   ⇒ 新 preset 要显式 `"CMAKE_MAKE_PROGRAM": "${sourceDir}/.toolchain/ninja/ninja.exe"`。

② 星标 **`-g2` 不够**：编译命令确实带了 `-g2`（41/42 个 TU），但 `.wasm.map` 的 sources 仍是 318 个、
   **全是 emsdk 自带的**（libc++ / embind），本工程一个 .cpp 都不在。
   换 **`-g`** 之后 sources 473 个、**其中 63 个是本工程的**。
   ⇒ 要 source map 映射到自己的源码，编译期就得给 `-g`（`-g2` 只够 name section）。

**代价**：调试档 wasm **16456 KB**（交付档 3440 KB，DWARF 进了 wasm），且有警告
`running limited binaryen optimizations because DWARF info requested`（链接期优化受限）。
想压体积可试 `-gline-tables-only`（只留行号表）—— 未试。

**验证**：调试档跑浏览器探针 **268 项全绿**；交付档不受影响（重新构建后仍是 3440 KB 且无 `.map`）。
