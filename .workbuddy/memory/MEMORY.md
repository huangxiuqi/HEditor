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
- 光标屏障只挡光标：点控件内部也落点（旧选区塌掉、值照 toggle），插入后光标落整块之后。夹哪一侧
  由动作定 —— 落点取离点击处最近那侧（同处点两次不换边），移动取来向的对面（方向键才跨得过去）。
  拖选锚端可以落在屏障里（`drag_anchor_` → `setAnchorAtFlow`；别用 `setSelection`，它清行别）。
- 折行处行别由流位置定（`lineAffinityAtFlow`）。`setCaretRel` 的 `line_aff`：0/1 显式（点击）、-1 自动；
  移动类命令先 `showCaret(true)` 再移动。
- 图片字节不进文档；拖动只更新预览框，松手才 `resizeImage`；提交要记当前选区（空 `Range{}` 会被撤销
  当成文档开头）。把手命中区就是画出来的方块，不沿边线拉伸；监听在按下这一拍同步挂上（起始尺寸
  异步取，否则快拖丢点）。选中图片期间不画文本光标（`caretShown()`：对象选区与文本光标互斥）。
- 宿主骨架的透明承接层（`.heditor-ime`）必须 `pointer-events:none`：它按"光标到页右一行"铺开，
  `opacity:0` 不穿透 —— 少了它，图片把手落在光标处时按下会被接走（是命中层，与画不画光标无关）。
- 控件记录 = 共同层 + `ControlDetail` 变体；标签是投影（`controlDisplayText`）；特有层整个进哈希；
  改定义只走 `ControlPatch`；连打走 `typeTextAndRender`（免得吸旧簇）。
- 选择类记号全自绘（内容 ☐/☑，成形换 U+25A1 借格宽）：外径 `size_pt * 0.72`，距离覆盖率；颜色
  `Appearance.glyph.color`（alpha 0 = 跟随文字）。未配 `group` 的单选是独立开关。
- 控件括号槽宽与字形取边界侧 run 的字体（`bracket_font`，排版绘制共用）。
- 值不在内容里的控件（选择类 / 下拉 / 日期）编辑上是整块：贴它退格删整块，选区不足时扩到整块。
- `blocks_` 不是块流真源（228）；页边距是四个数（227）；滚动总高只有 `scrollHeightPx()` 一条公式（221）。
- 协议表是唯一动词清单；引擎语义只有 `web/src/engine/api.ts`（零 import），引擎只跑 Worker。
- 字体：身份由文件自报，显示名由宿主给（`label`）；`addFont` 只加脸；同一份只装一次；名字在主线程
  折下标再下发。光栅只有 `bitmap_skia.cpp`（Skia 版本钉在脚本里）。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规 0。
