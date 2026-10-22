# web 测试工程化评估：换成 Playwright 值不值

结论先说：**换，但不是把 4400 行探针翻译一遍。** Playwright 值得买的是场地
（浏览器生命周期、选择粒度、报告与取证、多 project 并行），不是它的断言。断言的迁移是成本，
不是收益 —— 现有探针里的每一条都带着"反向一半"和像素级预算，逐条翻译的唯一确定结果是引入手误。

真正的分界是：**场地可以一次换掉，断言必须按风险从低到高换，最大那份最后换、也可以晚换。**

## 一、现在是什么

| 项 | 数 |
| --- | --- |
| 探针文件 | 8 份（`scripts/*_probe.mjs`），合计 4455 行 |
| 其中 `edit_probe.mjs` | 3818 行 / 36 节 / 367 处断言调用（基线 368 条） |
| 断言总量 | 每传输约 485 条，两条传输合计约 981 条 |
| 第三方依赖 | 0 —— `scripts/cdp.mjs` 160 行，Node 22 自带 WebSocket |
| 入口 | `scripts/verify_web.ps1`：起服务 → 起无头 Chrome → 串行跑一串探针 → 按 profile 杀进程树 |
| 选择粒度 | 整份文件。`-Probe "a.mjs@index.html,b.mjs@multi.html"` |
| 结果 | stdout 的 `[PASS]/[FAIL]` 中文行 + 退出码；只有 `edit_probe` 另写 `out/web/edit-probe.json` |
| 失败取证 | 手写。`out/web/` 下 83 个 png，多数是当时为定位一个问题临时加的 |

已经做对的两件事，迁不迁都要留着：**批量跑共用一次服务与浏览器**（十份 118 s vs 逐次 205 s），
以及**探针自己不许关浏览器**（改由外层按 profile 统一收）。第二件事本身就是"没有框架"的证据 ——
它是一个为了兼容批量模式而加的约定，Playwright 里不存在这个问题，因为没有第二个人共用一个调试端口。

## 二、它能替你做什么

| 现状的痛点 | Playwright 的答案 | 判定 |
| --- | --- | --- |
| 起 Chrome 要自己算：独立 `--user-data-dir`、等调试端口、按 profile 杀进程树、跨次复用 profile 带来的缓存 | 每次 `launch` 一个临时 profile，`context` 隔离，退出自己收 | 净减法。`verify_web.ps1` 能砍掉一半，`cdp.mjs` 的 `disableHttpCache` 也可删（`dev_server.py` 已经发 `no-store`，现在是双保险） |
| 只能按文件选，改一节要重跑整份 | `--grep` / `-G` / `--shard` / `test.only` | **前提是文件已经拆成 test**。这条收益来自拆分，不来自框架 |
| 结果只有文本，`Tee-Object` 写出的日志是 UTF-16LE，grep 不到 | list / json / junit / html 五种 reporter，`--reporter` 可叠 | 真收益。现在那份 1.1 MB 的 `verify_web.log` 用普通工具搜不了 |
| 失败后要靠改探针加打印复现 | `trace: 'retain-on-failure'`：时间轴 + 每步截图 + console + 网络；`screenshot` / `video` 三种保留策略 | 半收益。我们的红多是"值不相等"，DOM 快照没用；时间轴与截图有用，但不改变"要重跑才能看"的基本形态 |
| 两条传输串行跑两遍 | 两个 project 各自并行 | 真收益，前提是并行确实省时间（见下） |
| `multi.html` 两实例同页、`?glue=` 造致命档 | 多 context / 多 page 更省事，`page.route` 可造加载失败而不用改 URL | 小收益 |
| 四条已知恒红（`edit_probe` [18]/[19]，TRAPS 131）靠人记住别管 | `test.fixme` / `annotation` 显式登记，报告里单列 | 真收益。现在这条知识只活在记忆文件里 |

## 三、它不能替你做什么

**并行是真不确定的一笔。** native 分片试过：四片并行只快 20%，因为单节是内存与光栅的重活。
web 这边同理 —— 每个 worker 各自编译一份 4 MB wasm、各自解析 16.9 MB 中文字体、
各自持有页位图。8 逻辑核上 `workers: 4` 能快多少，**没测过就不要写进预期**。
现在的 118 s 里还有大量固定等待（`sleep(400)`、2.4 s 光标闪烁判据），那部分并行能压缩。

