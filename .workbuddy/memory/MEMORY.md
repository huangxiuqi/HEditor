# 项目长期记忆（HEditor）

**最高约束**：不执行 git 写入命令；不删除/移动 `.git`；`memory/` 只追加。
**阶段约定（2026-10-23）**：开发阶段不考虑向后兼容 —— 不写旧标记/旧签名的兼容分支，改名、
删选项、换形状直接做，改完把判据与文档一起更新。
**索引**：`TRAPS.md`（复发陷阱 1~213，按编号引用）· `ARCH-NOTES.md` · `MIGRATION.md` ·
`ENV.md`。设计文档：`docs/文档模型设计.md`（语义权威，`wc -l` 必须 2843）·
`正式化架构与迁移路线.md` · `外部API设计.md` · `跑测试.md` · `步5-渲染层方案.md`。

## 环境与基线

- native `ctest --preset native` 4/4：`shaping_and_layout` 151 / `edit_and_caret` 1713 /
  `doc_model` 846 / `rev_layer` 338（[24] 每活节点 ≤ 96 字节）。整套约 110 s。
- 单节调试用 `HEDITOR_TEST_ONLY="70,72"`；`edit_and_caret` 里 `init` 每次装字体约 56 ms。
- 构建：preset `native` / `native-debug` / `wasm` / `wasm-debug`，预设里**不含本机路径**。
  native 之前先 `.\scripts\msvc_env.ps1`（vswhere 找实例 + 调微软 `Enter-VsDevShell`，
  与开始菜单的 Developer PowerShell 同一件事）；`CMakeUserPresets.json` 已删、不再需要。
  **cmd 只是命令文本被校验层拦，运行期可跑**（DevShell 内部就起 cmd），所以别再手拼
  INCLUDE / LIB / Path 那种路径表。
- **`.toolchain` 三件换机器先跑 `scripts/toolchain_init.ps1`**（gn 排最前，fail-fast）：gn 钉
  `2222 (3c7785ec2008)` 但**只校验不下载** —— 那一份没有可下载的制品（CIPD 的
  `git_revision:3c7785ec…` 回 no such tag，Skia 自己的 fetch-gn 是更旧的 2175），缺了只能拷
  2.3 MB 的 exe 或现编；ninja 1.13.1 带 zip sha256；emsdk 6.0.12（安装器 commit 35ff8a6d，
  GitHub 归档字节不稳定，所以只钉 commit 不钉哈希）。
- 格式化**一律全局做**（`powershell -File scripts\format.ps1`），跑完 `-Check` 应为"全部合规"，
  然后必须重建两侧并重跑判据。两个配置缺口已补：`.clang-format` 的
  `EmptyLineAfterModifier: Always`（否则与 `check_docs_cpp.py` 的 missing-gap 打架）、
  `.clang-format-ignore` 排除 `payload_schema.gen.h`（生成物）。
- 示例页是 Vite 应用（`web/index.html` + `web/app/*.ts`）：`npm run dev` 起 5173，
  `npm run build:app` 出 `web/dist-app/`（验收口径）；改 wasm 后 `npm run build:worker`。
- **浏览器判据全是 Playwright（`web/tests/specs/`，13 份）**：demo / multi / saveload / input /
  diag / error / stress / edit(367 条) / clipboard / format / insert / smoke / table。
  硬规则：引擎状态一次性求值（禁 `expect.poll` 与 locator 断言）、`channel:'chrome'`、
  像素账以旧值为基线、**构建 wasm 与跑判据不许并发**（TRAPS 198）、
  **跑判据期间不动 `web/` 下任何文件**（Vite 热重启 → `Execution context destroyed`）。
  时序类读数：先 `stopBlink()` 再 `setCaret`（前者不 present）；"画面复原"比
  `diag.frameDigest()` 而不是 DOM 截图。
- 场地三件：`support/config.ts`（dev `/` 5173 / dist `/web/dist-app/` 8781）·
  `support/probe.ts`（`open` fixture、`ev` 不拼前言、`read` 拼 `readings.ts` 前言）·
  `support/check.ts`（`expect.soft` 一次性求值）。临时诊断写 `tmp.spec.ts`，用完即删。
- `web/vite.config.ts` 的 `server.host: '0.0.0.0'` 是用户自己要的，别当误改还原。

## 方向

- 派生量收成一份不可变快照，不留第二份可写真相；影响面由产生它的那一步带下去。
- 写入口只留一个；绕过提交路径的第二条路迟早分叉。
- 归因顺序：先最小复现，再跑对照，最后怀疑自己的改动。**红了先找机制，不许记成"偶发"。**

## 数据流四条主线（改动前先认线）

