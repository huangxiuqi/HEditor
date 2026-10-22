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
| 其中与表内条目同义不同名 | 36 | 人工配对，见附录 B |
| 表内完全没有对应命令 | 21 | 同上 |
| 特权访问（不能进表） | 4 | `FS` / `HEAPU8` / `allocBuffer` / `freeBuffer` |

两条结论：

- **抽公共函数这条路是死的。** 同名函数 11 个、完全一致 2 个，其余相似度 3%～80%。
  两边不是复制粘贴，而是同构改写：同一个"点击 → 命中 → 落光标"在三处因为画布所有权、
  滚动符号、结果何时到达而各自变形。把能抽的抽干净，收益约 15 行。
- **重复的真实位置是引擎访问面，而且是两层重复。** 第一层是命名：命令表用短名加参数
  对象（`type` / `move` / `undo` / `caretStyle`），`app.js` 用 wasm 原名加位置参数
  （`typeText` / `moveCaret` / `undoEdit` / `caretStyleJson`），61 个里 36 个是同一件事
  的另一个名字。第二层是覆盖：帧几何（`frameRgba` / `frameWidth` / `frameHeight` /
  `frameSlots` / `frameByteSize`）、页度量（`pagePxWidth` / `pagePxHeight` / `pageGapPx` /
  `setPageMetrics`）、光标即时读数（`caretOffsetLive` / `caretVerticalNow` /
  `caretBlockReversedNow`）这几组表里根本没有，只能由 in-thread 直调。

  命名这一层比覆盖更值得先处理：它不涉及语义，却让"读一份代码要会两套名字"成为常态。
  翻译一次之后，缺口只剩 21 条，其中 17 条是读命令。

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

落点（TS 改造之后，文件名按这一份走）：`engine_api` 就是 `web/src/engine/api.ts`，没有
构建产物；`host_core` 拆成 `src/host/*` 与 `src/input/*`；两条传输是
`src/transport/{inline,worker}.ts`，worker 壳是 `src/transport/worker_main.ts`。

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

### 4.4 命名：以命令表的短名为准

**方向**是短名，不是 wasm 原名。命令表是协议层，词汇由协议自己定义；`mod.typeText`
那一层是实现细节。让协议名跟随导出名，等于把两层绑在一起：C++ 侧一次改名就会同时
打到两个宿主与所有探针，而协议本该是稳定的那一面。

短名还避开一个实际撞名：光标样式的读与写是两个操作，表内写叫 `caretStyle`，
读若也叫 `caretStyle` 就无法区分。用原名则读是 `caretStyleJson`、写是 `setCaretStyle`，
字母上分得开、语义上反而更像同一件事的两个方向。

代价是 `app.js` 在步 3 改调表时按下面的表做一次名字翻译（36 个方法）。这个翻译只
发生一次，翻译完就只剩一套词汇。

配对样本（完整 36 对见附录 B）：

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

**补缺口**共 21 条，其中 17 条是读命令（已补进表，见 6 节步 2），4 条是宿主自己的
收尾入口，不进表：

| 组 | 命令 | 数量 |
| --- | --- | --- |
| 帧几何 | `frameWidth` / `frameHeight` / `frameByteSize` / `frameSlots` | 4 |
| 页度量 | `pagePxWidth` / `pagePxHeight` / `pageGapPx` | 3 |
| 光标即时读数 | `caretOffsetLive` / `caretVerticalNow` / `caretBlockReversedNow` | 3 |
| 窗口 | `windowFirstPage` / `windowPageCount` | 2 |
| 诊断 | `rasterBackend` / `buildStamp` | 2 |
| 样式读 | `caretStyleJson` / `selectionStyleJson` | 2 |
| 选区 | `selectionReport` | 1 |

不进表的 4 条：

