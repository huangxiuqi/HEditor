# AGENTS.md

本仓库对智能体与人类协作者的约定。改代码前请先读完「注释」一节，本仓库的注释有硬性格式要求。

项目名 **HEditor**。命名一律用这套（改名前叫 ced，别再用旧名）：
C++ 命名空间 `heditor`；CMake 工程 `HEditor`、target 前缀 `heditor_`、变量前缀 `HEDITOR_`；
wasm 产物 `heditor.js` / `heditor.wasm`（emscripten 工厂函数 `CreateHEditor`）；
浏览器控制台 API `window.__heditor`。

文档里的名词按层组织（模型层 / 编译层 / 排版层 / 编辑层），层与层同名不同物是设计使然。
对照表见 `docs/名词对照.md` —— 分不清"码点 / 簇 / 流位置 / run"时先查它。

## 注释

### C / C++

函数、结构体、类、成员（含常量）的声明处使用块注释：

```cpp
/**
 * 把 [from_para, to_para) 段排入 out，并返回结束时的续排状态。
 *
 * out 中 from_para 段之前的内容原样保留，本函数只做追加。
 *
 * @param state 第 from_para 段开始时的续排状态；传错不崩溃，但结果会与全量排版不一致
 * @param out   结果缓冲，返回前其 stats 会按段重算为整篇的汇总量
 * @return 第 to_para 段开始时的续排状态，可直接作为下一次续排的入参
 */
ReflowState layoutDocumentRange(FontLibrary& lib, /* ... */ LayoutResult* out);
```

行内与局部说明使用行注释（函数体内部一律用它）：

```cpp
// 间隙必须计入 slot 宽度，否则右括号会贴到相邻的字上
const float slot = bw + kControlBracketSlotGapPt;
```

**分节标签也按声明处处理**：在类、结构体或命名空间里给一族成员分组时，不要写裸标签
（`// 编辑`），写成一句简短的块注释，说明这一族是干什么的：

```cpp
/**
 * 编辑入口：坐标是 (段, 段内码点偏移)，落地前吸附到字素簇边界。
 */
bool insertTextUtf8(int para, size_t offset, const std::string& utf8);
```

**块注释一律多行**：写成 `/** 说明 */` 这种「单行块注释」既不是规范的行注释、
也不是规范的块注释，两边的读者都会误判它的用途。单行说明用 `//`；
需要块注释的地方（声明处）展开成三行 —— `/**`、` * 说明`、` */`。

### JavaScript / TypeScript

顶层函数与导出对象的方法使用 JSDoc；函数内的局部变量与行内说明用 `//`。
同样禁用 markdown 记号与 emoji，同样不写单行块注释（`/** 说明 */` 要展开成三行）。
TypeScript 与 JS 同一套要求：`web/src/**/*.ts` 里的 JSDoc 会原样进 `dist/*.d.ts`，
写坏了直接给使用者看。

**方法必须标参数与返回值**：有参数就有 `@param 名字 一句话`，有返回值就有 `@returns`。
每条注释块与上一个成员之间空一行 —— 整段挨着写，读起来是一堵墙而不是一份清单。
接口与类型里的数据成员不强制逐条写（那层的说明在 `web/src/types.ts` 的契约里，
逐字段复述类型只会变成噪音）。

判据：`node scripts/check_docs.mjs`（在 `npm run check` 里）。它按 AST 判，不看措辞 ——
漏一个 `@param`、少一个空行、方法没有 JSDoc，都会报出文件与行号。

```js
/**
 * 把引擎帧贴到画布。
 * @param {Uint8ClampedArray} px 整帧 RGBA 像素
 * @param {number} w 帧宽（设备像素）
 * @returns {boolean} 是否真的写入了画布
 */
function blitFrame(px, w) {}
```

### 内容要求

- 只写「为什么」与「约束」，不复述代码在做什么；签名已能表达的不要写。
- 简洁。块注释通常一到四行，行内注释一行；超出就拆分或删减。
- 篇幅硬上限：普通块注释不超过 12 行，文件头不超过 20 行，注释行不超过 100 列。
- 禁用 markdown 记号：`**粗体**`、`## 标题`、`- 列表`、反引号包裹。
  引用代码符号时直接写名字。
