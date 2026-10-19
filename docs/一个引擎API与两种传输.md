# 一个引擎 API 与两种传输

本文是方案，不是现状。现状（两套 Web 宿主为何并存、各自验收数字）见
`docs/正式化架构与迁移路线.md` 4.6.1、4.6.2。本文只回答一个问题：
**能不能把两宿主收成一套，同时保住 in-thread 那条。**

结论：能，但"一套"的正确粒度是"一个引擎 API + 一个宿主主体 + 两个传输实现"，
不是"一个文件里加 mode 分支"。真正需要合并的只有引擎访问面；事件层与呈现层有
本质差异，强行合并会得到一个到处判断传输方式的文件，比两份更难维护。

## 1. 现状的量化

先把"重复"这件事量出来，否则方案会建在错误的假设上。

| 度量 | 数值 | 取法 |
| --- | --- | --- |
| `web/app.js` | 2652 行 / 131 KB | `wc -l` |
| `web/worker_host.js` | 1898 行 / 95 KB | 同上 |
| `web/engine_worker.js` | 1258 行 / 62 KB | 同上 |
| 两宿主同名顶层函数 | 11 个 | 见附录 A |
| 其中完全一致 | 2 个（`armDprWatch` 5 行、`clickCountOf` 10 行） | 逐行比对 |
| `engine_worker.js` 命令表条目 | 79 | `WRITE` + `READ` 两块 |
| `app.js` 直接调 `mod.*` | 96 个方法 / 261 次 | 正则统计 |
| 两边命令名的字面交集 | 35 | 集合运算 |
| 仅 `app.js` 直调 | 61 | 同上 |
| 其中与表内条目同义不同名 | 33 | 人工配对，见附录 B |
| 表内完全没有对应命令 | 24 | 同上 |
| 特权访问（不能进表） | 4 | `FS` / `HEAPU8` / `allocBuffer` / `freeBuffer` |

两条结论：

- **抽公共函数这条路是死的。** 同名函数 11 个、完全一致 2 个，其余相似度 3%～80%。
  两边不是复制粘贴，而是同构改写：同一个"点击 → 命中 → 落光标"在三处因为画布所有权、
  滚动符号、结果何时到达而各自变形。把能抽的抽干净，收益约 15 行。
- **重复的真实位置是引擎访问面，而且是两层重复。** 第一层是命名：命令表用短名加参数
  对象（`type` / `move` / `undo` / `caretStyle`），`app.js` 用 wasm 原名加位置参数
  （`typeText` / `moveCaret` / `undoEdit` / `caretStyleJson`），61 个里 33 个是同一件事
  的另一个名字。第二层是覆盖：帧几何（`frameRgba` / `frameWidth` / `frameHeight` /
  `frameSlots` / `frameByteSize`）、页度量（`pagePxWidth` / `pagePxHeight` / `pageGapPx` /
  `setPageMetrics`）、光标即时读数（`caretOffsetLive` / `caretVerticalNow` /
  `caretBlockReversedNow`）这几组表里根本没有，只能由 in-thread 直调。

  命名这一层比覆盖更值得先处理：它不涉及语义，却让"读一份代码要会两套名字"成为常态。
  统一命名之后，缺口会从 61 缩到 24，剩下的才是真的需要补的命令。

## 2. 先例

### 2.1 VS Code 的 Channel 与 ProxyChannel（形式直接对应）

VS Code 把所有跨边界服务登记成 channel，用四种传输实现同一套接口：Electron IPC、
Node IPC、MessagePort、Web Worker（Web 版扩展宿主就是 worker）。`ProxyChannel`
把任意实现代理成 channel，调用方看到的接口与实现在哪一侧无关。其文档与源码注释
的原话是：IPC 模块天然异步，因此不区分本地进程还是远端进程，只要通过 channel 通信，
就可以复用相同的代码编写。

**适用。** 本项目要的正是这个：命令表是协议，传输是可替换实现。照抄它的三层
（协议 / channel 抽象 / 传输实现）即可，不必自创。

**不适用处，也是本项目唯一的偏离**：VS Code 全部调用都是异步的，没有"同步读一个值"
的需求。本项目 `app.js` 的 261 次调用里绝大多数是同步取值（`mod.pagePxHeight()` 这类），
而这一点是 in-thread 宿主作为对照物的全部价值来源。方案里必须显式处理它（见 5.2），
不能假装不存在。