- `frameRgba` —— 返回 wasm 堆视图，只在持有该实例的一侧有效，跨传输拿到的指针没有意义；
- `setPageMetrics` / `setFrameSlots` / `relayoutAndPaint` —— 宿主装载与收尾内部调用的入口，
  门面不暴露（暴露会让"什么时候重排"变成调用方的事）。

另有 4 个**特权访问**（`FS` / `allocBuffer` / `freeBuffer` / `HEAPU8`）：worker 侧不存在
对应的主线程堆，这 4 个在步 3 之后仍是 `app.js` 里仅剩的直接访问。

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

实测补充（见 6 节步 3b）：把门面方法包成 `async` 之后，`edit_probe` 14 项变红、
整轮耗时从 42 s 涨到 158 s。所以路 B 的落地必须与事件层合并同步进行，不能先改形状
再合并 —— 先改形状会得到一个"两边都异步、但探针同步假设全废"的中间态。

**路 C：不逼同步，按值折链**（做步 4 时实际采用的那条）

- 读：两边都是同步的。in-thread 直读引擎，异步宿主读镜像（`mirror.stats` / `mirror.geom`
  这批字段本来就是推回来的热读）。
- 写与命中：异步宿主返回 Promise，in-thread 返回当场结果。合并层用一个 `then1(v, fn)`
  折链 —— 同步值当场回调，Promise 才等。
- 于是事件层的代码只有一份，in-thread 那条路**仍然在同一个调用栈里完成**，
  探针的同步假设一条都不用改。

路 C 与路 A 的差别不是形状，而是"事件层归谁"：路 A 只能共享命令名，路 C 共享整层。
与路 B 的差别是 in-thread 不必假装异步。判据见第 9 节：合完之后 `edit_probe` 两宿主
各 365 项 0 红，没有任何中间态变红。

路 B 的代价在实测里出现了两次折扣：先异步化（步 3b）拿不到合并收益却丢掉同步性质；
强行异步之后，凡"发命令 → 立刻读值"的地方都要改成 `await`，而探针与工具栏接线里
这样的地方有几百处。没有一处需要那个异步 —— 需要异步的只有 Worker 那一侧的传输。

### 5.3 标记契约：三条必须由宿主页声明

（`host_core` 落地之后这张表是"写宿主页时必须知道"的差异对照。）

`host_core` 收一个 `opts.layout`，它决定三件事，这三条在 4.6.2 里已经写明是
"写宿主页时必须知道"的：

| 项 | `inside`（in-thread） | `overlay`（worker） |
| --- | --- | --- |
| 输入层挂法 | 滚动容器内部，滚动位移加在坐标上 | 滚动容器的兄弟覆盖层，位移减去 |
| 画布居中 | 由页面 CSS 给（`justify-content: center`） | 宿主自己算左边距 |
| 画布所有权 | 宿主自持 `<canvas>` | `transferControlToOffscreen` 交给 worker |

前两条是纯参数；第三条决定 `sink` 的实现，与 5.1 是同一件事的两面。

### 5.4 成本核算：读为什么走镜像而不是异步

实测（`out/_t_askcost.mjs`，worker 页，各 500 次）：

| 读法 | 单次 | 说明 |
| --- | --- | --- |
| `ask` 一次往返 | **0.188 ms** | 消息 + 克隆 + 回包；与 188 µs 的旧读数一致 |
| 读镜像两个字段 | **0.0006 ms** | 同拍快照，纯属性访问 |
| 读镜像一组四个字段 | **0.001 ms** | 仍是同一次快照 |

差 300 倍。按帧算更直观：某个字段每帧读 60 次，异步是 11 ms/帧，镜像 0.04 ms/帧。

但**慢不是主要理由**，绝对值 0.19 ms 单次并不致命。三条真正的理由：

- **自洽**：镜像是一份同拍快照，读 A 与读 B 之间不会插进一次编辑。异步读是逐个往返，
  要"一组自洽的量"就必须另做批量协议加版本号。