- 禁用 emoji 与装饰符号：星号标记、对勾、警告符、`───`、`═══` 之类的分隔线。
- **不写编号列表**：不用 `①②③`，也不用行首的 `1)`、`(1)`、`一、`。
  分点写成文字句子，用分号或顿号串联；确需分点时让每点各自成句。
  例外：测试分节标题里的 `[8]`、`(b2)` 这类是交叉引用的锚点，保留。
- **不写口语与俚语**：坑、兜底、顺手、悄悄、搞混、白做、硬塞 之类，
  换成技术说法（「失败点」「保证」「顺带」「静默」「混淆」「无谓重画」）。
- 禁止变更历史与流水账：不写「原来……后来改成……」「第一版是这么写的」
  「实测命中过」，也不写谁在什么时候发现了什么。
  需要保留的教训写成结论句，例如：
  写「括号占位必须计入 advance，否则括号会压在正文上」，
  不写「以前这里忘了算占位，所以括号压字」。
- 行宽不超过 100 列。
- 新增代码一律遵守；改动已有文件时就近把该文件的注释改为合规写法。

## 格式化

代码格式由 clang-format 统一，配置在仓库根目录的 `.clang-format`：基准取 Google 家族风格
（WebRTC / Chromium / Skia 同系），仅在与既有代码冲突处显式覆盖并注明理由（4 空格缩进、
100 列、指针靠左、注释与字符串不重排、include 分组保持原样）。

- 就地格式化：`powershell -ExecutionPolicy Bypass -File scripts\format.ps1`
- 只检查（有差异则退出码 1，可用于提交前把关）：
  `powershell -ExecutionPolicy Bypass -File scripts\format.ps1 -Check`
- 交给别人评审前先跑一次格式化；`.clang-format-ignore` 已排除 third_party / .toolchain /
  build / out，脚本本身也只扫 src 与 tests。

## 构建与验证

- 源码依赖（HarfBuzz submodule，钉 tag + 稀疏检出只留 `src/`）：
  `powershell -ExecutionPolicy Bypass -File scripts\deps_init.ps1`
  —— 它会执行 git 写入命令，按下面的硬性约束只能由人运行。
- 依赖归路（源码走 submodule、Skia 走脚本化归档）与判断标准见 `docs/依赖与工具链管理.md`。
  本工程不使用包管理器：唯一的非源码依赖面（native 侧的成品库）目前是空集。
- **Skia 的版本是钉住的**，唯一来源 `scripts/skia_rev.ps1`（commit + 根 `BUILD.gn` 的
  blob 哈希）。源码与六个外部依赖由 `scripts/skia_fetch.ps1` 取，那是**唯一的取数入口**
  （只依赖 curl 与 tar，不需要 gn / ninja / emsdk）；`build_skia_wasm.ps1` 与
  `build_skia_native.ps1` 发现树不是这一版都会先调它。要换版本就改那三个常量、重取、
  再把两套库与全部断言重跑一遍。不要绕过它手工换一棵树 —— native 与 wasm 的库必须出自
  同一棵树，否则宏与实现会悄悄分叉且不报错。
- 预设写在 `CMakePresets.json`：`native` / `native-debug` / `wasm` / `wasm-debug`。
  两套都要 `.toolchain` 下的工具链（emsdk / gn / ninja），换机器先跑
  `powershell -ExecutionPolicy Bypass -File scripts\toolchain_init.ps1`：ninja 与 emsdk
  由它下载并校验，gn 只校验版本（那一份没有可下载的制品，见 `docs/依赖与工具链管理.md` 5.2）。
  想连 Skia 源码一起取：给同一个脚本加 `-WithSkia`（否则那步跳过，构建脚本会自己补）。
  `native` 还需要 MSVC 环境与 x64 版 Skia 静态库（`scripts\build_skia_native.ps1`，产物
  `.toolchain/skia/.../out/win-x64/skia.lib`；首次跑会先取那 80 MB 源码）。MSVC 环境由
  `.\scripts\msvc_env.ps1` 进：
  它用 vswhere 找到 VS 实例，再调微软自己的 `Enter-VsDevShell`
  （`Common7\Tools\Microsoft.VisualStudio.DevShell.dll`）—— 跟开始菜单那个
  「Developer PowerShell for VS 2022」是同一件事，环境由 VS 自己算，仓库里没有一个版本号目录。
  **不需要也不再有 `CMakeUserPresets.json`。**
