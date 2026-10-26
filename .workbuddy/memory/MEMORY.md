# 项目长期记忆（HEditor）

**约束**：不执行 git 写入；不动 `.git`；`memory/` 只追加；开发期不留向后兼容（判据与文档同步改）。
**索引**：`TRAPS.md`（按号引用）· `ARCH-NOTES.md` · `ENV.md` · `MIGRATION.md`；语义权威
`docs/文档模型设计.md`、`docs/外部API设计.md`、`docs/跑测试.md`。

## 环境与节奏

- 构建 / 测试 / 分片开关全在 `ENV.md`「构建与验证」；复发陷阱查 `TRAPS.md`（按号引用）。
- 节奏：写代码 → 只跑针对的判据 → 注释与格式化 → 最后全量一次（再跑要有理由）。
- 闸：`scripts\format.ps1 -Check` 全合规；`cd web && npm run check` 全 0。
- 硬规则：构建 wasm 与跑判据不并发；跑判据期间不动 `web/`（Vite 热重启）；改 wasm 后必须
  `npm run build:worker`。
- 探针：`p.read` 才拼 H/D 前言（`p.ev` 没有）；`showCaret` 是静默写，断言前要 `D.present()`。

## 方向

派生量收成一份不可变快照；写入口只留一个；红了先找机制，不许记成"偶发"。

## 数据流

修订 `session_` → 投影 `blocks_` → 排版 `layout_` → 页位图 → 画布。光标真源是
`Range selection_`，唯一写入口 `setCaretRel`。引擎里没有帧，只有页位图。

## 不变量

- web（wasm）是主线，native 只当单测宿主；判据必须含反向一半。改帧的入口必须是命令（WRITE），
  worker 的 READ 只回值。
- 块流只有一份；脏区必须带右端；后缀复用只重排 `[from, from+count)`。
- 页指纹混每条行盒的行指纹（空段标记不占字形）；`rasterSignature` 管档位 / 底色 / 缩放 / 资源代际。
- **页指纹必须覆盖控件外观与值**：`content_hash` 取 `controlPaintDigest`，两个编译入口都混。
  只混外观时勾选不重画（☐→☑ 只换码点，成形后同一枚字形）。
- 页位图只有内容，瞬态（光标 / 选区 / 图片框）导出时叠。**单位未统一**：`caret().rect` 设备像素、
  `selection().rects` 磅。行距基准是单倍行高；空段也要有行盒。
- 新增 `Node` 字段同步四处：assignFrom / NodeSpec capture / materialize / 序列化。
- 控件区间只查 `control_spans.h`；"文字位置 + 侧别 = 流位置"两端各有唯一实现，不能互换（229/230）。
- 三档模式只裁三件事：光标能不能停、能不能编辑、隐藏控件是否参与流（222）。
- **光标屏障只挡光标**：点击控件内部也要落点（旧选区塌掉、值照 toggle），从内部起拖的 anchor
  就落在屏障里；插入选择类之后光标落整块之后。
- **折行处行别由流位置定**：贴左边那一格所在的行（`lineAffinityAtFlow`）。`setCaretRel` 的
  `line_aff` 是哨兵：0/1 显式（点击）、-1 自动。移动类命令先 `showCaret(true)` 再移动。
- 图片字节不进文档（宿主解码 → `registerImage`）。拖动改尺寸**不逐帧提交**：只更新瞬态预览框
  （`previewImageResize`），松手才 `resizeImage` 一次；提交要记当前选区（空 `Range{}` 会被撤销
  当成文档开头）。左右中把手纵向铺满整行（图片之后的光标竖线更高），横向不放大。
- 控件记录 = 共同层 + `ControlDetail` 变体；标签是数据的投影（`controlDisplayText`）；特有层整个
  进哈希（`hashControlDetail`）；改定义只走 `ControlPatch`；连打走 `typeTextAndRender`（免得吸旧簇）。
- 选择类记号**全自绘**（内容 ☐/☑，成形换 U+25A1 借格宽）：外径共用 `size_pt * 0.72`，方框
  `strokeRect` / 对勾 `strokePolyline` / 圆环 `strokeCircle` 都是距离覆盖率；颜色
  `Appearance.glyph.color`（alpha 0 = 跟随文字）。未配 `group` 的单选是独立开关。
- 控件括号的槽宽与字形取边界那一侧 run 的字体（`LineCluster::bracket_font`，排版与绘制共用）。
- 值不在内容里的控件（选择类 / 下拉 / 日期）编辑上是整块：贴着它退格删整块，选区不足时扩到整块。
- `blocks_` 不是块流真源（228）；页边距是四个数（227）；滚动总高只有 `scrollHeightPx()` 一条公式，
  画布 CSS 尺寸恒为整页（221）。
- 协议表是唯一动词清单；引擎语义只有 `web/src/engine/api.ts`（零 import），引擎只跑 Worker。
- 字体：身份由文件自报，显示名由宿主给（`label`）；`addFont` 只加脸；同一份只装一次；名字在主线程
  折下标再下发（Worker 下判名字会 15 s 超时）。光栅只有 `bitmap_skia.cpp`（Skia 版本钉在脚本里）。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规 0。
