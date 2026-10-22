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
而它们现在正是 368 条里最有价值的那部分。迁移的前置条件是：视口
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