- 模型：引擎只持一份修订（`session_`：`rev::Doc` + 撤销栈），模型树只在装载/审计两侧
  （`fromTree` / `toTree`）。编辑只产 `Step`，产物每轮 `project` 重算（TRAPS 3）。
  修订 → 投影 `blocks_` → 排版 `layout_` → 页位图 → 画布。
- 光标：真源 `Range selection_{anchor, focus}`，`caret()` 是 focus 的投影，写入口只有
  `setCaretRel`；重编译前先落地。
- 选区：文字选区只盖文字，单元格选区整格反白；`sel_` 存落点、`cell_lo_/hi_` 存流包络、
  `cell_caret_` 钉插入点；渲染与编辑读同一个 `selection()`。每段那枚段落标记占流里一位、
  不属于本段内容 ⇒ `block_flow[i] + block_flow_span[i]` 就是标记位，末段标记由
  `selectionRectsIn` 按"正向且焦点在文档末"补一条（TRAPS 209）。
- 位图：引擎里没有帧，只有页位图；页位置 = 页号 x 步距（宿主算）。

## 不变量与硬结论

- 判据必须含反向一半；web（wasm）是主线，native 只当单测与审计宿主。
- 新增 `Node` 字段同步四处：`assignFrom` / `NodeSpec` capture / materialize / 序列化读写。
- 会改帧的入口必须是命令（WRITE）；worker 的 READ 只回值，不 present、不 blit。
- 块流只有一份（`project(...)` 的 blocks 既是上轮也是本轮输出）；流位置只存局部量
  `BlockExtent{span, gap}`，每轮 `buildBlockFlow()` 重算（[62]）。
- 排版脏区必须带右端 `layout_dirty_from_/to_`；后缀复用只重排 `[from, from+count)`（125）。
- 控件集合只有一个来源：投影 `ControlSpan`（顺序 = 文档顺序）。
- 跨层坐标自带口径：流位置是（位置，可见性），`applySteps`/`damage` 带 `FlowVisibility`。
- 光栅按页缓存 `PageBitmap{fp, raster_sig, valid}`，两者都相等才算新鲜。
  **页指纹里除字形还必须混进每条行盒的行指纹**（空段标记不占字形，TRAPS 208）。
- **"按页取元素"一律走 `LayoutResult::page_lines` / `page_glyphs`**：行与字形按流顺序排，
  表格行跨页之后页号不单调，数组相邻也不等于版面相邻（TRAPS 215）。行带边界按块轴
  排序分组算（`pageLineBands`），两端取所有变化行的并集；索引重建要先数后填、别每拍建两遍
  （216：2000 段文档上 59 ms → 7 ms）。
- 页位图只有内容，瞬态（选区/光标）导出时叠；上屏按矩形贴；`caret_x/y` 与选区矩形都是
  页内坐标，页号另给。
- 页内容指纹没覆盖"光栅要画的全部输入"就是"命令成功、读回也对、auditLayout 还是 0、
  画面停在旧帧"（195）；`rasterSignature` 管档位/底色/缩放这类外因（128）。
- 缩放：引擎收密度（96 dpi = 4/3，含 DPR），界面收倍数。
- 表格：三种删除；进增量时脏区对齐最外层表起始段与表尾；`truncateLayoutAt` 连带丢
  `PlacedTable`，`trySuffixReuse` 按 first_block 二分换表（[61]）。跨页切片要连字形的
  页号一起改（210）；连页下行的进给是块轴绝对位置（211）；续排片段上的格可能没有行盒，
  命中与整格反白按表几何（212）。
- 行盒自己记 ascent / descent / advance；`placeLine` 记 `top`（148）。原子与括号共用占位簇，
  但原子 `bracket_side` 是 None，绘制按 `== Open` 判。
- 空段没有内容也必须有行盒（`lb.last_cp = 0`）：光标要有落点，标记要画得出来。
- **行距的基准是单倍行高（ascent + descent，NotoSansSC 约 1.448 em），不是字号**：
  `line_h = max(单倍行高 × lf, 内容高 + leading)`。默认 `line_factor = 1.0`、
  `space_after = 6 磅`（BlockSpec 与 ParaFormat 同一档；wasm 的 `loadTextDocument` 也写 1.0）。
- 单元格边距：左右 `kTableCellPadXPt = 4pt`、上下 `kTableCellPadYPt = 0`，行高 = 内容高，
  下限 1pt 只为挡住零高行。