**它治不了 `edit_probe` 的病。** 3818 行、36 节、节与节之间靠累积文档状态串起来的叙事。
Playwright 的默认模型是 test 之间隔离 —— 36 个 test 各自新开页面 = 36 次 boot
（每份 1~3 s + wasm 编译），比现在还慢。要拆就必须 `test.describe.configure({ mode: 'serial' })`
配一份共享 page，然后每节开头把状态推到确定点（`diag.reset(id, zoom)` 或
`open(savedJSON)` 都已有）。这件事的难点是状态，不是框架。

**它的断言默认会掩盖本项目最想抓的那一类回归。** 记忆里那条不变量是硬的：
同线程宿主必须同步，异步化回归是靠"调用返回后立刻读"抓到的（当年 14 项红）。
Playwright 的 `expect(locator)` 与 `expect.poll` 都会重试 —— 一个把同步改成异步的改动
在自动重试下会变绿。所以迁移时必须先立规则：**引擎状态判据一律一次性求值**，
用 `expect.soft(cond, msg).toBe(true)`，禁 `poll`、禁 locator 断言。

**像素级预算会被环境细节掀翻。** `edit_probe` 里有一批绝对值断言：方向键 72 px²、
打字 12338 px²、行带复用比例。视口、DPR、滚动条宽度任何一项变了，这些数就变，
而它们现在正是 368 条里最有价值的那部分。

（2026-10-22 复核：这两个数字在迁移后的判据里**不存在**。实际断言全是相对式或宽松下限 —
`maxPx <= full * 1.05`、`> 0`、`dBlue >= 200`、`diff === 0`；上面那句把旧探针 detail 里的
打印值当成了断言。环境敏感性的实测见第十节。）迁移的前置条件是：视口
1500x1200 / DPR 1 / `--hide-scrollbars` 与现在逐项相同，且**旧值就是新基线** ——
同一次运行两套判据的像素账必须逐个相等，不等就是迁移引入的，不是"新基线"。

## 四、分三期落地

| 期 | 范围 | 产出 | 工期 | 通过判据 |
| --- | --- | --- | --- | --- |
| 一 | 场地：`web/tests/` + `playwright.config.ts`，`channel: 'chrome'` 复用已装 Chrome（不下 130 MB Chromium），`webServer` 起 `dev_server.py`，两个 project（inline / worker） | 迁移 `demo_probe`（23）+ `multi_probe`（11）+ `saveload_probe`（11）三份，共 45 条 | 0.5~1 人日 | 45 条逐条对应，名字可改、条数不减；`npm run check` 全绿；lint 扫描列表已含新目录 |
| 二 | 断言与取证：`checkSoft` 包装、`evaluateProbe`（保留现有 async IIFE + quiesce 包装）、失败截图、恒红登记 | `input_probe`（25）+ `diag_probe`（39）+ `error_probe`（19） | 1.5~2 人日 | 同一份已知回归（`docPt` 少除一次 zoom，或 worker `hit` 走 `read` 不走 `ask`）两套 harness 同时红；`input_probe` 的 `TRAIL` 字符串逐位相同 |
| 三 | `edit_probe` 拆 36 节 | `describe.serial` + 共享 page + 每节状态种子；368 条 | 3~5 人日 | 368 条条数不减；像素账与旧探针逐个相等；单节可单独跑（这是这一期唯一值得付钱的东西） |

删掉 `cdp.mjs` 与 `verify_web.ps1` 的时点在第三期收尾。三期结束前两个入口并存，
这正是本项目最反对的"第二条写入口"，所以必须有截止日；没有截止日就别开始。

`stress_probe`（两份，0 条 `chk`，靠 `auditFrame`/`selfCheck` 打印不一致）不进这套 ——
它是压力取证工具，不是验收判据。native 的 `ctest` 四套更不进：那是 C++ 单测，
换框架是另一份评估。

## 五、不要做的事

- **不要 big-bang 重写。** 368 条里有大量"反向一半"（"改过之后确实变了，否则下面那条恒真"），
  翻译时最先丢的就是这类前置校验，而丢掉之后报告依然全绿。
- **不要为 Playwright 改产品语义。** `?glue=` 造致命档、`?transport=` 切传输、
  `?wasm=` 指产物目录，这些是页面契约，不该为了框架改成 `page.route` 注入。