- **形态**：`if (caret.para === n)` 这类判断在异步读下变成 then 链。一次交互里
  "读 → 判断 → 写"三步要串起来，点击从一次往返变成两三次，而每次都把这一拍让给渲染。
- **对照**：in-thread 宿主能同步读，才使"两宿主同形门面 + 一套探针"成立。全异步之后
  探针 630 处求值要改写，且失去独立对照（见 6 节步 3b 的实测代价）。

镜像也不是免费：每拍 `sendState` 要把 `text0`（第 0 段全文）、`controls`、`dirtyPages`、
`selection` 一起打包过去。这笔账记在 `host.viewDiag` 的 `textMax` / `textSum` /
`packAvg` / `packMax` 上，宿主只读不判。

判据因此是一条分工，而不是一个偏好：

| 读的性质 | 走哪条 | 例子 |
| --- | --- | --- |
| 每帧或每次按键都读、字段小 | 进 `sendState`，读镜像 | 光标、统计、几何、选区、样式 |
| 大对象、低频、本来就要等 | `ask` 往返 | 整篇文本、帧像素、bench、审计、命中 |

## 6. 迁移步骤

每步都可停，每步都有可证伪判据。判据一律以 wasm 侧的浏览器探针为准。

### 步 1：抽出 `engine_api.js`

把 `engine_worker.js` 的分派、状态、窗口计算、页几何搬进新文件；`engine_worker.js`
只剩 `importScripts` 与 `onmessage` 转发。行为不变。

判据（已实测）：`worker_probe` 68 项 0 红；`edit_probe` 在 `worker.html` 上 365 项 0 红；
`stress_probe` 69 次点击 0 不一致；`web_probe` 6 场景 0 页面异常、缺字 0。

反向一半：`engine_worker.js` 里除文件头那句说明外不再出现 `mod.`
（`grep -n "mod\."` 只剩注释）。仍有调用说明分派没搬干净，"一份 API"就只是名义上的。

### 步 2：补齐 17 条读命令

按 4.4 的表把 17 条读命令补进 `engine_api.js` 的 READ 表。命名翻译不在这一步做 ——
那是步 3 在 `app.js` 一侧的事，表内名一次定下就不再动。

`worker_probe` 加一节 [W15] 逐条取一次值并校验形状，另配一条反向断言（未知命令必须
抛错，不能返回 undefined）。少了这一节，方法名拼错只会表现为某处 undefined，而
undefined 不会让任何断言变红。

判据：`worker_probe` 项数从 68 增到 74，0 红。已实测通过。

### 步 3：`app.js` 改调命令表

已完成。`app.js` 里 `mod.` 字面量从 261 处降到 0，引擎访问面收成两档：

| 档 | 用法 | 内容 |
| --- | --- | --- |
| 语义命令 | `api.read(name, args)` / `api.write(name, args)` | 编辑、光标、选区、命中、样式、控件、记账 —— 全部 |
| 宿主特权 | `api.engine().xxx()` | 装载（字体、页度量、场景、缩放）、呈现（present / frameRgba / relayoutAndPaint / setFrameSlots）、wasm 堆（FS / HEAPU8 / allocBuffer / freeBuffer） |

第二档的存在理由是策略而非语义：装载类命令在命令表里带着 `paintFull` 收尾，in-thread
宿主有自己的 `settle`，两边都跑一遍就是双重重排。所以"改完之后怎么重画"留在宿主侧，
命令表只管引擎那一步。

替换形态分四类，前三类可批量、第四类必须逐条看：

| 现有形态 | 处理 | 规模 |
| --- | --- | --- |
| `JSON.parse(mod.xxxJson())` | `api.read('xxx')`（表内已解析） | 50 |
| 无参读写 | `api.read` / `api.write` | 91 |
| 有参命令 | 位置参数转对象 | 87 |
| 装载与呈现 | `api.engine().xxx()` | 33 |

判据（已实测）：`edit_probe` 在 `index.html` 上 365 项 0 红；`worker_probe` 74 项 0 红；
`lint_comments.py` 0 违规。