### 2.2 Monaco（反证：引擎全部搬进 worker 不是通例）

Monaco 的划分是：文本缓冲、视图、光标在主线程，只有语言服务（补全、校验、类型检查）
进 worker。它能在 worker 侧做的原因是那些都是"纯函数式的分析"，结果可以异步回来。

**不适用。** 本项目的引擎同时做模型、排版与光栅，搬进 worker 之后主线程拿不到任何
文档状态，于是所有读取都退化成镜像热读。这是本项目独有的取舍，不能拿 Monaco 的做法
论证"应该把模型留在主线程"——那会让滚动与排版重新回到主线程。

**可借用的一条**：Monaco 用 `MonacoEnvironment.getWorkerUrl` 这一层间接，把
"worker 产物从哪来"与"编辑器逻辑"分开。本项目对应的是 `glueUrl()` 与
`?wasm=` 产物目录，已经具备。

### 2.3 Comlink（可借形态，不可直接用）

Comlink 用 Proxy 把对象映射成 Promise 代理，是最轻的 worker RPC。

**不适用两条**：它把每次调用都变成 Promise，无法表达"命令发出去就不管"的交互路径；
它只代理调用，不代理画布与帧的转移，而本项目的核心约束是帧不跨线程。

**可借一条**：调用编号与挂起表的配对形态（`id → {resolve, reject, timer}`）。
`worker_host.js` 已经有了，不必改。

### 2.4 `transferControlToOffscreen` 的浏览器支持

MDN 现行数据：Chrome 69 / Edge 79 / Firefox 105 / Safari 16.4（含 iOS 16.4），
Baseline 标注为 widely available since March 2023。

**结论：不要再拿"老浏览器没有 OffscreenCanvas"当作保留两套的主要理由。** 它今天只
影响 2023 年 3 月之前的浏览器。in-thread 宿主要保，理由只能是"行为对照物"。这条
修正直接影响第 7 节的取舍。

## 3. 目标形态

```
web/engine_api.js        命令表 + 引擎状态 + 窗口/视图计算        （唯一）
web/host_core.js         DOM 装配、键盘、IME、命中、滚动、工具栏   （唯一）
web/transport_inline.js  本地传输：同步执行，立即返回              （约 200 行）
web/transport_worker.js  远程传输：postMessage + 挂起表 + 镜像      （约 250 行）
web/engine_worker.js     worker 薄壳：importScripts + 转发          （约 120 行）
```

职责边界，按"谁拥有什么"划分，不按"代码属于哪个页面"划分：

| 文件 | 拥有 | 不拥有 |
| --- | --- | --- |
| `engine_api.js` | wasm 句柄、缩放密度、视口尺寸、窗口范围、页几何 | DOM、事件、画布 |
| `host_core.js` | 容器内元素、事件绑定、镜像副本、工具栏接线、门面对象 | wasm 句柄（除 in-thread 特权三项） |
| 传输实现 | 命令如何到达 `dispatch`，结果如何回来 | 命令语义 |

`engine_api.js` 必须是纯的：不碰 `document`、不碰 `window`、不碰 `self.postMessage`。
它是唯一能在两种宿主里跑同一份代码的文件，一旦它碰了宿主全局，这个性质就没了。

名字说明：`transport_inline.js` 在路 A 下其实不承担传输，它只负责装载 wasm 与字体、
建出 `engine_api` 实例、暴露同步入口。叫 transport 是为了路 B 下不换文件名（5.2）。

## 4. 引擎 API 的接口

### 4.1 命令表

```js
/**
 * 引擎命令表：宿主唯一允许的引擎访问面。
 *
 * kind 决定它是否改变可见状态，传输层据此决定要不要等结果；run 收引擎句柄与参数，
 * 返回 { value, changed }，value 必须是可结构化克隆的普通值，不允许返回 wasm 堆视图。
 */
const COMMANDS = {
  typeText: {
    kind: 'write',
    run: (e, a) => ({ value: e.mod.typeText(a.s), changed: true }),
  },
  caretStyleJson: {
    kind: 'read',
    run: (e) => ({ value: JSON.parse(e.mod.caretStyleJson()), changed: false }),
  },
};
```

