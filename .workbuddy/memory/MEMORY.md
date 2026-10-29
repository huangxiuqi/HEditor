# 项目长期记忆（HEditor）

**约束**：git 写操作可用，但 `git stash` 绝对禁止（沙箱已拦，勿等价模拟）；不动 `.git`；
`memory/` 只追加；开发期不留向后兼容（判据与文档同步改）。
**索引**：`TRAPS.md` · `ARCH-NOTES.md` · `ENV.md`；语义权威见 `docs/`；
数据流与特例面的现状见 `docs/架构复核-数据流与特例面-2026-10-27.md`（取代 10-19 那份的数据流部分）；
位置与坐标的契约见 `docs/位置与坐标-契约.md`（flow 是文档坐标、affinity 是本地视图态、换算只有两个函数）。

## 节奏与闸

- 写代码 → 只跑针对的判据 → 注释与格式化 → 收尾全量一次。
  **全量不是保险**：只覆盖已有判据；没判据的改动先补一条。
- 闸：`format.ps1 -Check` 全合规；`cd web && npm run check` 全 0。构建 wasm 与跑判据不并发；
  跑判据期间不动 `web/`；改 wasm 或 `web/src/engine/api.ts` 后必须 `npm run build:worker`
  （映射表在 Worker 壳里；漏了的表现是「命令生效、读回的字段却是旧的」）。
- **判据红了先排除构建**：中文 VS 的 `/showIncludes` 前缀是「注意: 包含文件:」，CMake 只认
  英文 → ninja 头依赖恒为 0（同一份代码三次跑出 28/7/2 项失败）。已在 `CMakeLists.txt` 用
  `CMAKE_DEPENDS_USE_COMPILER OFF` 让 CMake 自己扫 `#include`；核对：`ninja -t deps <obj>`
  应 `#deps > 0`。
- 探针：`p.read` 才拼 H/D 前言；`showCaret` 是静默写。读页像素要 `H.flush()` + 两帧 rAF，
  页画布是 OffscreenCanvas（`drawImage` 出来再读）。
- 判据按**功能域**建 spec：一域一份，域内 `describe` 分节；新功能与修缺陷都加进所属域，
  不建按批次命名的文件（`feature` / `fixes` 已拆）。域够大就单开（`image` / `table` /
  `view` / `clipboard` / `format`），`edit-*` 七片只是"一份太大"的切分。

## 不变量

- web（wasm）主线，native 只当单测宿主；判据必须含反向一半；改帧的入口必须是命令。数据流：
  修订 → `blocks_` → `layout_` → 页位图 → 画布；光标真源 `Range selection_`，写入口 `setCaretRel`。
- 块流只有一份；脏区必须带右端；后缀复用只重排 `[from, from+count)`；`blocks_` 不是块流真源。
- 页指纹混行盒指纹与 `controlPaintDigest`；`rasterSignature` 管档位/底色/缩放/资源代际。
  行指纹必须含"画的时候才用到"的行盒字段（`clip_text`、`advance_pt`、`lead_top_pt`），
  漏一项就是「改完行距命令成功、画面不动」（TRAPS 232，同族 195）。
- 单位：`caret().rect` 是设备像素，`selection().rects` 与控件矩形是磅（读像素 / 点控件先乘
  `ZOOM` = zoom×4/3×dpr，`D.stats().zoom` 只是倍数不是密度）；行距基准是单倍行高，空段也要有行盒。
- 行距三态（auto/exact/atLeast）在 `BlockSpec.line_spacing`，行高统一走 `lineHeightFor`；
  `exact` 比墨迹矮时 `LineBox.clip_text` 为真，绘制侧按行进出裁剪栈 —— 裁剪一律用
  `Bitmap::clipMark/restoreToMark`（按深度退回），布尔配对少退一次会让整块画布一直被裁。
- 多出的空白怎么分，两档不同（`lineLeadTopFor`）：auto 上下对半 —— 字落在行槽中间（Word 在
  文档网格下、CSS 的 line-height 同一口径）；exact/atLeast 贴盒顶、空白全在下方。所以行盒有
  两个纵向范围：`block_lo/hi` 是字面盒（字形、脏区、图片锚点），行槽是 `slotLoPt()..+slotSpanPt()`
  （光标与反白）；段距靠光标矩形行顶差量。改这块要同步 `engine_layout` 的增量平移（按 lead 重算基线）。
- 零宽选区只在"本行段落标记真被选中"时补窄条；软换行处起选（上一行行末 / 下一行行首）不补，
  否则上一行末尾凭空多一个小方块。
- 块轴两套坐标：分页是页内 y（放不下就翻页、块轴回页顶），连页是整篇累加的绝对 y（页号与页内
  y 由位置现算）。按行推进块轴的写法（后缀平移）必须分两套，否则第二页起的后缀被拽回条带顶。
- 删除后的落点：整表 / 整块删取被删节点的起点，不取选区起点（起点在被删范围内部，删完会被
  后面的内容顶上来，光标就落在段中间）。
- 复制 / 删除同一把尺子：完整包住的控件整块走，只包住一部分的文本控件只取选区那几段文字
  （控件壳剔除、括号位跳过，见 `eraseAcrossControls`）。
- 控件区间只查 `control_spans.h`；"文字位置 + 侧别 = 流位置"有唯一实现。
- 表格命令的主语是"光标所在那一格"，拒绝条件全在引擎；改格记录一律整份 `CellSpec`；新格必须带
  `ParagraphMark`；文档不能以表格结尾（末尾补空段）。