- **不要信"并行能省一半"。** 先在第二期跑一次 `workers: 4` 量墙钟，再决定 CI 上开几个。
- **不要把 `channel: 'chrome'` 换成下载的 Chromium。** 光栅产物、字体回退、滚动条宽度
  都跟着浏览器版本走，而 368 条里有一批逐像素断言。已装 Chrome 是本项目的既有前提
  （`verify_web.ps1` 就是找它），换浏览器是另一件需要重取基线的事。

## 六、反面意见：不换，也能拿到八成

如果只想解决"改一节要重跑整份"，`edit_probe` 加一个 `HEDITOR_PROBE_ONLY="18,29"` 开关
（对齐 native 的 `HEDITOR_TEST_ONLY`）大约 30 行，比框架迁移便宜两个数量级。
再加两条各 20 行的：所有探针统一写 `out/web/<probe>.json`、失败时 `screenshot` 补一张。

这三件事加起来半天，能买到"选择粒度 + 结构化结果 + 失败截图"里的两项半。
买不到的是：trace、project 级并行、恒红登记、报告能被 CI 直接吃。
所以决策点其实是一句话：**这套判据要不要进 CI 门禁。**
要进，Playwright 值得那 5~8 人日；不进，先做上面这三条，框架迁移可以无限期推迟。

## 七、第一期已落地

场地与小探针迁完，判据一条没少：`demo` 23 条、`multi` 11 条、`saveload` 11 条，
每条传输各跑一遍，合计 90 次断言执行。

| 位置 | 作用 |
| --- | --- |
| `web/playwright.config.ts` | 默认口径：Vite 源码直连，`npm test` 走这一份 |
| `web/playwright.dist.config.ts` | 验收口径：跑 `web/dist-app/`，`npm run test:dist` |
| `web/tests/support/config.ts` | 两种口径共用一份配置工厂，差别只有服务、端口、页面路径 |
| `web/tests/support/probe.ts` | `open` fixture 与 `ProbePage.ev`（异步 IIFE + 宿主排空） |
| `web/tests/support/check.ts` | 一次性求值的 `check` / `near`，禁自动重试 |
| `web/tests/specs/` | 三份判据，按 `test.step` 分节，节名沿用旧探针的分节标题 |

三处与评估期判断不同的地方：

默认口径改成源码，不是构建产物。前端生态里 E2E 普遍跑开发服务器（Vue、React 的
模板都是 `webServer: npm run dev`），改一行就能测；打包产物只在验收时跑一次。
因此 `npm test` 只依赖 wasm 与 Worker 壳，不再打包页面与库。

并行度按 2 起步，不是按核数。实测源码口径 6 项 30~36 s、验收口径 20 s，
两边都不是并行占主导 —— 单页启动（wasm 编译 + 三份字体解析）才是分母，这条留给第二期量。

`scripts/lint_comments.py` 的扫描列表已加 `web/tests/**/*.ts`：新目录若不在列表里，
这批代码的注释就无人检查，而它已经有六百多行。

## 八、迁移完成（2026-10-22）

八份探针全部迁到 `web/tests/specs/`，旧入口删除。判据条数与旧探针逐项相等：

| 旧探针 | 条数 | 新 spec | 条数 |
| --- | --- | --- | --- |
| `demo_probe` | 23 | `demo.spec.ts` | 23 |
| `multi_probe` | 11 | `multi.spec.ts` | 11 |
| `saveload_probe` | 11 | `saveload.spec.ts` | 11 |
| `input_probe` | 25 | `input.spec.ts` | 25 |
| `diag_probe` | 39 | `diag.spec.ts` | 39 |
| `error_probe` | 19 | `error.spec.ts` | 19 |
| `stress_probe` | 0（打印不一致） | `stress.spec.ts` | 69 步 + 1 条总判 |
| `edit_probe` | 367 处调用（运行时 368） | `edit.spec.ts` | 367 处调用 |

`edit` 是机械迁移：`c.eval` 换 `probe.ev`、`chk.check` 换 `check`、`report` 换 `state`，
其余代码与注释原样保留。改了四处才跑得起来：页面变量从 `p` 改名 `probe`（`p` 在几十个
局部块里是坐标点，直接换名字会互相遮蔽）、`ev` 的泛型默认 `any`（原代码不写类型）、
模板字符串里被误注入的 `: any`、以及 `CDPSession.send` 要 `bind`（提出来调用丢 `this`）。

