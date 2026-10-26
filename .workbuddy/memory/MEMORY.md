# 项目长期记忆（HEditor）

**约束**：不执行 git 写入；不动 `.git`；`memory/` 只追加；开发期不留向后兼容（判据与文档同步改）。
**索引**：`TRAPS.md`（按号引用）· `ARCH-NOTES.md` · `ENV.md` · `MIGRATION.md`；语义权威见 `docs/`。

## 节奏与闸

- 构建 / 测试命令查 `ENV.md`「构建与验证」，复发陷阱查 `TRAPS.md`。
- 写代码 → 只跑针对的判据 → 注释与格式化 → 最后全量一次（再跑要有理由）。
- 闸：`format.ps1 -Check` 全合规；`cd web && npm run check` 全 0。构建 wasm 与跑判据不并发；
  跑判据期间不动 `web/`；改 wasm **或** `web/src/engine/api.ts` 之后必须 `npm run build:worker`
  （读写映射表在 Worker 壳里跑，漏了的表现是「命令生效、读回的字段却是旧的」）。
- 探针：`p.read` 才拼 H/D 前言（`p.ev` 没有）；`showCaret` 是静默写，断言前要 `D.present()`。

## 数据流

修订 `session_` → 投影 `blocks_` → 排版 `layout_` → 页位图 → 画布。光标真源是 `Range selection_`，
唯一写入口 `setCaretRel`。派生量收成一份不可变快照；写入口只留一个；红了先找机制。

## 不变量

- web（wasm）是主线，native 只当单测宿主；判据必须含反向一半；改帧的入口必须是命令。
- 块流只有一份；脏区必须带右端；后缀复用只重排 `[from, from+count)`；`blocks_` 不是块流真源。
- 新增 `Node` 字段同步四处：assignFrom / NodeSpec capture / materialize / 序列化。
- 页指纹混行盒指纹与 `controlPaintDigest`（外观 + 值）；`rasterSignature` 管档位/底色/缩放/资源代际；
  页位图只有内容，瞬态（光标/选区/图片框）导出时叠。
- `caret().rect` 是设备像素、`selection().rects` 是磅；行距基准是单倍行高，空段也要有行盒。
- 控件区间只查 `control_spans.h`；"文字位置 + 侧别 = 流位置"两端各有唯一实现（229/230）。
- 三档模式只裁三件事：光标能不能停、能不能编辑、隐藏控件是否参与流（222）。屏障只挡光标：
  点控件内部也落点，插入后光标落整块之后（拖选锚端可落在屏障里，`drag_anchor_`）。
- 表格命令的主语是"光标所在那一格"，拒绝条件全在引擎；改格记录一律整份 `CellSpec`；插出来的
  新格必须带 `ParagraphMark`。文档不能以表格结尾：`insertTable` 落在末尾时补一段空的（两段一次撤销）。
- 值是投影的控件（选择类 / 下拉 / 日期）：空值投影成一句提示，光标屏障生效（否则连插两个会嵌套）；
  下拉与日期贴它退格**先清值**再删块。`readonly` 只表示业务只读，不占"内容只是投影"这个位。
- 图片：字节不进文档；拖动只更新预览框，松手才 `resizeImage`；插进单元格的图按格宽限宽。
- 宿主承接层 `.heditor-ime` 必须 `pointer-events:none`（它铺在光标到页右那一条上，是命中层）。
- 右键菜单归宿主：库只拦浏览器菜单 + `placeAt({tap:false})` 搬落点 + 回 `onContextMenu(target)`；
  落点种类是 -1/0/1/2/3（none/text/table/control/image，跨语言别按下标查数组）。UI 一律建在
  `ui/layer.ts` 上（锚定滚动即关 / `openCentered({scrim})` 滚动不关），浮层之间互不算"外面"
  （`openedLayers`）；菜单二级按行定位并夹进视口。
- 属性面板的字段表归库（`describe*` 是纯数据），提交走 `apply*Form`（只发变化量）；`updateControl`
  收 readonly / required，翻开关走 `setControlProp`（改定义又翻开关 = 三步撤销）。
- 新增 host 可见 JSON 读：写进 `gen_payloads.py` 的 FILES，并在 `api.ts` 里补逐字段折返。
- 原生 `select` / `range` 已换成 `ui/select.ts` / `ui/slider.ts`；判据用 `PICK/SELECTED/SELECT_VALUE`。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规 0 是硬线；`web/app` 也在扫描集里。
