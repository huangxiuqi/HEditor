# 项目长期记忆（HEditor）

**最高约束**：不执行 git 写入命令；不删除/移动 `.git`；`memory/` 只追加。
**阶段约定**：开发期不考虑向后兼容 —— 改名、删选项、换形状直接做，判据与文档一起改。
**索引**：`TRAPS.md`（复发陷阱 1~230，按号引用）· `ARCH-NOTES.md` · `ENV.md` ·
`MIGRATION.md`；语义权威 `docs/文档模型设计.md`（行数不设固定值，改完看
`git diff --stat` 的增删是否就是自己写的那几行）、`docs/外部API设计.md`、`docs/跑测试.md`。

## 环境与基线

- native：`.\scripts\msvc_env.ps1` → `cmake --build --preset native` →
  `ctest --preset native`（7 个用例四片并行，约 50 s）。耗时比值判据单独跑
  `ctest --preset native-perf`（RUN_SERIAL，约 6 s）。wasm：`cmake --build --preset wasm`
  （tsc 是硬依赖，TRAPS 153）。单节调试 `HEDITOR_TEST_ONLY="70,72"`；
  `HEDITOR_TEST_SHARD="1/4"` 取一片，`HEDITOR_TEST_TIMES=1` 打印每节耗时（填权重要它）。
- 浏览器判据 `cd web && npm test`（dev）/ `npm run test:dist`（验收），19 份 spec
  （编辑面按域拆七份）。**trace 默认关**，要回放设 `HEDITOR_TEST_TRACE=1`；并行度 4。
  性能判据在 `web/tests/perf/`，`npm run test:perf` / `test:dist:perf`，单 worker 独占。
  硬规则：引擎状态一次性求值（禁 `expect.poll`）、`channel:'chrome'`、
  **构建 wasm 与跑判据不并发**（198）、**跑判据期间不动 `web/`**（Vite 热重启）；
  临时诊断写 `tmp.spec.ts`，用完即删。改 wasm 后 `npm run build:worker`
  （Worker 壳是构建产物，源码跑不起来）。
- 格式化全局做 `scripts\format.ps1`，`-Check` 必须"全部合规"，之后重建两侧重跑判据。
- 静态闸 `cd web && npm run check`（载荷 / 公开成员 / 注释 R1~R13 / TS+C++ 文档 /
  idl / api / tsc）必须全 0。

## 验证节奏（用户 2026-10-25 明确要求）

**先写代码 → 只跑针对的那几条判据 → 注释与格式化 → 最后跑一次全量。**
- 针对：改哪个 spec 就跑哪个（native 用 `HEDITOR_TEST_ONLY` 点名几节）；红了先看那一节。
- **web 侧全量只跑 `npm run test:dist`**（验收口径，顺带重建产物），不再 dev、dist 各一遍；
  dev 只用来快速验单个 spec。同一轮改动跑第二遍全量必须有理由。
- 耗时比值判据已独立成 perf（native 第 78 节 / web `tests/perf/`），跑它们时机器上不许
  有别的活；红了先查负载，别为它改代码。

## 方向

- 派生量收成一份不可变快照，不留第二份可写真相；影响面由产生它的那一步带下去。
- 写入口只留一个；绕开提交路径的第二条路迟早分叉。
- 红了先找机制，不许记成"偶发"。

## 数据流

修订 `session_` → 投影 `blocks_` → 排版 `layout_` → 页位图 → 画布。光标真源是
`Range selection_`，写入口只有 `setCaretRel`。引擎里没有帧，只有页位图；页位置 =
页号 × 步距（宿主算）。

## 不变量

- web（wasm）是主线，native 只当单测与审计宿主；判据必须含反向一半。
- 会改帧的入口必须是命令（WRITE）；worker 的 READ 只回值，不 present、不 blit。
- 块流只有一份；排版脏区必须带右端；后缀复用只重排 `[from, from+count)`。
- 页指纹要混进每条行盒的行指纹（空段标记不占字形，208）；`rasterSignature` 管档位 /
  底色 / 缩放这类外因（128）。
- 页位图只有内容，瞬态（光标 / 选区）导出时叠。**单位未统一**：`caret().rect` 是设备像素，
  `selection().rects` 是磅，判据送去 `diag.hit`（磅）要先折。
- 行距基准是单倍行高（ascent + descent），不是字号。空段也必须有行盒。
- 新增 `Node` 字段同步四处：assignFrom / NodeSpec capture / materialize / 序列化。
- 控件区间只有一个查询面 `core/engine/control_spans.h`；"文字位置 + 侧别 = 流位置"两端
  各有唯一实现，不能互换（229/230）。
- 三档模式只裁三件事：光标能不能停、能不能编辑、隐藏控件是否参与流（222）。
- 图片字节不进文档：宿主解码 → `registerImage`（注册时预乘 alpha）；页指纹看不见像素，
  换图靠 `rasterSignature` 里的资源代际作废页位图；拖拽改尺寸用 `rev::MergeRun` 合成一步撤销。
- 控件记录 = 扁平共同层 + `ControlDetail` 变体（每族只带自己那组）；选择类的标签是数据的
  投影（`controlDisplayText` 生成内容），不是作者打的字；光标屏障三档都生效，选区不受限。
- 控件记录的特有层必须整个进哈希（`hashControlDetail`）：修订层的"没变"按根哈希判，漏一个
  字段那一改就被丢掉（画面不变、不报错）。改定义只走 `ControlPatch` 一条路，改完重建内容
  并清掉指向已删选项的值。写命令不重排：连打要走 `typeTextAndRender`，旧簇表不吸附。
- 选择类的记号（方框/对勾/圆圈）由绘制层画，**不是字体字形**：内容用 ☐/☑（U+2610/2611），
  成形时换成 U+25A1 只为借格宽。颜色取 `Appearance.glyph.color`（alpha 0 = 跟随文字）。
- 值不在内容里的控件（选择类/下拉/日期）在编辑上是**整块**：贴着它退格/前删删整块，
  选区只盖住一部分时扩到整块；插入选择类之后光标落整块之后（否则连插两次会长出嵌套）。
- **页指纹必须覆盖控件外观**：`InlineObject::content_hash` 进 `controlAppearanceDigest`，
  行指纹混它（两个编译入口都要：`doc/compile.cpp` 与 `rev/project.cpp`）。
- `blocks_` 不是块流真源（228）；页边距是四个数（227）；滚动总高只有
  `host/pages.ts` 的 `scrollHeightPx()` 一条公式，画布 CSS 尺寸恒为整页（221）。
- 协议表是唯一动词清单（`COMMANDS` + 三张实现表 + `OUTSIDE`）；引擎语义只有一份
  `web/src/engine/api.ts`（零 import），引擎只跑 Worker。
- **字体**：身份（族名 / PostScript 名 / 文件名）由字体文件自己报（wasm `fontsJson`），
  **显示名由宿主给**（`label`，不传等于族名）—— 字体里没有中文名，界面那一行只能由注册方
  写。`addFont` 只加脸、不重排不重画；同一份（装载方式 + 地址）只装一次；名字在主线程折成
  下标再下发（引擎判名字在 Worker 下会变成 15 s 超时）。`reloadFonts`（diag）才是重装一遍。
- 存档字体身份是 PostScript 名 → 族名 → 文件名；文件格式 v3。
- 光栅只有一个实现 `bitmap_skia.cpp`；Skia 版本钉在 `scripts/skia_rev.ps1`，
  唯一取数入口 `scripts/skia_fetch.ps1`。
- 注释权威 = 根 `AGENTS.md` + `lint_comments.py`（R1~R13），违规 0、不设基线。
