# 项目长期记忆（HEditor）

**约束**：不执行 git 写入；不动 `.git`；`memory/` 只追加；开发期不留向后兼容（判据与文档同步改）。
**索引**：`TRAPS.md` · `ARCH-NOTES.md` · `ENV.md`；语义权威见 `docs/`。

## 节奏与闸

- 写代码 → 只跑针对的判据 → 注释与格式化 → 收尾全量一次。
  **全量不是保险**：只覆盖已有判据；没判据的改动先补一条。
- 闸：`format.ps1 -Check` 全合规；`cd web && npm run check` 全 0。构建 wasm 与跑判据不并发；
  跑判据期间不动 `web/`；改 wasm 或 `web/src/engine/api.ts` 后必须 `npm run build:worker`
  （映射表在 Worker 壳里；漏了的表现是「命令生效、读回的字段却是旧的」）。
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
- 单位：`caret().rect` 是设备像素，`selection().rects` 与控件矩形是磅（读像素 / 点控件先乘
  `ZOOM`）；行距基准是单倍行高，空段也要有行盒。
- 块轴两套坐标：分页是页内 y（放不下就翻页、块轴回页顶），连页是整篇累加的绝对 y（页号与页内
  y 由位置现算）。按行推进块轴的写法（后缀平移）必须分两套，否则第二页起的后缀被拽回条带顶。
- 删除后的落点：整表 / 整块删取被删节点的起点，不取选区起点（起点在被删范围内部，删完会被
  后面的内容顶上来，光标就落在段中间）。
- 复制 / 删除同一把尺子：完整包住的控件整块走，只包住一部分的文本控件只取选区那几段文字
  （控件壳剔除、括号位跳过，见 `eraseAcrossControls`）。
- 控件区间只查 `control_spans.h`；"文字位置 + 侧别 = 流位置"有唯一实现（229/230）。
- 表格命令的主语是"光标所在那一格"，拒绝条件全在引擎；改格记录一律整份 `CellSpec`；新格必须带
  `ParagraphMark`；文档不能以表格结尾（末尾补空段）。
- 整格选区盖满某张表的**全部格位** = 选的是这张表（拖选整表即此形态）⇒ 删整表删节点；只盖
  一部分才逐格清空（逐格比块区间，空格流长度为 0）。表是文档唯一内容时不动结构。
- 值是投影的控件（选择类 / 下拉 / 日期）：空值投影成提示、光标屏障生效（否则连插两个会嵌套）；
  下拉与日期贴它退格**先清值**再删块。
- 空控件提示是排版槽不是流位置（`InlineObject::hint` → 一枚 `control_hint` 簇，宽
  `hintAdvancePt`、上限容器宽）：数槽 = 数位置处都跳过它（`bracketCountAt`、`offsetAtSide`、
  `hitTestInLine`）；不进流，判空仍按 `hi-lo==2`，没有码点时 `controlRects` 用提示槽。
- 图片：字节不进文档；拖动只更新预览框，松手才 `resizeImage`；进格的图按格宽限宽。
- `.heditor-ime` 必须 `pointer-events:none`。右键菜单归宿主：库只拦浏览器菜单 +
  `placeAt({tap:false})` + 回 `onContextMenu(target)`（种类 -1/0/1/2/3）；UI 建在 `ui/layer.ts` 上。
  菜单高亮是**一条链**：子菜单开着时父项一直亮，置灰项也接悬停。
- 属性面板：字段表归库（`describe*`），提交走 `apply*Form`（只发变化量）；翻 readonly / required
  走 `setControlProp`。新增 host JSON 读写进 `gen_payloads.py` 的 FILES 并在 `api.ts` 折返。
- 注释硬线 = `lint_comments.py` 违规 0（R1~R13，`web/app` 也在扫描集）。