三个实测踩到的坑，都不是"名字写错"这类显性问题：

- **`present` 在命令表里是空壳。** 表内 `present: () => {}` 的收尾由 Worker 侧的 `push`
  统一做；in-thread 直接调它等于什么都不做，症状是帧停在上一拍而所有读数正常。
  这一处必须走 `api.engine().present()`。
- **`bracketSlots` 在表内剥了一层。** 表内返回 `.slots` 数组（Worker 门面的形状），
  而 `app.js` 要的是整个对象，直接替换得到 `数组.slots` 即 undefined。
- **`hit` 的返回形态变了。** 原来返回 JSON 字符串（`'null'` 表示没命中），表内已经
  `JSON.parse`。调用点若还写 `JSON.parse(hit)`，会在真实点击路径上抛错、光标不落，
  表现成"点画布没反应"而不是报错。

### 步 3b：门面一律返回 Promise（未落地，与技术判断相反）

实测把 `facade` 的方法包成 `async`：`edit_probe` 14 项变红、整轮耗时从 42 s 涨到 158 s。
原因是探针在单线程宿主上依赖"调用即生效、随后同步读值"，包成 Promise 后读到的是上一拍。

结论：**门面异步化不能单独落地**，必须与步 4 的 `host_core` 合并一起做 —— 那时两边的
事件层同时改写，探针的同步假设也一起换掉。单独做它等于丢掉 in-thread 的同步属性、
却拿不到合并的收益。

### 步 4：合并 `host_core.js`（走路 C，已落地第一刀）

`web/host_core.js` 收的是**输入层**，判据是"这一层决定的都是这一下算哪条命令"：

| 内容 | 原来在哪 |
| --- | --- |
| 键位到命令（含竖排方向、修饰键、可打印字符） | 两份各自 60 行 `switch` |
| 命中到落点（整格 / 带侧别与行别 / 普通落点） | 两份各自 30 行 `if` 链 |
| 三击计数、组合提交、指针捕获记账 | 两份逐字重复 |
| 屏幕像素比跟随、markdown 还原 | 两份逐字重复 |

宿主交给它一个环境对象：`write` / `hit` / `pointOf` / `zoom` / `place` / `commit` /
`status`，加上可选的 `onChain`。差异从环境进来，判定只写一遍。

**没有并进这一层的**（它们不是"算哪条命令"，而是"像素去哪"）：帧与画布的归属、
窗口与滚动的策略、IME 框的定位、上屏与统计面板。这几处留在各自传输里，
与第 5 节那三个分叉是同一条边界。

判据（已实测）：`edit_probe` 两宿主各 365 项 0 红；`worker_probe` 74 项 0 红；
`stress_probe` 69 次点击 0 不一致；`multi_probe` 35 项 0 红；`error_probe` 两宿主各
19 项 0 红；`web_probe` 6 场景 0 异常；23 个诊断脚本全绿。

**这一刀踩到的一个真缺陷**：`placeAt` 最初没等"落光标"那一步就返回，于是异步宿主的
`clickAt` 在光标摆好之前就 resolve，调用方读到的是上一拍的光标。`edit_probe` 没抓到，
`out/_linehead.mjs`（折行处点行首）抓到了 —— 两次点击读到同一个偏移。
判据的差别就在这里：像素断言看的是最终画面，时序缺陷要看"调用返回之后立刻读"。
修法是让 `placeAt` 把 `place` / `selectCellPara` 的返回值也折进链里（`then1`）。
in-thread 侧 `then1` 当场回调，行为零变化。

**第二刀：缩放换算收进 `core.createZoom`**（已完成）

`kZoomBase` / `baseDensity` / `engineZoomOf` / `uiZoomOf` / `setZoomDensity` 在两侧逐字
相同，各有 20~25 个调用点。收进 `createZoom` 之后，宿主保留同名函数与全部调用点，
只把实现换成 `zoomCtl.base()` / `ofFactor()` / `factorOf()` / `set()`，密度仍存在各自的
局部变量（`currentZoom` / `zoom`）里 —— 这样 diff 只落在声明处。