- 原生：`cmake --workflow --preset native`（配置 + 构建 + 跑功能判据，四片并行）。
  只要构建 `cmake --build --preset native`；只要单测 `ctest --preset native`
  （四片并行，45~51 s）。耗时比值判据不在这轮里：改完排版或光栅单独跑
  `ctest --preset native-perf`，它带 RUN_SERIAL，跑的时候机器上不该有别的活。
- WASM：`cmake --workflow --preset wasm`（工具链取仓库内的 `.toolchain/emsdk`）。
- 原生渲染出图（验证台要用）：`build/native/heditor_native.exe --out-dir out`。
- 浏览器端测试（Playwright，TS）：`cd web && npm test`。默认跑 Vite 源码直连，
  改一行就能测，只有 Worker 壳需要预先构建（`pretest` 会做，不到一秒）；
  `npm run test:dist` 跑构建产物，那是验收基准。配置在 `web/playwright.config.ts`
  与 `web/tests/support/config.ts`，判据在 `web/tests/specs/`，性能判据在 `web/tests/perf/`。
  **trace 默认关**：它给每个 test 持续截图并抓 DOM 快照，实测让全部用例的耗时之和从
  180 s 涨到 298 s。失败时仍有截图、报错与附件，要回放就设 `HEDITOR_TEST_TRACE=1` 重跑。
  并行度默认 4（本机四个物理核）；六 worker 更慢，而且会把负载敏感的判据顶红。
  性能判据单独一条命令：`npm run test:perf`（源码直连）或 `npm run test:dist:perf`
  （构建产物，先跑过一次 `test:dist`），单 worker 独占。
  二十二份 spec 跑一遍：`demo` 5 个用例 / 47 条判据（含连页末尾的总高：可滚区跟着内容走、
  至少留一页）、`multi` 11、`saveload` 11、`input` 25、`diag` 39、
  `error` 19、`stress` 69 步差分审计、
  `edit-core` / `edit-form` / `edit-boundary` / `edit-sweep` / `edit-longdoc` /
  `edit-fonts` / `edit-selection` 七份合起来 372 条（编辑面按域拆开，各自一个页面：
  基准与键盘、表单控件、边界归属、边界总覆盖、长文档（含连页增量：中段回车之后后缀
  只让出一行、整篇块轴一路递增、增量与整篇重排逐字段一致）、字体按需读、选区与样式）、
  `clipboard` 11 条（复制粘贴的保真、
  剪切、跨实例、键盘路径、撑长之后的滚动与画布）、`format` 14 个用例 / 49 条判据（字符格式落不落到标记上、
  幂等、只改选区那一段、回显取共同值、整段改字重之后增量排版等于全量、**改完格式画面必须
  重画**（比截图）、敲字沿用落点**左边**那一段 —— 格式段左边界取左边、右边界取它自己，两条互为反向）、`insert` 35 条
  （插入表格、内容控件与图片：段数与表格结构、光标落进第一格 / 控件内容末尾、越界与模式被拒时
  文档不动、撤销与重做、**撤销之后帧指纹逐像素回到插入之前**、插进去那一刻屏幕上看得见；
  图片那两条钉住"像素注册 → 插入 → 命中把手 → 改尺寸 → 撤销"与"真实鼠标点图片只选中、
  不落光标"，改尺寸一次拖拽只留一步撤销也在这里验；控件那两条钉住单选组的取值与图形、
  引擎侧切换与"异步前置回调拒绝时不生效、放行后才落"，改定义（选项表重建内容、
  删掉选中项清空值、类型不可改、撤销把选项与值一起还原），以及插入之后光标落在整块之后
  与"贴着选择类控件退格删整块、不进去删字"；拖选整张表删除之后光标落回表格所在的那一处
  （不取选区起点：那一端在表格内部、随表消失，后面的内容会顶上来）；空控件的提示文字那条按像素判：空内容时那一段
  出现灰字（不是黑字）、换一句等宽的提示画面跟着变、点在提示上光标落进字段、填了字灰字消失、
  只读态不画）、`smoke` 3 条
  （工具栏每个按钮点一遍都要有反应 —— 含页面设置面板改纸张后页宽要跟着变、裁剪标记默认开；
  公开面每个方法调一遍形状与效果都对 —— 常用操作全走一遍，别等用户点了才发现；
  再加一条"点完按钮键盘还在编辑器里"）。`table` 6 条
  （跨页切片：第二页上点空格子、整格反白铺满每一页、格内折行、连页下逐行下推、
  空格的段落标记、切片之后每页位图自洽）。`menu` 9 个用例 / 43 条判据
  （右键菜单：格内右键认出表格、只剩一行一列时删行删列置灰、没有整格选区时合并置灰、
  菜单动作真的落到文档且一步撤销；控件右键出控件菜单、属性面板回填现状并改得动标签；
  方向键走项、Esc 与点外面关掉）。`fonts` 3 条
  （注册与选字体：清单里的名字来自字体文件的 name 表、重复注册不增脸、
  按族名 / PostScript 名 / 文件名都能选到、注册不重排、没注册的名字当场拒绝）。
  `props` 6 个用例 / 34 条判据（属性面板的字段表由库算：表格三层都在且"自动"写 0、
  改一层两项一起落且一步撤销回原样；控件字段随类型给、不认识的类型只剩行为三项；
  提交只发变化量、面板不收的键不当成改定义、没改动不算一步、三步撤到改之前；
  页面设置的宽高在固定纸张下置灰、选自定义后可用、应用之后页宽跟着走；
  日期字段的原生控件类型与步长随精度走、只给片段也补成完整 ISO、同值重交不算改动；
  **业务只读的控件拒写**，取消只读之后同一个提交才成）。`ui` 3 个用例 / 13 条判据
  （对话框居中、标题栏拖得动、点遮罩与 Esc 都关；下拉候选多时出搜索框、过滤之后只剩匹配项、
  **在候选列表里滚动不关**、选中回填触发器；滑块点轨道改值、方向键走一档）。
  旧的 `scripts/*_probe.mjs` 与
  `scripts/verify_web.ps1` 已删除，别再按那套入口跑。