三条硬约束：

- **命令名只有一份**。宿主侧不再出现 `mod.xxx` 字面量（四个特权访问除外）。
  新增引擎能力时只动这张表，两个宿主自动同时具备。
- **`value` 必须可结构化克隆**。`frameRgba` 因此**不进表**：它返回的是 wasm 堆视图，
  只在持有该 wasm 实例的一侧有效，跨线程拿到的指针没有意义。它由 `sink` 自己取
  （见 5.1），`engine_api` 只提供 `frameGeometry`（页宽、页高、字节数）。
- **未知命令必须抛错**，不返回 `undefined`。静默的空值会让"命令名拼错"变成"功能没反应"，
  后者在探针里表现为超时而不是失败原因。

### 4.2 返回与错误

同步返回统一为 `{ value, changed }`，并对外给三个入口，省掉调用点到处写 `.value`：

| 入口 | 返回 | 用途 |
| --- | --- | --- |
| `dispatch(name, args)` | `{ value, changed }` | 传输层唯一需要的东西 |
| `read(name, args)` | `value` | 读命令，等价于 `dispatch(...).value` |
| `write(name, args)` | `changed` | 写命令，等价于 `dispatch(...).changed` |

写命令的 `changed` 由 `engine_api` 自己算，不由宿主传。
传输层负责包装：in-thread 直接给回，worker 包成 Promise。**门面是否统一返回
Promise 是 5.2 的取舍，不在这一层决定。**

错误沿传输原样回传 `{ where, message, stack }`，宿主侧还原成 `Error` 并在
`host_error.js` 里按现有两档（fatal / warn）分级，分级规则不变。

### 4.3 状态归属

`engine_api` 持有：`dpr`、`zoomDensity`、`viewW/viewH`、`scrollY`、`scrollLead`、
`windowFirst/windowCount`、页几何。宿主只读镜像，不维护第二份。

这条是消除"两边各算一遍窗口"的前提。今天的 `desiredWindow` / `targetWindow` /
`applyWindow`（`app.js`）与 `desiredWindow` / `targetWindow` / `retarget`
（`engine_worker.js`）是同一个算法的两份，且已经出现参数名不同的漂移。

### 4.4 两件事：先统一命名，再补缺口

**统一命名**是前置动作，因为它不涉及语义、却是"读一份代码要会两套名字"的根源。做法是
命令表沿用 `app.js` 的 wasm 原名（`typeText` / `moveCaret` / `undoEdit` /
`caretStyleJson`），废弃表里的短名（`type` / `move` / `undo` / `caretStyle`）。选原名
而不是短名的理由：引擎的导出名是 C++ 侧的权威命名，短名只是宿主侧的手写别名，
保留短名等于让引擎 API 有两套词汇。

配对样本（完整 33 对见附录 B）：

| 表内短名 | app 直调原名 |
| --- | --- |
| `type` | `typeText` |
| `move` | `moveCaret` |
| `vertical` | `moveCaretVertical` |
| `undo` / `redo` | `undoEdit` / `redoEdit` |
| `caretStyle` | `caretStyleJson` |
| `stats` / `editStats` | `statsJson` / `editStatsJson` |
| `controls` | `controlsJson` |
| `dirtyPages` | `dirtyPagesJson` |
| `docStart` / `docEnd` | `caretToDocStart` / `caretToDocEnd` |
| `mergePrev` / `mergeNext` | `mergeWithPreviousParagraph` / `mergeWithNextParagraph` |
| `stress` / `textDoc` | `loadStressDocument` / `loadTextDocument` |
| `scenario` / `formSample` | `setScenario` / `loadFormSample` |

**补缺口**共 24 条：