这一刀真正留下的是"两个 DPR"的记账：

| 名字 | 含义 | 用在哪 |
| --- | --- | --- |
| 当下屏幕的 DPR | 现读 `devicePixelRatio` | 算基准密度、倍数转密度 |
| 密度按哪个屏幕算的 | `set()` 时记下 | 密度转倍数、换屏判断 |

混用这两个的后果是"换屏那一拍读到缩掉一半的假倍数，照它反推回去等于空操作" ——
原来这段说明在两个文件里各写一遍，现在只有一份，而且 `set()` 是唯一能同时更新两者的入口。

**还没做的，以及为什么不再往下合**：

- 画布与窗口：单线程宿主自己算窗口（`visiblePageRange` / `desiredWindow` / `applyWindow`），
  worker 宿主把滚动交给引擎、按回推的几何建画布。这不是重复，是**两套所有权模型**。
  合成一份等于让其中一侧改模型，那是产品决策，不是重排。
- IME 框定位、上屏、统计面板、bench / 表单 / 布局诊断：各自依赖本侧 DOM，合并只会
  产出一堆 `if (transport)`。
- 剩下的逐字重复只有 `frameSlotOf` 的三行取模，与 `setStatus` 的几行文字。

判据：23 个探针全绿（同第一刀），`lint_comments.py` 0 违规。

回退点：任一步的探针出现无法在当轮解释的红，回到上一步的产物，不做"边修边合"。

## 7. 建议的落点

**步 1～3 已完成**（见第 9 节）。做到这里，用户感知的繁琐已经消掉大半：同一件事不再有
三个写法，命令表覆盖了 in-thread 用到的全部读命令，新增能力只动一处。

`app.js` 里不再出现 `mod.*` 字面量；它对引擎的全部知识收敛到命令表，剩下的
`api.engine()` 只在装载、呈现、wasm 堆三类出现，每一处都能说出为什么必须直连。

剩下的判断只有一条：事件层要不要合并。要合，就选路 B 把门面异步化**与** `host_core`
合并一起做（5.2 与步 3b 的实测说明这两件事不能拆开）；不合，步 1～3 就是终点。

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

## 9. 落地进度（2026-10-19）

| 步 | 产物 | 判据 |
| --- | --- | --- |
| 1 | `engine_api.js` 1070 行；`engine_worker.js` 从 1258 行降到 342 行 | worker_probe 68/0、edit_probe(worker) 365/0、stress 69 次 0 不一致、web_probe 6 场景 0 异常 |
| 2 | READ 表补 17 条读命令；worker_probe 加 [W15] 6 项 | worker_probe 74/0 |
| 3 | `app.js` 的 `mod.` 从 261 处降到 0，改走 `api.read` / `api.write` / `api.engine()` | edit_probe(index) 365/0、worker_probe 74/0、lint_comments 0 违规 |
| 3b | 门面异步化（试过，已回退） | 14 项红、耗时 42 s → 158 s，见 6 节步 3b |
| 4 | `host_core.js`：输入层（键位、命中归宿、三击、组合、指针记账、DPI、markdown）+ 缩放换算（`createZoom`，两个 DPR 的记账）；两宿主的 `onKeyDown` / `onPointerDown` / `onComposition*` / `clickCountOf` / `cleanMarkdownLines` / `armDprWatch` / `kZoomBase` 一族全部删除 | 23 个探针全绿；`edit_probe` 两宿主 365/0；`worker_probe` 74/0；`stress` 69 次 0 不一致；`multi_probe` 35/0；`lint_comments` 0 违规 |

步 2 之后 `worker_probe` 从 68 项增到 74 项；其余探针项数不变。

## 附录 A：两宿主同名函数（11 个）