- 两个宿主的分工（详见 `docs/正式化架构与迁移路线.md` 4.6）：wasm 是交付物，
  native 只做验证宿主，承担四套断言与内存布局诊断。据此有三条规则：
  **新功能不得只在 native 上实现**，否则它从宿主变成了第二个产品；
  逐像素判据只在同一构建内算数（native 与 wasm 的编译器、SIMD 与 CPU 路径不同），
  线上那一侧仍以浏览器端测试为准；
  光栅只有一个实现 `bitmap_skia.cpp`，两侧都链接它 —— 不要再引入第二个后端，
  跨后端像素比较没有判据意义。
- 注释合规检查：`python scripts/lint_comments.py`（加 `-v` 打印违规原文；违规则退出码 1）。
  扫描 src 与 tests 的 C/C++、scripts 的 .mjs、web 根下的 .js/.ts、web/src 与 web/app 下的 .ts；
  `cd web && npm run check` 里也调它，所以前端那一侧漏不过去 —— 示例页那 4000 行也在里面。
  **违规数必须为 0，不设基线**：新写的注释一旦违规就当场改掉，不要把它记成"基线"留到以后。
  基线这种记账法只会让数字慢慢往上爬 —— 本项目就这么积到过 147 处，最后要一次性清掉。
  共 13 条规则，写在脚本文件头：反引号、装饰分隔线、markdown 强调、流水账用语、
  口语人称、行首列表符号、行宽、块长、声明处必须是块注释、编号列表记号、口语俚语、
  单行块注释、悬空块注释。
- 文档齐不齐另有两份判据（管"有没有"，不管措辞）：
  `node scripts/check_docs.mjs` 按 AST 查 TS 的方法有没有 JSDoc 与 `@param` / `@returns`、
  注释块与上一个成员之间有没有空行（web/src 与 web/app 都扫；**在 `npm run check` 里，当前 0 问题**）。
  `python scripts/check_docs_cpp.py` 查 C++ 头文件：类/结构体成员与命名空间层函数的块注释、
  块与上一条声明之间空一行、公开函数的 `@param` / `@return`、以及"有块又写行尾注释"；
  私有段只要求块注释（内部实现的参数说明写在正文里）。
  **它也在 `npm run check` 里，当前 0 处**（60 个头文件）。判据里刻意不查的几类：
  `*.gen.h`（注释会被下一次生成覆盖）、`= default` / `= delete`、运算符重载、多行续行。