| 组 | 命令 | 数量 |
| --- | --- | --- |
| 帧几何 | `frameRgba` / `frameWidth` / `frameHeight` / `frameSlots` / `frameByteSize` | 5 |
| 页度量 | `pagePxWidth` / `pagePxHeight` / `pageGapPx` / `setPageMetrics` | 4 |
| 呈现与视图 | `relayoutAndPaint` / `setViewportAnchor` / `showCaret` / `setCaretStyle` / `setFrameSlots` | 5 |
| 光标即时读数 | `caretOffsetLive` / `caretVerticalNow` / `caretBlockReversedNow` | 3 |
| 窗口 | `windowFirstPage` / `windowPageCount` | 2 |
| 诊断 | `rasterBackend` / `buildStamp` | 2 |
| 文档文本 | `documentText` | 1 |
| 选区 | `selectionReport` / `setSelectionStyle` | 2 |

另有 4 个**不进表**：`FS`（字体文件写入）、`allocBuffer` / `freeBuffer`（wasm 堆）、
`HEAPU8`。它们是 in-thread 特权访问，worker 侧不存在对应的主线程堆。这 4 个在步 3 之后
仍然是 `app.js` 里仅剩的直接访问。

## 5. 三个不可合并的分叉

这三条是本质差异，参数化之后仍然是两段代码。承认它们，才不会写出满屏 `if`。

### 5.1 帧去哪：像素汇

```js
/**
 * 一页的像素落到哪。
 *
 * in-thread 直接写页面画布；worker 写交给自己的 offscreen 上下文。两者的目标坐标口径
 * 相同（页号乘步距减滚动量），差别只在上下文从哪来。
 */
const sink = {
  putPage(page, view, dy) {},
  clearPage(page) {},
};
```

`engine_api` 算脏页集合与目标坐标；帧视图由 `sink` 自己从引擎取（视图只在持有该
wasm 实例的一侧有效，不能跨传输）。今天的 `blitFrame`（`app.js`）与 `blitPage` /
`blitPageContinuous`（`engine_worker.js`）在"算目标位置"这一段是同一件事，可以进
`engine_api`；只剩取视图与 `putImageData` 那一行分开。

### 5.2 结果何时到达：两条路，必须选一条

这是唯一会改变调用方写法的分叉。两条路都成立，但不能混。

**路 A：保留同步**（推荐，配合第 7 节做到步 3）

- `engine_api.dispatch()` 同步返回 `{ value, changed }`。
- in-thread 宿主原样同步读；worker 宿主的门面把同一命令包成 Promise 并走消息。
- 调用点写法因此不同（一个直接取，一个 `then`），`host_core` 只能共享命令名，
  不能共享事件层。
- 收益：in-thread 保留"结果在同一个调用栈内就绪"，同步调试与对照的价值不变。
- 代价：步 4 做不了，事件层的重复继续存在。

**路 B：一律异步**（只有决定做步 4 时才选）

- 门面方法一律返回 Promise；in-thread 传输在同一个 tick 内执行并 resolve。
- 交互路径仍然不 `await`，`then` 里只做镜像更新。
- 收益：事件层可以合并成一份。VS Code 那条路的前提正是"全部异步"（见 2.1）。
- 代价：in-thread 不再完全同步。它仍在、仍独立，但"同一调用栈内就绪"这个性质没有了。

取舍判据很简单：**不做步 4 就不要选路 B**。路 B 付出的是同步对照，换回来的是事件层合并；
事件层不合并，这笔代价就白付。按第 7 节做到步 3 为止，走**路 A**。

### 5.3 标记契约：三条必须由宿主页声明

（只有在路 B 下 `host_core` 才存在。路 A 下这张表仍然有用，它是两个宿主页的差异对照。）

`host_core` 收一个 `opts.layout`，它决定三件事，这三条在 4.6.2 里已经写明是
"写宿主页时必须知道"的：

| 项 | `inside`（in-thread） | `overlay`（worker） |
| --- | --- | --- |
| 输入层挂法 | 滚动容器内部，滚动位移加在坐标上 | 滚动容器的兄弟覆盖层，位移减去 |
| 画布居中 | 由页面 CSS 给（`justify-content: center`） | 宿主自己算左边距 |
| 画布所有权 | 宿主自持 `<canvas>` | `transferControlToOffscreen` 交给 worker |

前两条是纯参数；第三条决定 `sink` 的实现，与 5.1 是同一件事的两面。

## 6. 迁移步骤

每步都可停，每步都有可证伪判据。判据一律以 wasm 侧的浏览器探针为准。

### 步 1：抽出 `engine_api.js`