`armDprWatch`、`clickCountOf`（完全一致）；`cleanMarkdownLines`、`dragExtendTo`、
`framePointAt`、`frameSlotOf`、`onCompositionEnd`、`onDprWatch`、`onKeyDown`、
`positionIme`、`syncPageCanvases`（相似度 3%～80%）。

## 附录 B：仅 `app.js` 直调的 61 个方法

数据来源：`app.js` 中 `mod.<name>` 去重后，与 `engine_api.js` 命令表的键做差集。
61 = 同义不同名 36 + 表内无对应 21 + 特权 4。另有 44 个命令表条目 `app.js` 未直调
（`ping` / `heap` / `inkRows` / `frameDigest` / `blinkPhase` 等），属 worker 宿主专用或
调试用，合并时原样保留。

### B.1 表内无对应命令（21 条）

读命令 17 条，步 2 已补进 READ 表：

| 组 | 命令 |
| --- | --- |
| 帧几何 | `frameWidth` / `frameHeight` / `frameByteSize` / `frameSlots` |
| 页度量 | `pagePxWidth` / `pagePxHeight` / `pageGapPx` |
| 光标即时读数 | `caretOffsetLive` / `caretVerticalNow` / `caretBlockReversedNow` |
| 窗口 | `windowFirstPage` / `windowPageCount` |
| 诊断 | `rasterBackend` / `buildStamp` |
| 样式读 | `caretStyleJson` / `selectionStyleJson` |
| 选区 | `selectionReport` |

宿主内部入口 4 条，不进表：`frameRgba`（wasm 堆视图，只在持有该实例的一侧有效）、
`setPageMetrics` / `setFrameSlots` / `relayoutAndPaint`（装载与收尾内部用）。

### B.2 同义不同名（36 个 app 方法对应 34 个表内条目）

步 3 按这张表翻译。左列是 `app.js` 现在的调用，右列是命令表里的名字。

| `app.js` 直调名 | 表内名 |
| --- | --- |
| `typeText` | `type` |
| `moveCaret` | `move` |
| `moveCaretVertical` | `vertical` |
| `undoEdit` / `redoEdit` | `undo` / `redo` |
| `caretToDocStart` / `caretToDocEnd` | `docStart` / `docEnd` |
| `caretToLineStart` / `caretToLineEnd` | `lineStart` / `lineEnd` |
| `mergeWithPreviousParagraph` | `mergePrev` |
| `mergeWithNextParagraph` | `mergeNext` |
| `setCaretStyle` | `caretStyle` |
| `setSelectionStyle` | `selectionStyle` |
| `setViewportAnchor` | `viewport` |
| `showCaret` | `phase`（SILENT 档，只改状态不呈现） |
| `statsJson` / `editStatsJson` | `stats` / `editStats` |
| `controlsJson` | `controls` |
| `dirtyPagesJson` | `dirtyPages` |
| `bracketSlotsJson` | `bracketSlots` |
| `documentText` | `text` |
| `hitTest` | `hit` |
| `controlAtPoint` | `controlAt` |
| `removeControlAt` | `removeControl` |
| `setControlPropAt` | `setControlProp` |
| `benchRelayoutPaint` | `bench` |
| `prewarmPageGlyphs` | `prewarmPage` |
| `setScenario` | `scenario` |
| `setZoom` | `zoom` |
| `loadFormSample` | `formSample` |
| `loadStressDocument` / `loadStressDocumentDiverse` | `stress` / `stressDiverse` |
| `loadTextDocument` | `textDoc` |
| `loadFont` / `loadFontFromFile` / `setFontRoles` | `fonts`（三条合成一条 installFonts） |

### B.3 特权访问（4 条，不进表）

`FS`（字体文件写入 wasm 文件系统）、`allocBuffer` / `freeBuffer`（wasm 堆）、`HEAPU8`。
worker 侧没有主线程堆，这四条在步 3 之后仍是 `app.js` 里仅剩的直接访问。