- 字形基线吸附整设备像素（`Bitmap::drawGlyphs` 里 `round(y)`），横向仍亚像素。
- 会话层：撤销拿回旧修订不做逆操作；撤销后清空重做栈、没变化不占栈（111）。
- 断行：`breakLine` 切一行 + 度量，`breakBlock` 横排整块；竖排一列一列断。
- 光栅只有一个实现 `bitmap_skia.cpp`；native 前先 `scripts/build_skia_native.ps1`；
  Skia 版本钉在 `scripts/skia_rev.ps1`，源码与六个外部依赖只走 `scripts/skia_fetch.ps1`
  （唯一取数入口，只依赖 curl + tar；两个构建脚本成树前都会调它，`toolchain_init.ps1 -WithSkia`
  只是转接一次）。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规必须 0、不设基线；
  `check_docs.mjs`（TS 按 AST）与 `check_docs_cpp.py` 都在 `npm run check`。
- **tsc 是 wasm 构建的硬依赖**：`--emit-tsd` 链接期调它，缺了留未优化 wasm（TRAPS 153）。
  载荷层 `payloads.json` → `gen_payloads.py` → 两份生成物入库。
- 文件格式 v3：文本 UTF-8，顶层字体表只列用到的下标，身份是 PostScript 名。保存/打开只走
  修订层（`saveDocument` 查询 / `loadDocument` 命令，共用 `installDocument`）。
- 网格推导只有 `buildGridFromSpans`；`format_map` 管长度/行高/垂直对齐，`atom_map` 管原子盒。
- `ParaDelta` 已删；`commitSteps` 只带 `dirty_para`。

## 外部面（Editor 对象）

- 面 = 44 方法 + `element` / `diag`；`clickAt` / `selectCell` / `hit` / `removeControl` 归 `diag`。
  `diag` 由 `createEditor({debug:true})` 开关；契约真源 `web/src/types.ts`。
- **单位不统一（待修）**：`caret().rect` 是设备像素，`selection().rects` 是磅。判据里送去
  `diag.hit`（磅）的坐标要先折。两类都还没折成"CSS 像素"，改它要连 13 份 spec 一起动。
- **插入结构只有一条路**：`insertTable` / `insertControl` 都造 `NodeSpec` 走
  `InsertNode{at_flow, subtree}` → `commitSteps`。表格落点是"当前段标记之后"；控件落点在
  光标处。第一格内容的流偏移是常量 3。两者都只在文档模式放行。
- **页内容指纹就是"这一页画面对不对"的判据**：`mixGlyph` 混 glyph_id / x / y / rotate /
  cluster / 颜色 / italic / underline / size_pt / weight。
- **三档模式只裁三件事**：光标能不能停在这一点、能不能编辑、隐藏控件是否参与流。
  不存在"没有可编辑控件就退回文档模式"那一档（222）；`formDegraded` 已删。
- **删除与落点按模式分家**：文档模式走括号四条规则（进出字段、删空控件）；表单模式走
  `eraseInControl` —— 只删光标所在控件的内容、不删控件、不带出括号，选区删除要求整个区间
  在同一控件内（223）。"两端都可编辑"不等于"区间合法"，相邻字段共享偏移时会从隔壁偷字，
  判据锚在光标所在控件（`eraseInsideCaretControl`，224）。
- **选区与光标是两件事**（225）：选区自由（拖选 / Shift 扩展 / 全选跨控件），插入点只在
  能打字的地方 —— 表单模式限于可编辑控件内容，只读模式一个都不画。画不画由
  `insertableAtFlow(f)` 一处决定（`caretInsertable()` / `editStats.caretVisible` /
  `collectTransient` / 页导出都读它），夹取由 `confineCaret` 做。程序化定位
  （`setCaret`/`setCaretAt`）不受限，受限的是点击、方向键、Home/End。
  - **控件内容区间只从流索引取**（229）：`flow_->intervalOf(control_node_ids_[i])` 给含两枚
    括号的半开区间，内容区间退掉两头。**不许用"文字区间 + 括号排布"反推** —— 六枚括号压
    同一 cp 时（三枚空控件套起来）反推要把区间缩成一个点。
  - **判"这一格归谁"取最内层**（`innermostControlAt`：起点最大，同起点取跨度短），
    不能按遍历顺序取第一个命中的 —— 外层可编辑会替内层只读作证。
  - 夹取后的落点仍不可插入（区间中间嵌着只读层）就退回这一步之前的位置；一步之前
    也不在任何控件里就整下不动。
- **页边距是四个数**（227）：`PageMetrics::margins{top,right,bottom,left}`；纸张规格不进
  引擎，TS 侧 `PAPER_PT` 算宽高，`pageSetup` 命令下发六个浮点数。`configure({page})` 代价
  是 relayout。纸张另有 `'Custom'` 档（`custom:{w,h}`，36~5000 磅），示例页面板里能直接填。
- **`blocks_` 不是块流真源**（228）：括号常显时 `syncInlineObjects` 会清空它、排版吃
  `blocks_full_`。判"有没有内容 / 画不画光标"不许读 `blocks_`，要么走 `blocks()`、
  要么干脆不参数化块流。