把 `engine_worker.js` 的分派、状态、窗口计算、页几何搬进新文件；`engine_worker.js`
只剩 `importScripts` 与 `onmessage` 转发。行为不变。

判据：`worker_probe` 68 项 0 红；`edit_probe` 在 `worker.html` 上项数与 0 红同前；
`stress_probe` 69 次点击 0 不一致；`web_probe` 6 场景结构列一致。

反向一半：`engine_worker.js` 里除 `importScripts` 与 `glueUrl` 外不得再出现 `mod.`
（`grep -n "mod\."` 只剩这两处）。仍有残留说明分派没搬干净，"一份 API"就只是名义上的。

### 步 2：统一命名并补齐 24 条命令

按 4.4 先把表里的短名换成引擎原名，再补 24 条。改名与新命令都不动调用方。
新增命令各配一条最小探针断言，避免"加了但没人用、也没人验"。

判据：新命令在 worker 页可调且返回形状与 `app.js` 直调同一方法一致。做法是在
`worker_probe` 里对同一命令与 in-thread 同方法各取一次值做结构比对。

### 步 3：`app.js` 改调命令表

261 处替换，机械但需要逐个核对返回形态。按三类处理：

| 现有形态 | 处理 |
| --- | --- |
| `JSON.parse(mod.xxxJson())` 一类（`editStatsJson` 单是 29 次） | 命令表侧已经 `JSON.parse`，调用点去掉外包 |
| `mod.pagePxHeight()` 这类标量 | 替换为 `api.read('pagePxHeight')` |
| `mod.FS` / `mod.allocBuffer` / `mod.freeBuffer` / `mod.HEAPU8` | 不动，in-thread 特权 |

判据：`edit_probe` 在 `index.html` 上 361 项 0 红（`edit_probe` 的断言是具体像素与文本，
不是"与另一宿主相同"，所以这一步的真判据就是它全绿）；`grep -n "mod\."` 在 `app.js`
里只剩特权访问那几处。

反向一半：替换后若某条命令的返回值形态与原直调不同，探针必须变红。验证办法是故意
把一条命令的 `JSON.parse` 去掉，`edit_probe` 应出现类型错误而不是静默通过。

### 步 4：合并 `host_core.js`（可选项，前提是切到路 B）

实施前提是把门面切到路 B（一律 Promise）。不切直接合并，得到的是"看起来一份、
实际两套分支"的代码，比两份更难读。

切完之后事件层收成一份，两个传输各自注入。这一步的重排量最大（约 4400 行 → 3000 行），
收益是"新增交互只写一遍"。

判据：四份探针在两宿主各跑一遍全绿；`stress_probe` 两宿主的现场帧逐像素一致；
`multi_probe` 的混合三实例仍全绿。

回退点：任一步的探针出现无法在当轮解释的红，回到上一步的产物，不做"边修边合"。

## 7. 建议的落点

**推荐做到步 3 为止。**

理由：步 1～3 解决的是用户实际感知的繁琐——同一件事三个写法、命令表覆盖不全、
加功能要动两处。做完之后 `app.js` 与 `worker_host.js` 各剩 800～1000 行纯 DOM 与事件
代码，差异全部落在三个已知分叉上；`app.js` 不再出现 `mod.*` 字面量（四个特权访问除外），
它对引擎的全部知识收敛到那张表里，"第二个产品"的风险随之消失。

剩下的判断只有一条：事件层要不要合并。要合，就选路 B 并付那笔代价（5.2）；
不合，步 1～3 就是终点。

步 4 的收益递减：剩下的是事件层，而事件层的差异（滚动的符号、输入层挂法、命中结果的
到达时刻）真正存在，合并后仍要分支。它值得做的前提是"交互功能还在快速增加"，
今天不是。

## 8. 明确不做

- 不合并成一个 `createEditor(container, { transport })` 单入口。两个工厂名
  （`createEditor` / `createWorkerEditor`）保留，各自只做"选一种传输 + 起 `host_core`"。
  合并入口会掩盖 5.3 那三条标记契约，写宿主页的人更容易漏。
- 不把 `engine_api` 做成 ES module。三张宿主页都用 `<script>` 直接加载，引入模块
  边界会让 `file://` 与无打包的调试路径失效。沿用全局挂载（`window.__heditorEngine`）
  与 IIFE 隔离。