删除：`scripts/` 下八份 `*_probe.mjs` 与 `verify_web.ps1`。保留三样，各有理由：
`cdp.mjs` 仍被十八个一次性诊断脚本引用（它们不属于验收面，也不该被这次改动搞坏）；
`web/tests/support/lib_adapter.js` 是编辑与压力 spec 依赖的迁移垫片；
`scripts/probe_serialization_size.py` 与 `probe_encoding_formats.py` 是模型层的分析工具，
与浏览器测试无关。

迁移里两处判据口径要交代，两处都不放宽：

`stress` 的帧差有一处恰好等于新光标矩形（56 px = 2x28）。成因是光标从不可见转为可见时，
增量帧与整帧重画差出一个光标，内容层完全一致。判据对"帧差只落在光标矩形上"补一次
`present` 再问一遍，补完仍差就照算不一致。

逐页滚动的"平移复用"原先要求重画页数严格大于 0。页位图有内容指纹缓存之后，滚进窗口的页
可能整页命中缓存、重画数为 0 —— 那是比部分重画更省的复用，不是没复用。判据改成"重画页数
小于窗口页数"，整窗重建（重画数等于窗口页数）仍会红。

## 九、拆掉迁移垫片（2026-10-22 晚）

`web/tests/support/lib_adapter.js` 已删除。它是 277 行的页面注入脚本，在 `window.__heditor`
上挂出旧门面的 66 个成员，把新库的公开面与诊断面重新摊成"旧宿主的样子"，好让机械迁移过来的
367 条断言先跑起来。现在 `edit` 与 `stress` 直接调 `window.__demo` 与 `window.__demo.diag`。

**留在测试侧的只剩"读数摊平"**，在 `web/tests/support/readings.ts`：一个 `PRELUDE` 字符串，
每次求值拼在代码片段前面，提供 `H` / `D` 两个别名与 `CARET` / `SEL` / `FRAME` / `ZOOM` /
`TYPE` / `MOVE` / `LOAD` / `SET_MODE` 这些测试动作。它与垫片的三点区别：不挂 `window` 全局、
不注入脚本（只活在一次求值的函数体里）、成员从 66 个降到 10 个且名字是测试语义。

**调用形状全部改成新面**，这是拆垫片的主要工作量：

| 旧形状 | 新面 | 处数 |
| --- | --- | --- |
| `setCaret(para, offset)` | `setCaret({ para, offset })` | 83 |
| `setEditMode(0\|1\|2)` | `configure({ mode })` | 16 |
| `caret` 扁平对象 | `caret()` + `pos` / `rect` | 44 |
| `selection` 全局流位置 | `diag.selectionRaw()` | 17 |
| `text` / `allText` | `paragraph(0)` / `text()` | 48 |
| `pagePx[1]` 数组下标 | `diag.pagePx().h` | 12 |
| `zoomLevel` 密度 | `config().zoom x 4/3 x dpr` | 7 |
| `canvas` 帧代理 | 按 `geometry()` 现算 | 6 |
| `type()` 返回记账 | `type()` 之后另读 `diag.editStats()` | 7 |
| `move(-1)` 返回 null | 比 `caretLive()` 前后 | 1 |
| `textarea` | `document.getElementById('ime')` | 8 |

`edit` 改了 643 处求值调用（`probe.ev` → `probe.read`）、`stress` 8 处。367 处 `check(` 一条没动，
断言名只把 `__heditor.selfCheck()` 这类改成 `diag.selfCheck()`。

**`ev` 与 `read` 分开是踩出来的**，不是设计出来的。第一版让 `ev` 无条件拼前言，全量立刻红 10 项：

`demo.spec` 有一条判据要在"没有诊断面"的页面上跑（它验的是公开面只有公开面），前言去读
`__demo.diag`，白名单 Proxy 当场抛"diag 不是公开 API"；`diag.spec` 自己的片段里有
`const H = window.__demo`，与前言里的 `const H` 构成重复声明。所以前言必须是"要的人自己拼"：
`read()` 拼，`ev()` 不拼。

另一处值得记：`LOAD.text(t, r)` 第一版写成 `H.setText(t, o)`，把 `repeat=3` 当成了 options 对象。
`repeat` 静默失效，文档只装了 1 段，`[18]` 整节十一条断言全红 —— 从"全选跨行应该有多个矩形"
一路红到"跨段退格把两段并成一段"。这类错**不会**红在装载那一步，红在十几个断言之后的语义上，
所以换口径时"装载类的参数形状"要单独对一遍。