- 块↔格只有一份记录：`CellIndex` 从 `LayoutTable::cells` 的块区间反推（`BlockSpec` 不带
  `cell_path`，`StructureIndex` 只是它的视图）。要问"这一段在哪一格"就查 `cellIndex()`。
- 整格选区盖满某张表的**全部格位** = 选的是这张表（拖选整表即此形态）⇒ 删整表删节点；只盖
  一部分才逐格清空（逐格比块区间，空格流长度为 0）。表是文档唯一内容时不动结构。
- 值是投影的控件（选择类 / 下拉 / 日期）：空值投影成提示、光标屏障生效（否则连插两个会嵌套）；
  下拉与日期贴它退格**先清值**再删块。
- 空控件提示是排版槽不是流位置（`InlineObject::hint` → 一枚 `control_hint` 簇，宽
  `hintAdvancePt`、上限容器宽）：数槽 = 数位置处都跳过它（`bracketCountAt`、`offsetAtSide`、
  `hitTestInLine`）；不进流，判空仍按 `hi-lo==2`，没有码点时 `controlRects` 用提示槽。
- 图片：字节不进文档；改尺寸只更新预览框、松手才 `resizeImage`；浮动图拖动用
  `previewImageMove`：四周型 / 上下型把位移叠进 `floatsForLayout()` 的副本**真重排**，
  文字当场绕开；衬于下方 / 上方不占版面，仍只挪绘制（`ImagePreview::placed` 区分两者，
  已排进版面的位移 `imagePreviewPos` 不再叠第二次）；松手才 `moveImage`，不画插入点
  （`hit.drag` 为 3）；行内图在流里，按 2 走落点提示。进格的图按格宽限宽。
- 「点 → 格」只有一处实现（`PlacedTable::cellAtPt`）：命中、悬停栅格、格线拖动都读它。
  容差是两个值不是一值：横向 0.5 抓边框、纵向 0.01 保表外那一点。网格列按占位推
  （`gridColStarts`），不按本行累加 —— 跨行合并后下面几行的列会整体左移。
- 续排锚点必须带排除带（`ReflowState::bands`）：从中间续排看不到前面段落的浮动图，
  没有这份快照，行宽与整篇排版不同。拼接那条路仍不带有图文档（带是绝对位置）。
- 浮动图摘要与绘制、命中、选中框一律走 `imagePreviewPos`：各写一份的话拖动中框与图错开。
- 拖动预览只标图那一页（`markPageDirty`），别用 `markPagesChangedFrom`：后者从该页标到
  末尾，视口每页都重光栅、每帧回传十几兆。输入层必须只留最新坐标（上一个预览回来再发
  下一个）—— 事件比一帧密，逐个发就在 Worker 前排队，症状是图落后指针几秒，不是单帧慢。
- `afterEdit` 之后只在**光标真的挪了**时才滚视口（`caretPara/Offset/Page` 有一项变）。
  否则搬图、改格式、提交表单都会把视口拽回旧光标处 —— 那是"松手页面自己跳了"。
- 一行可多片段（四周型图在中间）：`LineBox::spans` 只在段数 > 1 时填，簇的 `inline_pt`
  在空隙处直接跳过；绘制、命中、光标全按簇走，只有选区要按 spans 切。行高与基线取各段
  最大值，`advance_pt` 也是。一段都放不下时下推到框底（与上下型同一条规则）。
- `.heditor-ime` 必须 `pointer-events:none`。右键菜单归宿主：库只拦浏览器菜单 +
  `placeAt({tap:false})` + 回 `onContextMenu(target)`（种类 -1/0/1/2/3）；UI 建在 `ui/layer.ts` 上。
- 属性面板：字段表归库（`describe*`），提交走 `apply*Form`（只发变化量）；翻 readonly / required
  走 `setControlProp`。新增 host JSON 读写进 `gen_payloads.py` 的 FILES 并在 `api.ts` 折返。
- 注释硬线 = `lint_comments.py` 违规 0（R1~R13，`web/app` 也在扫描集）。
- 浮动图框一律夹进版心（`clampFloatBoxToTextArea`）：排版、`imagePreviewPos`、写偏移三处共用；
  只夹预览会把出界值存进文档，下次拖动先空走。
- 表格右边框 / 底边框也是格线（`i ∈ [1,n]` / `[0,n-1]`）；`setTableGridCols` 同时钉表宽，
  表宽上限 = 内容区 + 右边距。宿主拖最右那条只改最后一列（没有下一列可分摊）。
- 连页的条带边界按设计会切开一行：别想"把行挪到下一条带顶"来消掉 —— 那条规则让排版依赖
  页高，`edit_test` 的「分带与整带逐字节相同」会红，缝上还会行距变窄。脏页指纹要
  `dirty_hi + 1`（接缝那半行画在下一页）。
- 接缝两半用同一个位移拼，位移必须取整像素（`spill_dy = -pagePxHeight()`）。用
  `slab_pt * zoom` 会带小数（A4 下 1122.7，位图 1123 行），段落标记的横笔正好压在
  两行之间、被抗锯齿摊成两行浅灰 —— 看着比别的标记粗一档（`view` 的接缝用例钉住它）。
- 开发期没有热更（`server.hmr:false`），改完手动刷新。判据侧 `reuseExistingServer:true`：
  5173/8781 上残留的旧 dev server 会一直发旧转译（症状是"改了宿主的字段映射却不生效"），
  跑之前先确认端口干净。
