# 项目长期记忆（HEditor）

**约束**：不执行 git 写入；不动 `.git`；`memory/` 只追加；开发期不留向后兼容（判据与文档同步改）。
**索引**：`TRAPS.md`（按号引用）· `ARCH-NOTES.md` · `ENV.md` · `MIGRATION.md`；语义权威见 `docs/`。

## 环境与节奏

- 构建 / 测试 / 分片开关全在 `ENV.md`「构建与验证」；复发陷阱查 `TRAPS.md`。
- 节奏：写代码 → 只跑针对的判据 → 注释与格式化 → 最后全量一次（再跑要有理由）。
- 闸：`scripts\format.ps1 -Check` 全合规；`cd web && npm run check` 全 0。硬规则：构建 wasm 与跑判据
  不并发；跑判据期间不动 `web/`；改 wasm 后必须 `npm run build:worker`。
- 探针：`p.read` 才拼 H/D 前言（`p.ev` 没有）；`showCaret` 是静默写，断言前要 `D.present()`。

## 数据流

修订 `session_` → 投影 `blocks_` → 排版 `layout_` → 页位图 → 画布。光标真源是 `Range selection_`，
唯一写入口 `setCaretRel`。派生量收成一份不可变快照；写入口只留一个；红了先找机制。

## 不变量

- web（wasm）是主线，native 只当单测宿主；判据必须含反向一半。改帧的入口必须是命令（WRITE）。
- 块流只有一份；脏区必须带右端；后缀复用只重排 `[from, from+count)`。
- 页指纹混每条行盒的行指纹（空段标记不占字形）；`rasterSignature` 管档位 / 底色 / 缩放 / 资源代际；
  控件那段 `content_hash` 取 `controlPaintDigest`（外观 + 值），两个编译入口都混。
- 页位图只有内容，瞬态（光标 / 选区 / 图片框）导出时叠。`caret().rect` 是设备像素、
  `selection().rects` 是磅。行距基准是单倍行高；空段也要有行盒。
- 新增 `Node` 字段同步四处：assignFrom / NodeSpec capture / materialize / 序列化。
- 控件区间只查 `control_spans.h`；"文字位置 + 侧别 = 流位置"两端各有唯一实现（229/230）。
- 三档模式只裁三件事：光标能不能停、能不能编辑、隐藏控件是否参与流（222）。
- 光标屏障只挡光标：点控件内部也落点，插入后光标落整块之后。落点取离点击处最近那侧（同处点两次
  不换边），移动取来向的对面；拖选锚端可落在屏障里（`drag_anchor_`）。
- 折行处行别由流位置定（`lineAffinityAtFlow`）；`setCaretRel` 的 `line_aff`：0/1 显式、-1 自动。
- 图片字节不进文档；拖动只更新预览框，松手才 `resizeImage`；提交要记当前选区（空 `Range{}` 会被当成
  文档开头）。把手命中区就是画出来的方块；选中图片期间不画光标。
- 宿主承接层（`.heditor-ime`）必须 `pointer-events:none`：它铺在光标到页右那一条上，是命中层。
- 控件记录 = 共同层 + `ControlDetail` 变体；标签是投影（`controlDisplayText`）；改定义只走 `ControlPatch`。
- 选择类记号全自绘：外径 `size_pt * 0.72`，颜色 `Appearance.glyph.color`（alpha 0 = 跟随文字）。
- 控件括号槽宽与字形取边界侧 run 的字体（`bracket_font`，排版绘制共用）。
- 值不在内容里的控件（选择类 / 下拉 / 日期）编辑上是整块：贴它退格删整块，选区不足时扩到整块。
- `blocks_` 不是块流真源（228）；页边距是四个数（227）；滚动总高只有 `scrollHeightPx()`（221）。
- 协议表是唯一动词清单；引擎语义只有 `web/src/engine/api.ts`（零 import），引擎只跑 Worker。
- 字体：身份由文件自报，显示名由宿主给（`label`）；`addFont` 只加脸；同一份只装一次。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规 0。
- 表格命令的主语是"光标所在那一格"，拒绝条件全在引擎；结构一变流位置就失效，重摆光标按格坐标
  （`caretIntoCell`）。**改格记录一律整份 `CellSpec`**（只赋跨度会把垂直对齐清成顶端）；
  插出来的新格必须带 `ParagraphMark`（否则零块格，点不进去）。
- 右键菜单归宿主：库只拦浏览器菜单 + `input.placeAt` 搬落点 + 回 `onContextMenu(target)`。落点
  种类是 -1/0/1/2/3（none/text/table/control/image），**跨语言别按下标查数组**。UI 一律建在
  `ui/layer.ts`（定位与关闭时机）上，菜单 / 属性面板 / 下拉浮层共用它。新增 host 可见 JSON 读：
  写进 `gen_payloads.py` 的 FILES 才进契约快照。
- 属性面板的**字段表归库**（`describeControl` / `describeTable` / `describePageSetup` 是纯数据），
  提交走 `applyControlForm` / `applyTableForm` / `applyPageSetupForm`（按白名单只发变化量，
  页面设置没改就不重排）；宿主只渲染，标签用 `FormOptions.labels` 覆盖。`updateControl` 收
  readonly / required，拆到 `setControlProp` 单独提交 —— **改定义又翻两个开关是三步撤销**。
- 注释与 JSDoc 判据扫 `web/app`（示例页 4000 行不再靠自觉）：`lint_comments.py` 的 JS 集合与
  `check_docs.mjs` 的 FILES 都含它，违规 0 是硬线。