**页面结构依赖收紧了一道。** 拆完之后第一章查了这件事：`#stage` / `#pages` / `#ime` 不是
示例页的 DOM，是库自己造的骨架（`src/host/dom.ts` 的 `ensureMarkup`，还带一句"缺了就抛"的
检查）。所以读数前言里不再写 `document.querySelector('#ed #pages')` 这种"页面容器叫什么"
的路径，统一走 `H.element.querySelector(...)`。现在对页面只剩下一个约定：`window.__demo`
是页面交给外界的实例句柄（`evaluate` 在页面里跑，拿不到模块引用，这是唯一可行的交接方式），
示例页那行注释写明了"改名字要同步前言"。

拆完实测：全量 16 项 3.5 min 绿（源码口径），`npm run check` 绿。`lib_adapter.js` 存在时
那 10 项也是绿的 —— 但那时 `edit` / `stress` 证明的是"垫片 + 新库"，现在证明的是新库。

## 十、像素判据怕不怕换环境（2026-10-22 实测）

疑问是合理的：一套判据如果只在某台机器上绿，它的价值就要打折。所以把这件事拆成三层看，
再实测一次。

**三层判据的环境依赖性不一样：**

| 层 | 形态 | 依赖什么 |
| --- | --- | --- |
| 引擎自审 | `auditFrame().diff === 0`，把当前帧与"整帧重画一遍"逐字节比 | 只依赖 wasm 里的 Skia，跟浏览器、宿主 GPU 无关 |
| 屏幕 vs 引擎 | `selfCheck().screenVsEngineDiffPx === 0`，贴上去再读回来 | 依赖浏览器的 `putImageData` 保真度 |
| 变化量 | `dBlue >= 200`、`maxPx <= full * 1.05`、`> 0` | 阈值宽松，跟具体像素数无关 |

**实测：把 `deviceScaleFactor` 从 1 临时改成 2，跑全量 16 项。**

结果 12 过 / 4 红（`edit` 与 `input` 各两条传输）。红的是这些：

- `真实鼠标横扫：命中偏移与归属都等于点击后的实际落点`（117 个采样点全不一致）
- `拖选产生非空选区`、`拖出可视区仍继续扩展`、`焦点在锚点右侧`、`点击后光标仍在这一段`

**一条像素账都没红。** 单独量了一遍 DPR=2 下的核心读数：`screenVsEngineDiffPx` 仍是 0、
`auditFrame().diff` 仍是 0、帧尺寸还是 893×1284（与 DPR=1 逐项相同）。所以那批"贴上去再读回来"
的判据对 DPR 不敏感 —— 光栅在 wasm 里，字体随仓库走（16.9 MB 中文字体），DPR 又由 Playwright
模拟、不读宿主屏幕。

红的是**测试自己的坐标换算**：那批"帧磅 → 屏幕像素"的算式在 DPR=1 时两种单位恰好相等，
DPR 一变就把差值暴露出来。这不是判据脆弱，是测试代码里对单位的假设没被写出来 —— 换句话说，
把 DPR 钉在 1 的理由不是"像素账会飘"，而是"坐标换算只在 1 下验过"。

**结论，分三种换环境：**

换台电脑：基本稳。字体不依赖系统、光栅不依赖 GPU、DPR 由 Playwright 模拟、视口写死、
滚动条 `--hide-scrollbars`。剩下唯一的不确定是 CPU 浮点边界（Skia 里极少数取整），
没有实测到，但不影响"逐像素相等"这类同进程比较 —— 两边都是同一颗 CPU 算的。

换浏览器：没建立过基线，所以不知道。`channel: 'chrome'` 是明确选择：被测的是"我们的引擎
输出什么"，不是"浏览器怎么排版"，跨浏览器的成本是每个浏览器一份基线。真要跨，先做的是把
坐标换算里的 DPR 收成一处（与宿主 `docPt` 同口径），否则第一个撞上的还是那批拖选判据。

浏览器版本变了：这是唯一会自己变的变量（`channel:'chrome'` 用已装的 Chrome 且自动升级）。
升级后要复跑一次。如果 `screenVsEngineDiffPx === 0` 红了，那是"上屏保真度真的变了"，
该由人判断是浏览器变了还是我们变了 —— 这条判据守的就是这件事，不该放宽。