- 改动排版或光栅后，除了单测还要跑浏览器端测试：`stress` 与 `edit` 带像素级与帧自审判据。
- ★ **构建是否成功要看日志里的 `exit=` 行，不能看外层命令的退出码**。用
  `& powershell -File build_*.ps1 *> $log` 包装时，`$LASTEXITCODE` 之后只要再执行任何
  命令（包括写日志的 `Add-Content`）就会被覆盖，于是构建失败也显示成功 ——
  这个坑真实发生过一次，把一次编译失败当成了通过。包装时读完就 `exit $code` 把它传出去；
  直接看日志时以 `exit=0`（且没有"构建失败"与 `error` 行）为准，并且**核对产物时间戳**，
  因为"什么都没编"与"编对了"在输出上长得一样。
- ★ **新加构建 target 必须调 `heditor_target_flags(<tgt>)`**。它按 target 下发 `/utf-8`，
  而源码**都不带 BOM**，所以漏掉它的症状是"中文注释那一行报语法错"
  （C2143 / C2628 / C2059），并且**只在 MSVC 上出现** —— wasm 侧（clang）默认按 UTF-8 读，
  不会报错，于是错误看起来像"native 独有的怪问题"。新增 target 后按
  `grep -c "add_library(<tgt>\|add_executable(<tgt>"` 与 `grep -c "heditor_target_flags(<tgt>)"`
  各应为 1。
- ★ **演示数据在 `demo/`（`heditor_demo` 库），不在 `src/`**：`sampleDocument` 与
  `mixedParagraphBlock` 是喂给引擎的样例内容，不是引擎能力。改它们会影响单测与探针里
  依赖页序的断言，改之前先看那些断言。
- 比对 git 里的文件与工作区文件时**不要用 md5**：本机 `core.autocrlf=true`，
  工作区是 CRLF 检出，每个换行比 blob 多一字节；要比就比 git 对象标识
  （`git rev-parse HEAD:<path>`），或者给 diff 加 `--strip-trailing-cr`。

## 设计与实现准则

### 方案先有依据

出一个设计方案之前先查资料：最新实践、成熟项目怎么做的、有没有现成的标准或协议。
**不凭记忆下结论**——记忆里的做法可能已经过时，也可能只适用于别的语境。
查到的依据要写进文档（谁怎么做、为什么适用或不适用），后来人才能复核，
而不是只看到一个结论。

### 简单直接优先

默认选最简单、最直接、最易读的实现。设计模式与抽象不是目标：
只有它们确实减少复杂度、消除有意义的重复、或稳定一条边界时才引入。
为"以后可能要扩展"预留的层，往往在需求明确之前就已经腐烂。

### 类与文件

类要做出高内聚低耦合的单元。文件按职责合理拆分，不要出现巨长的文件——
如果已经没法一眼说出这个文件负责什么，那就是该拆了。

### 函数粒度

函数表达一个完整且清晰的职责即可，**不要为了缩短行数而切碎**：
拆成几段、每段只有一个调用点、名字还得靠注释解释，比原来更难读。
函数体内部那些只做一件小事的逻辑，保持内联反而更清楚。

### 不留占位与伪成功

不提交占位实现、空壳、或伪造的成功路径。典型形态是恒真的返回值、
打印"完成"却什么都没做、用 TODO 撑着的主流程、把未实现的分支接到一条看似正常的返回上。
这些东西比"还没做"更危险：调用方会以为自己拿到了结果。

暂不实现的能力，要么只声明清楚的接口（签名、语义、失败方式），
要么显式返回可识别的"不支持"错误（错误码、异常，或断言级别的拒绝），
让它在第一次被用到时立刻暴露，而不是悄悄给出错误答案。

## 硬性约束

- 不允许执行任何 git 写入命令（add / commit / push / fetch / reset / checkout / stash 等），
  只读命令（status / log / diff / show）可用。需要提交时由人来做。
- 不删除或移动 `.git` 目录。
- `.workbuddy/memory/` 是项目工作日志，只追加、不覆盖。