- 不引入 Comlink 或任何 RPC 库。现有挂起表工作正常，且库无法表达帧与画布的转移。
- 不为 in-thread 补 `quiesce` 的假实现。它没有异步队列，`quiesce` 返回真即可，
  现有探针已经这样处理。
- 不因为步 3 就删掉 `index.html` 的验证台面板。那些面板（bench、重绘计数、表单变体、
  构建时间）是手工诊断入口，与宿主合并无关；要删是另一件事。

## 附录 A：两宿主同名函数（11 个）

`armDprWatch`、`clickCountOf`（完全一致）；`cleanMarkdownLines`、`dragExtendTo`、
`framePointAt`、`frameSlotOf`、`onCompositionEnd`、`onDprWatch`、`onKeyDown`、
`positionIme`、`syncPageCanvases`（相似度 3%～80%）。

## 附录 B：仅 `app.js` 直调的 61 个方法

数据来源：`app.js` 中 `mod.<name>` 去重后，与 `engine_worker.js` 命令表的键做差集。
61 = 同义不同名 33 + 表内无对应 24 + 特权 4。另有 44 个命令表条目 `app.js` 未直调
（`ping` / `heap` / `inkRows` / `frameDigest` / `blinkPhase` 等），属 worker 宿主专用或
调试用，合并时原样保留。

### B.1 表内无对应命令（24 条，需新增）

| 组 | 命令 |
| --- | --- |
| 帧几何 | `frameRgba` / `frameWidth` / `frameHeight` / `frameSlots` / `frameByteSize` |
| 页度量 | `pagePxWidth` / `pagePxHeight` / `pageGapPx` / `setPageMetrics` |
| 呈现与视图 | `relayoutAndPaint` / `setViewportAnchor` / `showCaret` / `setCaretStyle` / `setFrameSlots` |
| 光标即时读数 | `caretOffsetLive` / `caretVerticalNow` / `caretBlockReversedNow` |
| 窗口 | `windowFirstPage` / `windowPageCount` |
| 诊断 | `rasterBackend` / `buildStamp` |
| 文档文本 | `documentText` |
| 选区 | `selectionReport` / `setSelectionStyle` |

### B.2 同义不同名（33 个 app 方法对应 26 个表内条目，需改名统一）

| 表内现有名 | `app.js` 直调名 |
| --- | --- |
| `type` | `typeText` |
| `move` | `moveCaret` |
| `vertical` | `moveCaretVertical` |
| `undo` / `redo` | `undoEdit` / `redoEdit` |
| `docStart` / `docEnd` | `caretToDocStart` / `caretToDocEnd` |
| `lineStart` / `lineEnd` | `caretToLineStart` / `caretToLineEnd` |
| `mergePrev` / `mergeNext` | `mergeWithPreviousParagraph` / `mergeWithNextParagraph` |
| `caretStyle` | `caretStyleJson` |
| `selectionStyle` | `selectionStyleJson` |
| `stats` / `editStats` | `statsJson` / `editStatsJson` |
| `controls` | `controlsJson` |
| `dirtyPages` | `dirtyPagesJson` |
| `bracketSlots` | `bracketSlotsJson` |
| `hit` | `hitTest` |
| `controlAt` | `controlAtPoint` |
| `removeControl` | `removeControlAt` |
| `setControlProp` | `setControlPropAt` |
| `bench` | `benchRelayoutPaint` |
| `prewarmPage` | `prewarmPageGlyphs` |
| `scenario` | `setScenario` |
| `zoom` | `setZoom` |
| `formSample` | `loadFormSample` |
| `stress` / `stressDiverse` | `loadStressDocument` / `loadStressDocumentDiverse` |
| `textDoc` | `loadTextDocument` |
| `fonts` | `loadFont` / `loadFontFromFile` / `setFontRoles` |

### B.3 特权访问（4 条，不进表）

`FS`（字体文件写入 wasm 文件系统）、`allocBuffer` / `freeBuffer`（wasm 堆）、`HEAPU8`。
worker 侧没有主线程堆，这四条在步 3 之后仍是 `app.js` 里仅剩的直接访问。