- **裁剪标记默认开**，臂长固定 18 磅（1/4 英寸，印刷裁切线惯例），边距更窄时按边距一半收
  （226）；它同时是"页边距设对了没有"的直接读数。
- **页面缩放归编辑器**：Ctrl+滚轮 / 捏合 / Ctrl+±/0 在库内拦截（`input.onWheel` +
  `zoomKeyOf`），宿主用 `onZoomBy`/`onZoomReset` 接到 `setZoom`，增量按帧合并。
- **落点格式取左边**（200）：敲字沿用落点左边那一段；段首没有左边才取右边；打字盖住选区取
  第一个字。空选区回显与敲字同一个函数（`marksAtFlow`）。
- **`formatAt({effective:true})` 报"实际生效的那一份"**：字体字号永远有值，加粗按字重判（≥600）。
- **格式四方法** `format` / `paragraphFormat` / `formatAt` / `paragraphFormatAt`：写读对称
  （`b/i/u/fi/sz/col` 与 `lf/sa/pbb`），三态、幂等；作用面是选区那段文字。
- **工具栏焦点**（197/201）：按钮 `mousedown` preventDefault；`<select>` 自己要焦点才能展开，
  外壳 click 收尾跳过 `select, input, textarea, label`。
- **编辑标记 `showMarks`**（`none`/`paragraph`/`all`，默认 `paragraph`）：画在页位图里、
  不占宽度、只影响重光栅；档位混进 `rasterSignature`；自绘不借字形（202）。条件是
  `lb.last_cp == 该段 cp_text.size()`。
- **裁剪标记 `marginMarks`**（默认关）：正文四角的短角线，臂朝页边距一侧伸（朝里会压着
  第一个字）；`show_margin_marks` + `margin_mark_color`，进 `rasterSignature`，连页不画。
- **`setIndent` 是 Tab 语义**，不是首行缩进；有选区先收到左端（194）。
- **剪贴板**：片段是结构（`rev::captureRange` 取 `NodeSpec` 串，`writeFragment` 自带字符串
  表）。写 `navigator.clipboard` 必须带 `web ` 前缀；Worker 载荷异步 → 写 `DataTransfer`
  会"成功但剪贴板为空"（184/185）。
- 配置只有 `configure(patch)`（原子、只做最贵一档）；`mode: document|form|readOnly` 落到
  `setEditMode 0/1/2`；`change` 带惰性 `json`。
- 公开面是白名单 Proxy（`guard.ts`），`web/dist/index.d.ts` 由 `npm run build:types` 生成。
- **协议表是唯一动词清单**：`COMMANDS` 三张实现表由 `collect(kind, impl)` 套起来；
  `PRESENT_ALWAYS = IDL.presentAlways()` 不许手抄；`OUTSIDE` 9 条。
- **引擎语义只有一份** `web/src/engine/api.ts`（零 import）；引擎只跑 Worker，壳是
  `transport/worker.ts` + `worker_main.ts`（`web/dist/heditor_worker.js`）。
- **跨层坐标折算只在一处**：`host/dom.ts` 的 `docPt()`；输入层与拖选不许自己除 zoom（156）。
- **输入层只有一份**：`web/src/input/{keymap,input,drag}.ts`；宿主只交环境对象。
- 门面形状："路 C" `then1(v, fn)` 把同步值与 Promise 折成一条链。
- 两宿主文件各自包 IIFE；命名 `heditor` / `window.__heditor`。
- **页画布必须绝对定位、页容器是它的定位祖先**（189）；白纸由页位图给（188）。
- **滚动总高只有一条公式** `host/pages.ts` 的 `scrollHeightPx()`：分页 = 页数×步距−末尾间隙；
  连页 = max(一页, 内容底边)，最后一页只露自己那一段。**画布 CSS 尺寸恒为整页**，多出来的
  白纸靠页容器 `overflow:hidden` 裁（改 canvas 的 CSS 高度是压扁位图，221）。
  `geom()` 为此多报 `continuous` / `contentPx`。
- **库内骨架只按类名认**：`heditor-stage` / `heditor-pages` / `heditor-ime`，
  页画布 `heditor-pages__canvas`，错误面 `heditor-error__title|detail|hint`（191）。
- **示例页外壳组件化**（`web/app/ui/`）：`update()` 只重读回显、不重建 DOM；动作表在
  `main.ts`，`api_check.mjs` 扫 `web/app` 全部 TS；CSS 走 BEM；下拉框候选是固定档位，
  列外的值补临时项。状态栏左侧常驻页数与字数（`setCounts`），
  `beforeunload` 只在"改过且没存"时拦。
