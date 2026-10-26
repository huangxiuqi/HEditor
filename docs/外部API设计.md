# 外部 API 设计（v2，2026-10-22）

> v1 的四处判断被推翻，这一版改在明处：
> 1. **`doc` / `text` 不进构造参数** —— 实例化只管"挂在哪、怎么配"，内容走 `open()` / `setText()`。
> 2. **光标移动、键盘、快捷键、IME、拖选是库的事**，不是宿主的接线活 —— 外部不该写任何
>    `addEventListener`。这一条把"包一层"变成了"搬输入层"。
> 3. **`change` 必须带格式**：给 `json`（惰性，见 §8）。
> 4. vite 不是"只打包新层"，而是**库的构建与开发环境**：`build --lib` 发 npm、`dev` 起开发服务器、
>    `demo/` 直接是调试页（§13）。
>
> v3 增补的三条（同日，见 §10–§12）：
> 5. **类型分三层出，不搞大一统 schema**：wasm 用 `emcc --emit-tsd`、载荷用 C++ 投影出的 schema、
>    契约迁移期手写、**重写后就是 TS 源码**（`src/types.ts` 的 `export interface`，`.d.ts` 由
>    `tsc --declaration` 生成 —— 但只导出接口，不导出实现类）。tsc 进 CMake 且是**硬依赖**，
>    没有就在 configure 期报错。
> 6. **公开面是白名单**：`createEditor()` 返回冻结的 Proxy，未知成员抛；demo 只加载库产物，
>    其余全是私有（五道闸，§11）。
> 7. `diag` 不再是"公开但没人管"：`debug: true` 才有。

---

## 1. 现实应用：谁在用、在哪用

方法表不是从现有 111 个成员里往外挑，而是从五个用例往回推。**没有一个用例用到的，不进产品面。**

| 场景 | 典型宿主 | 用到的成员 |
| --- | --- | --- |
| **A 只读展示** | 合同预览、报告归档页 | `createEditor` `open` `geometry` `scrollTo` `configure({mode:'readOnly'})` `on('error')` `destroy` |
| **B 表单填写** | 病历、审批单、报关单 | `open` `focusControl` `setControlProp` `form` `on('change')` `save` `destroy` |
| **C 富文本编辑** | 笔记、工单、模板 | `open` `setText` `type` `deleteBackward/Forward` `splitParagraph` `mergeParagraph` `undo/redo` `setIndent` `caret` `setCaret` `moveCaret` `select` `selectAll` `selection` `insertText` `on('change'/'selection'/'caret')` `save` |
| **D 数据填充** | 批量生成合同、套打 | `open` `insertText(pos, text)` `setControlProp` `save` |
| **E 自动化与测试** | 无头探针、审计 | `editor.diag.*` |

由此得出两条取舍：

- **`clickAt` / `selectCell` / `hit` 不是产品 API**。现实里没有人"用坐标点一下"，那是输入层的活；
  自动化要的是 `setCaret(pos)` 与 `focusControl(index)`。它们进 `diag`。
- **`insertText(pos, text)` 是产品 API**（场景 D）—— 引擎里本来就有 `insertTextUtf8(para, offset)`，
  只是没暴露。模板填充是最常见的集成方式，缺了它，使用者只能"先 setCaret 再 type"。

---

## 2. 现状为什么不能当 API

| 量 | 数 |
| --- | --- |
| `createEditor` 返回的成员 | **111**（一个字面量全部平铺） |
| `createWorkerEditor` 返回的成员 | **62**（字面量 + 赋值 + `defineProperty` + 访问器表，四种拼法） |
| 两者交集 | **46** |

111 个按用途：文档 6 / 编辑 10 / 光标选区 17 / 控件 5 / 视图 8 / 配置 14 / **诊断 33** / 内部 12 /
demo 6。问题不是数量，是**四件事混在一层**：产品动作、诊断账本、宿主内部件、演示入口。

v1 漏掉的第五条：**输入层根本不在库里**。`host_core.js`（315 行：键位映射与命中归宿）、
`drag_select.js`（273 行：拖选）、两个宿主各自的 `addEventListener`、IME 承接层 —— 这些是
"编辑器"的一部分，现在却要求每个宿主自己接。

---

## 3. 分层

```
L3  Editor        产品面：本文件设计的东西（33 个方法 + diag）
L2  输入层        键盘 / IME / 指针 / 拖选 / 滚动 / 焦点 —— 库内部，外部看不见
L2' 宿主实现      app.js（2546 行）与 worker_host.js（1701 行）：画布、贴图、DOM 结构
L1  engine_api    命令表 + 收尾 + 宿主回调（10 个）。只对"自己写宿主"的人开放
L0  wasm          106 个导出
```

L2 与 L2' 的分界是本版的核心：**判定进库，像素归属留在传输**（沿用 `host_core.js` 的既有约定）。
输入层要知道"这一下该走哪个命令、落点在哪、要不要扩选"，这些是库的判断；它不该知道
"画布在 DOM 的哪一层、贴图怎么搬"。

---

## 4. 创建与内容

```ts
const editor = await createEditor({
  container: el,                  // 只管挂在哪
  assets: { glue?, wasm? },       // 资源定位，默认按当前脚本地址推
  fonts: ['/f/思源黑体.otf'],      // 可给一项或一整串；不给用内置的三份 Noto
  config: { mode: 'readOnly' },   // 初始配置
  chrome: false,                  // 是否画工具条
});

await editor.open(savedJson);     // 内容走方法，不走构造参数
```

### 4.1 字体：注册一次，按名字选

```ts
await editor.addFont(file);                       // File / Blob / ArrayBuffer / 地址串都行
await editor.addFont({ url: '/f/a.otf', lazy: true, role: 'cjk', label: '宋体' });
editor.fonts();     // [{index, family, label, postScript, file, url, lazy, role}]
await editor.format({ font: '宋体' });             // 显示名 / 族名 / PostScript 名 / 文件名 / 下标
```

四条约束：

- **名字由字体文件自己报**（name 表的族名），**显示名由宿主给**（`label`）。两者的分工是
  "身份 vs 界面"：字体文件里没有中文名（Noto 这三份只有英文族名），而选字体那一格给人看的
  是中文，所以中文名只能由注册的一方写。不传 `label` 时显示名等于族名。
- **名字在主线程先折成下标**再下发（引擎只认下标）。引擎那一侧判"名字不存在"在 Worker
  传输下是一条 15 秒超时，报出来是"命令超时"而不是"字体没注册"。
- **注册不重排**：已经排好的内容不会因为多装一份字体而变样，新字体从下一次
  `format` / `paragraphFormat` 起生效，所以它是"写状态、不碰帧"那一档。
- **同一份只装一次**（键 = 装载方式 + 地址或文件名）。重复注册回同一个下标；
  存盘用的身份仍是 PostScript 名（退到族名、再退到文件名）。

角色（`cjk` / `mongol` / `latin`）只决定"新文档的正文用哪一份、样例里那几段用哪一份"，
不是装载的前提：只给一份字体时，正文就是它，蒙文跟随正文，拉丁关掉回退。

**`doc` / `text` 移出构造参数**的理由：

- 构造参数的语义是"这个东西长什么样"，而内容是会换的（打开另一份、清空、批量套打）。
- 内容可能在实例化之后才到（网络、权限、用户选择），构造参数会逼出"先等再创建"的假同步。
- 构造期只有一次；`open()` 可以被调很多次，且**失败时引擎状态不变**（现在的 `loadDocument`
  已经是这个语义，正好对上）。

`open()` 一个入口，格式自辨：

```ts
open(data: string, opts?: { format?: 'json' | 'text' }): Promise<void>
setText(text: string, opts?: { repeat?: number }): Promise<void>   // 明确用纯文本重置
```

判定规则：首字符 `{` 且能解析出版本头 → 按 JSON；否则按纯文本。解析失败抛 `TypeError`，
引擎不动。纯文本恰好以 `{` 开头时用 `format: 'text'` 兜底。

---

## 5. 方法表（41 个方法 + 2 个只读属性）

### 5.1 内容

| 签名 | 用在 | 备注 |
| --- | --- | --- |
| `open(data, opts?)` | A B C D | 失败时引擎不变 |
| `setText(text, opts?)` | C | `repeat` 复用同一段（压测） |
| `text()` | C | **整篇**（段间 `\n`）。现在是"第 0 段"，最容易踩 |
| `paragraph(i)` | D | 越界返回 `''`，读操作不抛 |
| `paragraphs()` | D | 分段数组（导出 / 统计） |
| `save()` | A B C D | 序列化成 JSON 文本 |
| `insertText(pos, text)` | **D** | 在指定位置插入，进撤销栈 |

### 5.2 编辑（用户输入由输入层走同一条路）

| 签名 | 备注 |
| --- | --- |
| `type(text)` | 在光标处插入（等价用户打字） |
| `deleteBackward(n = 1)` / `deleteForward(n = 1)` | 一次删 n 个，不是发 n 条命令 |
| `splitParagraph()` / `mergeParagraph(dir = 'prev')` | 回车 / 退格的段落语义 |
| `undo()` / `redo()` | |
| `setIndent(levels)` | 正数加深、负数变浅（现在只能 ±1，必须连点） |
| `copy()` / `cut()` / `paste()` | 系统剪贴板；片段带样式、控件、表格（见 15.1） |

### 5.3 格式（选中即改，回显走查询）

| 签名 | 备注 |
| --- | --- |
| `format(patch)` | 字符格式：`bold` / `italic` / `underline` / `font` / `size`（磅）/ `color`（`[r,g,b]`）；只写补丁里出现的键 |
| `paragraphFormat(patch)` | 段落格式：`font` / `size` / `lineFactor` / `spaceAfter`（磅）/ `pageBreakBefore` |
| `formatAt(opts?)` | 选区的共同字符格式；字段**缺席 = 没有统一的值**（空选区时是光标处的标记），`false` = 显式取消。`{effective:true}` 改报**实际生效的那一份**（样式链与默认值都算进去，字体字号永远有值） |
| `paragraphFormatAt()` | 选区起点所在段落的格式覆盖 |

三条约定：一是**三态**（缺席 / `true` / `false`），所以"取消加粗"必须写 `false`，不能省字段 ——
省字段是"别动它"。二是**幂等**：值本来就一样时返回 `false`，不产生撤销记录，所以
"点两下 B"之后一次撤销就回到加粗之前。三是**只改选区覆盖的那一段文字**：字符标记挂在文本节点
上，所以落下去时会按选区的两端把节点切开（留下的、要改的、被切出去的三段），选区之外的字
一个都不动。

回显与动作分开：`formatAt()` 是查询（Worker 下是一次往返，`await` 之后拿到补丁），界面在
光标 / 选区 / 内容事件之后读一次，再决定按钮点亮与否。有选区时它报的是**这些字共同的标记**
（一半粗一半不粗就报"没有统一值"，按钮不点亮），所以"按一下 B"永远等于"把这一段统一成粗体"。

回显有两份口径，别混：`formatAt()` 报"这一处**显式写了**什么"（三态，所以能分清"没设过"与
"显式关掉"）；`formatAt({effective:true})` 报"**实际生效**的是什么" —— 值取自排版产物里覆盖
落点的那个 run，字体与字号永远有值（正文一个字没改过也有字体有字号），加粗按字重判（≥600
算粗）。工具栏上"当前字体 / 字号"两个下拉框必须读后者：读前者时正文只有缺席，控件显示空白，
看起来就像"没有回显"。落点取的是**左边那个字**（段首取第一个），与"接着敲字会沿用哪一份"
同一条约定 —— 于是下拉框显示的与敲出来的永远是一份。

界面这一侧有两条硬要求，写在示例页的 `main.ts` 里：**读改写型动作要排队**（两次点击不排队，
第二次读到的是第一次写之前的值，于是连点两下只走一档）；**取反现读、不用缓存**（按钮亮不亮
取决于缓存何时刷新完，不该决定点下去生效不生效）。读写用同一套键名（`b/i/u/fi/sz/col` 与
`lf/sa/pbb`），与落盘那份是同一套 —— 界面不需要为"显示"再学一种表示。第三条在组件那一侧：
工具栏按钮**按下时不许抢焦点**（`button.ts` 的 `mousedown` 拦一次），下拉框选完要把焦点交回
编辑器（外壳那层的 click 收尾必须跳过 select / input / label —— 原生下拉靠焦点展开，
在它上面抢焦点就是"点开就收回去"，一个选项都选不了） —— 焦点一离开键盘承接层，之后敲字与快捷键就全落进按钮里，用户看到的是"点了加粗，
然后键盘没反应了"。

两条用户能直接看见的语义：

- **改完格式，画面立刻跟着变**。斜体（合成倾斜）、下划线（另画一条线）、字号与字重都可能
  "只改画出来的样子、不改字形位置"，页面的重画判据必须包含它们，否则就是"命令成功、读回
  也对、画面停在旧帧"。判据在 `format.spec` 的"画面重画"那一条（比截图，不比返回值）。
- **敲字沿用落点旁边的格式**：先看左边（同层前一个文本节点），左边没有才看右边（段落开头）。
  段落末尾那一下落点其实在**段落标记**上（它不占字符、占一个流位置），所以"取同层文本兄弟"
  这一步不能只按容器的槽做 —— 少了它，在粗体后面敲出的字是普通的。判据在 `format.spec` 的
  "敲字继承"那一条。

### 5.4 光标与选区（程序化定位）

| 签名 | 备注 |
| --- | --- |
| `caret()` | 同步读本地镜像 |
| `setCaret(pos, opts?)` | `{ side?: 'before'\|'after', line?: 'first'\|'last' }` —— 合并原来 4 个方法 |
| `moveCaret(step)` | `'left'\|'right'\|'up'\|'down'\|'lineStart'\|'lineEnd'\|'docStart'\|'docEnd'`，合并 6 个 |
| `select(anchor, focus)` / `selectAll()` / `clearSelection()` | |
| `focus()` | 把键盘焦点交回编辑器（工具栏、菜单用完要还回来；不滚视口） |
| `selection()` | 同步读，含跨页矩形与单元格区间 |

### 5.5 控件（表单）

| 签名 | 备注 |
| --- | --- |
| `focusControl(index)` | |
| `setControlProp(index, prop, value)` | `prop` 是运行态开关号（0 隐藏 / 1 只读 / 2 必填 / 3 结构锁），值是布尔 |
| `form()` | 精简视图：`[{ index, kind, label, value, focused }]`，给"整表读一遍"的宿主 |

| `removeControl(index, { keepContent? })` | 删控件；`keepContent` 为真时拆壳留内容。只在文档模式可用 |

删控件走进产品面，是因为它有一个宿主要画出来的入口（右键菜单）。它仍然受模式约束：
表单模式里控件的结构默认锁住，调用返回 false 且文档不动。

八种基础控件由 `insertControl({ type })` 插入，`type` 是 `text` / `number` / `radioButton` /
`checkBox` / `radioGroup` / `checkBoxGroup` / `dropdownList` / `date`；选项、互斥组、日期格式、
可选单位与记号颜色都在同一个入参里（`options` / `group` / `format` / `dateUnits` / `glyphColor`，
颜色是 `[r, g, b, a]`，不给就跟随文字颜色）。记号颜色对四种选择类都生效（单选、复选、
单选组、复选组）。插入选择类控件之后光标落在整块之后。

| 签名 | 备注 |
| --- | --- |
| `form()` | 每个控件一条：`type` / `key` / `label` / `group` / `text`（内容）/ `value`（记录上的值）/ `options` / `glyphColor` / `format` / `dateUnits` / `readonly` / `required` / `freeInput` / `start`、`end`（区间）/ `focused` |
| `setControlValue(index, { flag?, text?, items? })` | 改值并按类型重建显示内容；选择类与下拉、日期的内容是值的投影 |
| `toggleControl(index, option?)` | 切换一个选项（单控件传 -1）；互斥与多选规则在引擎里，整批一次撤销 |
| `updateControl(index, { type?, key?, label?, options?, group?, freeInput?, format?, dateUnits?, glyphColor?, readonly?, required? })` | 改定义与两个运行态开关：只改给出的项；改完按新定义重建内容，指向已删选项的值跟着清空，同属一步撤销 |
| `describeControl(control)` | 把一个控件摊成属性面板的字段表（哪种类型有哪些项、当前值是什么）；纯数据。日期给一个 `kind: 'datetime'` 字段：`inputType`（`date` / `time` / `datetime-local`）与 `step` 由精度算出，`value` 已按该类型裁好长度 |
| `applyControlForm(index, values)` | 按一份面板值改控件：值走内容、定义与开关走各自的入口，只提交真的变了的项。日期只给片段（`2026-10-26`）即可，补成完整 ISO 归库 |

`updateControl` 的 `type` 只能与当前一致（换类型请删掉重插）；给这个控件不收的字段
（例如给文本控件送 `options`）整份拒绝，不静默忽略。

选择类控件"点一下就切"由输入层完成：命中给出控件下标与选项下标，宿主的前置回调
（`createEditor({ onBeforeControlChange })`）可以返回 boolean 或 Promise&lt;boolean&gt;，
拒绝时文档与光标都不动。下拉的选项列表与日期的选择面板归宿主画（`onControlTap`）。

`onControlTap(index, rect, readonly)` 的第三项是业务只读位：宿主据此决定弹不弹 —— 但
**写回去的路已经在引擎里封住了**。`setControlValue` 与 `toggleControl` 对 `readonly` 为真的
控件一律返回 false，与"光标停不进去"是两条独立的线：前者管直接换值（弹层、脚本），
后者管键入。只读因此不再兼职表达"内容只是投影"——那件事由值形态决定（值在记录上的
控件，内容本来就不是可编辑正文）。

右键也一样归宿主：`createEditor({ onContextMenu(target, at) })`。库在这一下里做两件事 ——
拦掉浏览器自己的菜单、把插入点搬到指的地方（点图片则选中它，走的是与左键同一条落点路径）；
然后把落点归属交出去（`target.kind` 是 `'text'` / `'table'` / `'control'` / `'image'` /
`'none'`，带 `pos`、格坐标、控件下标与图片下标，`at` 是指针的客户端坐标）。
不给这个回调时库完全不拦右键 —— 拦掉一个宿主没打算接的手势，等于把一个能用的功能变成不能用。

### 5.5.1 图片（字节外置，文档只存资源 id）

| 签名 | 备注 |
| --- | --- |
| `registerImage({ id, width, height, data })` | 宿主解码后的 RGBA；重复注册同一个 id 表示换图，相关页重光栅 |
| `dropImage(id)` | 删像素，文档里的引用保留（画面退回占位框） |
| `insertImage({ resourceId, pxWidth, pxHeight, dpi?, width?, height? })` | 光标处插一张图；像素尺寸与显示尺寸至少给一组 |
| `images()` | 文档里每张图的下标、资源 id、显示尺寸与落页矩形 |
| `imageHit(x, y)` | 文档坐标（磅）命中哪张图的哪个把手；没命中 index 为 -1 |
| `selectImage(index)` | 选中出拖拽框与八个把手（-1 取消）；与文本选区互斥，选中期间不画文本光标 |
| `resizeImage(index, wPt, hPt, { merge? })` | 改显示尺寸；拖拽的后续帧带 `merge: true`，一次拖拽只留一步撤销 |

把手序号从左上顺时针 0..7。四个角按原图比例缩放（按住 Shift 解除），四条边只动一个轴。
把手命中区就是画出来的那个方块（不沿边线拉长）：边线上离把手远处点下去仍按图身算。
选中图片时 `caret().visible` 读回 false —— 对象选区与文本光标是两套互斥的选区。

### 5.5.2 表格（结构命令 + 三层记录）

八条结构命令只有动词不同：主语都是"光标所在的那一格 / 那一行 / 那一列"，被拒的条件也都在
引擎里判（模式、只剩一行一列、行里有纵向合并、合并区域不是矩形）。

| 签名 | 备注 |
| --- | --- |
| `tableOp(op)` | `'insertRowAbove'` / `'insertRowBelow'` / `'removeRow'` / `'insertColumnLeft'` / `'insertColumnRight'` / `'removeColumn'` / `'mergeCells'` / `'splitCell'` / `'removeTable'` |
| `setTableCell({ vAlign? })` | 格记录：垂直对齐（`'top'` / `'middle'` / `'bottom'`） |
| `setTableRow({ header?, split?, breakBefore?, heightPt?, heightRule? })` | 行记录：标题行、跨页断行、行前分页、行高与规则（`'auto'` / `'atLeast'` / `'exact'`） |
| `setTable({ widthPt?, align? })` | 表记录：宽度（0 = 自动）与水平位置（`'start'` / `'center'` / `'end'`） |
| `tableState()` | 一次读全：`inTable` / `rows` / `cols` / 表宽与位置 / 标题行数 / 当前行的四个开关与行高 / 当前格的坐标与两个跨度 / 整格选区的格数行列数 / 七条结构命令的可用性（`canInsertRow` 等，引擎试造一遍步骤再回答） |
| `describeTable(state)` | 把现状摊成属性面板的字段表（表 / 行 / 格三层在一张里）；纯数据，不读写文档 |
| `applyTableForm(values)` | 按一份面板值改表格：只提交真的变了的层，一层里改几项也只算一次提交 |

三条边界，都不是"暂不支持"而是设计选择：合并要求选中的格子正好铺满一个矩形，且除左上角
那一格以外都是空的（合并会删掉它们，有内容就是丢数据）；删行遇到行内纵向合并时拒绝（内容会
随行消失，而下面几行会凭空少一格）；删最后一列 / 最后一行拒绝。

`tableOp` 返回 false 时文档一个字节都不动。一次命令 = 一步撤销（插行与随之的跨度调整是同一
批步骤）；改属性是幂等的 —— 值与现状相同返回 true 但不产生步骤。

### 5.6 视图与配置

| 签名 | 备注 |
| --- | --- |
| `geometry()` | 页尺寸 / 间距 / 页数 / 滚动高度 / 当前窗口 |
| `scrollTo({ page?, y? })` | |
| `setZoom(z)` | 转发 `configure({ zoom })`，为可读性保留 |
| `configure(patch)` | 唯一配置写入口（§7） |
| `config()` | 只读快照 |
| `counts()` | `{ chars, words }`：码点数与非空白码点数。口径在引擎里（换宿主同一个数），读它不额外往返 |
| `toggleFormat(field)` | 点一下开关（粗体 / 斜体 / 下划线）：按光标处实际生效的值取反，连点两下按顺序落地 |
| `stepFontSize(dir, sizes)` | 按宿主给的阶梯把字号调大一档 / 小一档；已在两端时返回 false |
| `describePageSetup()` | 把页面设置摊成属性面板的字段表（厘米）；固定纸张下宽高两项带 `disabled` |
| `applyPageSetupForm(values)` | 按一份面板值改页面设置；与现状相同就不调 `configure`（省掉那次整篇重排） |

### 5.7 生命周期与事件

`on(event, cb)` · `flush()` · `destroy()` · `element` · `diag`

`flush()` 等所有在途命令的回话落地：批量 fire-and-forget 之后用它一次，比每个方法都
`await` 便宜。

---

## 6. 内部输入层

**外部不写一行 DOM 事件。** 库内部要负责：

| 类别 | 内容 | 现在在哪 |
| --- | --- | --- |
| 键盘 | 字符、方向键（含竖排方向翻转）、Home/End、Backspace/Delete、Enter、Tab | `host_core.js` 的 `keyToOp` |
| 快捷键 | Ctrl/Cmd+Z / Y / Shift+Z、Ctrl+A、Ctrl+Home/End | `host_core.js` |
| IME | `compositionstart/update/end`、候选窗定位、组合期间抑制排版 | 两个宿主各一份 |
| 指针 | 点选、拖选（含自动滚动）、双击选词、三击选段、跨页拖动 | `drag_select.js` + 两宿主 |
| 表格 | 单元格命中 → 选格、格内光标 | 两宿主 |
| 滚动 | 滚轮、滚动条、窗口跟随、连页重贴 | 两宿主 |
| 焦点 | focus/blur、隐藏输入框、Tab 顺序 | 两宿主 |

搬迁的做法（不是重写）：

1. 把两个宿主里**重复的那份事件接线**合成一个 `InputLayer`：判定（键 → 命令、点 → 落点/选区）
   与接线（`addEventListener` 挂在哪个元素）都留在库里；
2. 只留一个对外开关：`configure({ input: false })` 给"我要自己驱动"的宿主（无头探针就是这种）；
   默认 `true` —— 使用者什么都不用做；
3. 判据：**同一串真实事件在两个宿主上必须产生逐位相同的命令序列**（抓 `post` / `read` 的调用，
   与现在 `stress_probe` 的点击序列同构）。

`host_core.js` 已经写明"判定写在这里，像素归属留在传输"——搬迁是把这句话在代码里落实，
不是换一套设计。

---

## 7. 动态配置

### 7.1 三分类

| 类别 | 项 | 为什么 |
| --- | --- | --- |
| 构造期固定 | `container` `assets` | 换容器要重建 DOM；换 Worker 壳要重建引擎 |
| 运行期可改 | `zoom` `pageMode` `page` `mode` `input` `showBrackets` `showControlDots` `showMarks` `marginMarks` `undoLimit` `caretStyle` `selectionStyle` | 有明确语义，代价可分级 |
| 追加式 | `fonts` | 字体可以在任何时刻注册（`addFont`），只加不改 —— 已排好的内容不受影响 |
| 只读 | `pages` `geometry` `caret` `selection` | 从引擎状态推出来 |

### 7.2 一个入口，不是 14 个 setter

```ts
await editor.configure({ zoom: 1.5, pageMode: 'continuous' });
const c = editor.config();     // 生效后的值（越界已夹取）
```

1. **写入口只留一个**：14 个 setter 就是 14 条路，迟早与提交路径分叉。
2. **原子**：`zoom` 与 `pageMode` 各自都要重排，分两次调用 = 两次重排 + 两次呈现。
3. **能夹取并回报**：引擎会夹缩放、取整页间隙；返回值就是真相。
4. **能声明代价**：`configure` 只做最贵的那一档。

### 7.3 成本标签

| 标签 | 项 |
| --- | --- |
| `none` | `undoLimit` |
| `paint` | `mode` `input` `showControlDots` `showMarks` `marginMarks` `caretStyle` `selectionStyle` |

`mode` 是三档而不是 `readOnly: boolean`：`document` / `form` / `readOnly`。表单模式（光标只在
可编辑控件里，只有那些字改得动）是填表这个场景的主路径，布尔值表达不了它。三档都要落到引擎
（`setEditMode 0/1/2`）—— 宿主侧那道拦截只保护库自己的方法，键盘走输入层，绕得过去（实测过）。
三档的落点口径：选区（拖选、Shift 扩展、全选）三种模式都自由；插入点只出现在能打字的地方
—— 表单模式限于可编辑控件的内容，只读模式一个都不出现。
| `relayout` | `zoom` `pageMode` `page` `showBrackets` |
| `none` | `fonts`（`addFont` 只加脸、不重排也不重画） |

### 7.4 规范化与错误

类型不符 / 枚举越界 / `NaN` / 未声明的键 / 只读项 → **同步抛**（入队之前）。
数值越界（`zoom: 100`）→ 夹取，返回值里给生效后的数。

---

### 7.5 编辑标记（不可见字符）

`showMarks` 三档：`'none'` 全不显示、`'paragraph'` 只显示段落标记（默认）、`'all'` 连空格与
制表符一起显示（空格里画中点、制表符画箭头）。标记画成 ↵ 的形状（一横 + 左端箭头 + 右端
上折一竖），位置在每一段最后一行的行末。

三条约束，都写进了实现：标记**不占宽度**（不进流位置、不改断行，于是开关它只重光栅）；
**不进页内容指纹**（它不是内容），所以档位与"只读态画不画"都混进了光栅签名 ——
签名是"这一页像素还作不作数"的唯一判据，漏一项就是"改档位屏幕不动"；
**只读模式不画**（那是呈现态，标记会被印出来，与表单括号同一条）。
形状用矩形与三角形拼、不借字体字形：这三份字体里都没有 U+21B5，借字形会在缺字的行上
静默消失；顶点对齐到整数像素并且不开抗锯齿 —— 分带与整带的画布高度不同，AA 的覆盖率会
差出一两个色阶，而连页接缝的判据是逐字节相同。

---

### 7.6 页边距裁剪标记

`marginMarks`：正文可用区四角的短角线（与文字处理软件里的"显示裁剪标记"同一件事），默认打开
—— 它是"页边距设对了没有"的直接读数。标的是页边距的内沿，不是页框 —— 它回答"这一页能写到哪里"。
臂长固定 18 磅（1/4 英寸，印刷裁切线的惯例）：它是物理尺寸，不跟字号、页边距、缩放走；
边距比它窄时按边距的一半收，免得越进正文。连页下不画：那里没有页。

与 `showMarks` 同一条成本标签（`paint`）：画进页位图、不占宽度、不进流位置，所以只重光栅。
颜色与页框分开一项（`EngineConfig::margin_mark_color`，比页框浅一档）：它是辅助线，
不该比正文与页框更显眼。

---

### 7.7 页面设置

```ts
await editor.configure({
  page: { paper: 'A3', landscape: true, margins: { top: 72, right: 72, bottom: 72, left: 72 } },
});
// 自定义宽高：'Custom' 时 custom 必填（磅，纵向口径；横排由 paperSizePt 对调）
await editor.configure({
  page: { paper: 'Custom', custom: { w: 566.93, h: 850.39 }, margins: { top: 56.7, right: 56.7, bottom: 56.7, left: 56.7 } },
});
```

`page` 是 `{ paper, landscape, custom?, margins }`：纸张列 A4 / A3 / A5 / B5 / Letter / Legal
六档固定规格（`PAPER_PT` 表，磅）外加 `'Custom'`，方向是布尔，四边页边距分开给（磅，
1 厘米 = 28.35 磅）。自定义宽高限 36 ~ 5000 磅：再小放不下一行字，再大只是把排版撑爆。
`custom` 在换成固定规格之后仍然留着，再切回 `'Custom'` 不用重新输。代价标签是
`relayout`：断行的可用宽度就是它算出来的，改一次整篇重排。

为什么纸张规格不进引擎：规格只是"一组宽高"，抄进引擎就是第二个真相。库按表算出宽高，
再以 `pageSetup` 命令下发六个浮点数（页宽、页高、上右下左边距）。引擎只认这六个数，
宿主也可以绕开规格名直接给宽高（`configure` 里 `paper` 仍要填，它是这条配置的身份）。

四边边距各有上限（"一半再减 1 磅"）：再大就不给正文留地方了，`contentWidthPt` 会变成 0，
断行的可用范围为零。越界按上限收，返回值里是收过的那一份。

---

## 8. 事件

```ts
interface ChangeEvent {
  source: 'user' | 'api';
  version: number;              // 内容版本，每次变更 +1
  text: string;                 // 整篇纯文本（引擎里坐着的，几乎零成本）
  /** 惰性：读它才序列化整篇。同一版本只算一次；大文档别在每次 change 里读。 */
  readonly json: string;
}

interface EditorEvents {
  change: ChangeEvent;
  selection: Selection;
  caret: Caret;
  render: { pages: number; ms: number };
  configchange: { changed: Partial<EditorConfig>; config: EditorConfig };
  error: { message: string; fatal: boolean };
}
```

**为什么 `json` 是惰性而不是普通字段**：2000 段文档序列化一次 55.7 ms（实测），一次敲键若都带上，
键盘输入会直接卡住。做成 getter 之后：不用的人付 0；用的人第一次读付一次，同一版本后续读是缓存。
文档里必须写清这一条，否则使用者会以为 `change.json` 是免费的。

大文档的正确用法是 `change` 里只标脏、由宿主 debounce 后调 `await editor.save()`；
小文档（表单、模板）直接读 `change.json` 就够。

**为什么不再推那 25 个字段**：那是诊断账本（`blitPx` / `packAvgMs` / `heapMB` …）。产品只需要
"内容变了 / 选区变了 / 光标动了 / 画完了 / 出错了"。原始账本留在 `diag` 里按需读。

---

## 9. 错误模型

| 类别 | 例子 | 表现 |
| --- | --- | --- |
| 用法错误 | 未知事件名、`pos` 越界、`configure` 拼错键 | **同步抛**，无副作用 |
| 编辑被拒 | 只读、光标在不可编辑区、表格边界 | 返回 `false`，不是错误 |
| 环境错误 | 装载失败、Worker 死、wasm 崩 | `error` 事件（`fatal: true`）+ 后续调用 reject |

---

## 10. 类型：谁产生真相，谁出类型

三条实测事实决定了方案只能长成这样：

| 层 | 真相在哪 | 能自动出类型吗 | 实测 |
| --- | --- | --- | --- |
| wasm 导出（106 个） | C++ 的 `emscripten::function` 表 | **能** | `--emit-tsd` 出 107 个带签名的函数 / 14 KB |
| JSON 载荷（19 份 / 245 键） | C++ 的 16 个写出函数 | **能出形状，出不了语义** | 键名与粗种类（num / str / bool / arr / obj）可抽；int 还是小数位、哪个键会缺席，抽不出 |
| 对外契约（33 个方法） | 设计 | **不能** | 从实现反推，只会把现在这 111 个成员冻成标准 |

所以：**每层用它自己的真相出类型，不搞一份大一统 schema。** 三份声明、三条路径、四条判据。

### 10.1 wasm 层：emcc 直接出，不手写

链接时加 `--emit-tsd=<file>`（emcc 6.0.12 实测）：

```ts
type EmbindString = ArrayBuffer | Uint8Array | Uint8ClampedArray | Int8Array | string;
interface EmbindModule {
  typeText(_0: EmbindString): boolean;
  loadDocument(_0: EmbindString): string;
  editStatsJson(): string;
  frameRgba(): any;
}
export default function MainModuleFactory(options?: unknown): Promise<MainModule>;
```

三条局限，这正是它**只给库内部用**、不进公开契约的原因：

- 参数名是 `_0/_1` —— C++ 签名里没有名字；
- 返回 typed array 的是 `any`（embind 表达不了）；
- **JSON 字符串内部看不见**：`editStatsJson(): string` 仍然是 string —— 载荷形状得靠下一节。

产物是 ESM 形状（`export default factory`），而胶水仍按 `<script>` / `importScripts` 加载，所以加一层八行的 shim：

```ts
// web/types/wasm.shim.d.ts（手写）
declare const CreateHEditor: typeof import('./wasm.gen').default;
```

### 10.2 载荷层：schema 是 C++ 的投影，不是第二份真相

```text
C++ 的 16 个写出函数（wasm_main / engine_audit / engine_diag / engine_paint）
   │  scripts/gen_payloads.py —— 抽键名 + 粗种类
   ├─→ docs/api/payloads.json                    入库；唯一被人 review 的那一份
   ├─→ src/core/engine/payload_schema.gen.h      constexpr 键表（C++ 运行时用）
   └─→ web/types/payloads.gen.d.ts               interface EditStats { … }
```

**为什么不是 schema-first**：245 个键要在 JSON 里重抄一遍，而 C++ 的表达式仍得手写 —— 等于两份真相。这个仓库最贵的错从来不是"名字取得不好"，是"两边不一致"；再引入一份手写的 schema，只是把不一致的可能从两处变成三处。

**为什么不是 C++ 反射**：本机没有 `clang.exe`（`VC/Tools/Llvm/bin` 只有 clang-format 与 clang-tidy，2026-10-21 实测）；而且 `editStats` 的 62 个键里只有 46 个是字段名的机械 camelCase，另外 16 个（重命名 4 / 派生 2 / 跨结构 10）反射也推不出来。

**C++ 侧怎么被守**：三档，这次只做第一档。

| 档 | 做法 | 覆盖什么 | 成本 |
| --- | --- | --- | --- |
| 1（做） | 每个写出函数末尾一行 `return pk::checked(pk::edit_stats, o.str());` —— 扫顶层键，与生成表比数量 / 名字 / 顺序，不符抛 | **运行时真实输出**，含分支与动态键 | ~1 µs/份，常开 |
| 2（不做） | 生成 `Key<Kind>` 常量，替换 245 处字符串字面量 | 编译期 | 245 处改动 + 可读性下降 |
| 3（不做） | 宏 / 反射（glaze、`BOOST_DESCRIBE`） | 编译期 | 引依赖，且只覆盖 46/62 |

第 2 档不做不是因为难，是因为它**不增加覆盖**：第 1 档已经能抓住"写出来的与声明的不同"，第 2 档只是把它提前到编译期。等 `payloads.json` 稳定了再谈。

**JS 侧校验**：`readPayload(name, text)` 用生成的键表，默认关；`?strict=1` 与探针里开。62 键一次约 4.2 µs（parse 的 +45%；朴素写法因为每次重建键表是 +203%），默认开会让每拍多 13 µs。

### 10.3 契约层：迁移期手写，重写后它就是 TS 源码

**手写只是过渡。** TS 重写完成之后，契约就是 `src/types.ts` 里的一个 `export interface`，
`dist/index.d.ts` 由 `tsc --declaration` 生成 —— 自带类型，没有人再手写第二份。

| 阶段 | 契约在哪 | 谁保证实现不越界 |
| --- | --- | --- |
| 迁移期（B–C，已过） | `web/types/editor.d.ts`（手写）+ JSDoc `@typedef` | `tsc --noEmit`；实现对象的类型必须**推断**出来 |
| 现在（D2 已落） | `web/src/types.ts` 的 `export interface Editor` | `createEditor(): Promise<Editor>` 显式返回类型 |
| 发布 | `web/dist/index.d.ts`（`npm run build:types` 生成） | `src/index.ts` **只导出契约，不导出实现** |

落地结果：`web/types/editor.d.ts` 已删除，`web/src/{editor,index,transport/inline}.ts` 的引用
全部指向 `./types`；`gen_public_members.py` 与 `api_check.mjs` 的契约路径改成 `web/src/types.ts`。
生成的 `web/dist/index.d.ts` 只有两行实质内容 —— `export { createEditor }` 与
`export type * from './types'`；`web/dist/editor.d.ts` 只有 `createEditor` 的签名。
搬前搬后公开成员集合逐条相同：`public_members.gen.ts` 仍是 34 项 + `diag`，`--check` 绿。

**决定性的不是"手写还是生成"，是导出什么。** TypeScript 里 `export` 就是公开面：

```ts
// src/index.ts —— 公开面就这两行
export type { Editor, EditorOptions, EditorEvents } from './types';
export { createEditor } from './editor';

// src/editor.ts
class EditorImpl { /* 内部成员随便长 */ }          // 不导出
export async function createEditor(o: EditorOptions): Promise<Editor> {
  return new EditorImpl(o);                        // ← 返回类型是 Editor，不是 EditorImpl
}
```

三条理由说明为什么必须这么写 —— 这也正是"自带类型"最容易踩的坑：

1. **导出类 = 把实现也导出去了。** `class EditorImpl` 只要 `export`，它所有非 `private` 的成员都会
   进 `dist/index.d.ts` —— 那就是现在这 111 个成员的 TS 版。不导出，它一个都进不来。
2. **`implements` 只查"至少满足"**：实测把一个多出 4 个成员的对象赋给两方法的接口，tsc 一声不响。
   真正的闸是**返回类型显式标成 `Editor`** —— 调用方拿到的只有契约，实现多出来的成员不可见。
3. **对象字面量会查多余属性**（同一次实测）。所以最省事的实现方式是 `createEditor` 里造一个对象
   字面量并标 `: Editor`，多一个成员当场编译错；用 class 的话补 §10.3.1 那行断言。

那迁移期为什么还是手写？只剩两条理由（"生成物没人 review"到终态就不成立了，那时 review 的是
`src/types.ts`）：

- **契约先于实现**：33 是拿五个用例往回推的（§1），不是数出来的。
- **迁移期生成不出来**：`src/` 一开始是转发到旧宿主的薄壳，`--declaration` 只会产出一堆 `any`。
  要让它有类型，就得先写注解 —— 那还是手写，只是换了个文件。

### 10.3.1 两个方向，第二个必须自己写

```ts
// ① 少一个方法、签名不符 → 编译错
const _implements: Editor = impl;

// ② 多一个成员 → 结构化类型不拦（见上），自己写一行。
type Extra<T, U> = Exclude<keyof T, keyof U>;
type AssertNone<T> = [T] extends [never] ? true : { 多余成员: T };
const _noExtra: AssertNone<Extra<typeof impl, Editor>> = true;
```

② 报错时直接点名：

```
Type 'boolean' is not assignable to type '{ 多余成员: "c" | "auditFrame"; }'.
```

前提是 `impl` 的类型**是推断出来的**，不能先标成 `Editor`（标了就是自己跟自己比）。迁移期用
JSDoc 写这两行，重写后换成 TS 版 —— 内容一样。

**搬家那一次要有判据**：契约从 `web/types/editor.d.ts` 搬进 `src/types.ts` 时，`api_check.mjs`
必须证明**搬前搬后的公开成员集合逐条相同**（顺序 + 名字 + 参数个数）。否则"搬家"就成了偷偷改契约。
实测：搬家后重跑 `gen_public_members.py --check` 一次就绿（34 项 + `diag`，与搬前逐条相同），
这条判据因此是同一次执行里给出的，不是事后补的说明。

**契约会不会被顺手改？** 会。所以 `src/types.ts` 顶部写死一条"这个文件只写承诺"，再加一条快照
判据：公开成员的有序列表 + 签名哈希，改动即红、要改必须显式更新常量 —— 与 `edit_test` 的摘要
A/B/C 同构（有意改行为才允许重新取值，且必须写清原因）。

`strict` 一开始会有噪声 —— 只对 `web/src/` 开；旧宿主文件不进来，等它们被搬进 `src/` 时逐文件打开。

### 10.4 几种漂移，分别红在哪

| 漂移 | 红在哪 | 什么时候 |
| --- | --- | --- |
| C++ 改了键名，忘了重生成 | `gen_payloads.py --check` | 提交前 / CI |
| 重生成之后 JS 还读旧名 | `tsc --noEmit`（类型上没有这个属性） | 同上 |
| 键只在某条分支里出现（静态抽取看不见） | `pk::checked` 运行时抛 | 第一次走到那条分支 |
| 实现多长出一个公开方法 | `tsc`（§10.3.1 的 ②）+ `api_check.mjs` | 同上 |
| 实现少一个契约方法 | `tsc`（`Editor` 不满足） | 同上 |
| wasm 新增导出没登记 | `idl_check.mjs`（已有） | 同上 |

这张表的意义是：**没有一种漂移是静默的**。现在的状况正好相反 —— 改个键名，C++ 编译过、845 条 native 断言全绿、探针读到 `undefined`，一切看着正常。

### 10.5 四条命令

```bash
python scripts/gen_payloads.py --check     # schema 与 C++ 一致
node scripts/idl_check.mjs                 # 命令面与导出（已有）
node scripts/api_check.mjs                 # 公开面：demo 只调白名单
tsc --noEmit -p .                          # 契约 vs 实现
```

前两条**不需要构建产物、不需要浏览器**：打开浏览器之前就红。

---

## 11. 边界：公开面是白名单，其余私有

### 11.1 现在外部能摸到什么

一个 `<script>` 就能拿到全部：

| 全局 | 内容 |
| --- | --- |
| `window.__heditor` | 111 个成员（产品 + 诊断 + 内部件平铺） |
| `window.createEditor` / `createWorkerEditor` | 两个内部工厂 |
| `window.__heditorEngineApi` | 命令表与传输门面 |
| `window.CreateHEditor` + `mod` | wasm 工厂与 106 个导出 |

所以"毛坯房"不只是形状难看 —— 是**根本没有墙**。

### 11.2 五道闸

| # | 闸 | 落在哪 | 拦什么 |
| --- | --- | --- | --- |
| 1 | 产物隔离 | `heditor.js`（库产物）是 demo 唯一加载的脚本；`app.js` / `worker_host.js` / `engine_api.js` 进了 bundle 就是模块作用域，不挂 window | 页面顺走内部工厂 |
| 2 | 实例白名单 | `createEditor()` 返回冻结的 Proxy；未知属性 `get` 抛 `TypeError` | 从实例上摸内部成员 |
| 3 | 静态检查 | `scripts/api_check.mjs` 扫 `demo/` | 内部标识符；契约里没有的方法 |
| 4 | 运行时探针 | `scripts/demo_probe.mjs` | 全局泄漏、实例多出成员 |
| 5 | 编译检查 | `tsc --noEmit`（§10.3） | 实现与契约不一致 |

**闸 1 的迁移期做法**：内部脚本还是 classic script，入口在注入前后做三件事 —— 先记下要用的全局，加载完 `delete window.X`，再 `Object.defineProperty(window, X, { get() { throw … } })`。demo 页从第一行起就只能拿到 `HEditor`。

**闸 2 的实现**（这几行决定了它能不能真拦住）：

```js
const PUBLIC = new Set([...]);   // 从 editor.d.ts 生成，不手抄
const guard = new Proxy(target, {
  get(t, k, r) {
    // then 必须放行：await 一个对象时会先读 .then，拦了它连 await 都过不去。
    if (k === 'then' || k === 'toJSON' || k === 'constructor' || typeof k === 'symbol') {
      return Reflect.get(t, k, r);
    }
    if (!PUBLIC.has(k)) {
      throw new TypeError(String(k) + ' 不是公开 API —— 能调的只有 editor.d.ts 里的方法');
    }
    return Reflect.get(t, k, r);
  },
  set() { throw new TypeError('配置走 configure()'); },
});
```

`then` / `toJSON` / `constructor` / Symbol 这四类放行是必须的 —— 守卫把正常用法也拦掉的坑，比不设守卫更难查。

**闸 3 的规则**（每条都有具体理由）：

| 规则 | 为什么 |
| --- | --- |
| 不许 `__heditor` / `EngineApi` / `CreateHEditor` / `mod` / `HEAP` / `FS` / `importScripts` | 这些是绕过公开面的具体路径 |
| `editor.X` 的 X 必须在 `Editor` 接口里 | 抓"实现有、契约没写"的成员 |
| `HEditor.X` 必须在导出的入口里 | 同上，入口那一层 |
| 不许 `addEventListener('keydown' / 'pointerdown' / 'compositionstart')` | 输入层在库里；demo 自己接等于把 §6 又做一遍 |
| 不许自己 fetch `.wasm` / 胶水 | 资源定位是库的事（`assets`） |

**闸 4 的断言**（都在 `demo_probe.mjs` 里）：

- `window.__heditor === undefined`、`window.CreateHEditor === undefined`、`window.createEditor === undefined`；
- `Object.keys(editor)` 是契约成员的子集，且包含全部产品方法；
- `editor.auditFrame` 抛 `TypeError`（证明私有真的私有）；
- 用公开方法走一遍：`setText` → `text()` → `type` → `save()` → `setCaret`。

### 11.3 diag 的定位

`diag` 不是"公开但没人管"，而是 **debug 开关下的公开面**：只有 `createEditor({ debug: true })` 才有，否则访问 `editor.diag` 抛。探针一律带 `debug: true`，产品 demo 不带 —— "能不能依赖 diag"在代码里是显式的。

### 11.4 内部验证台不在此列

`web/index.html` / `worker.html` / `multi.html` 继续用 `__heditor` 与 wasm 导出：它们是**仓库内部的验证台**，不是对外 demo。检查器只扫 `demo/` —— 扫 `web/` 会把 10 份探针全判红。

---

## 12. 构建：tsc 是硬依赖，缺了在 configure 期就报错

### 12.1 为什么不能等到链接

`--emit-tsd` 是**链接阶段**调 `tsc` 的（`tools/emscripten.py` 里：先找 `<emscripten>/node_modules/.bin/tsc`，再找 PATH，都没有就 `exit_with_error`）。这条链上有两个坑：

- **链接失败会留下未优化的 wasm**：实测 4.66 MB vs 4.02 MB（`-O3` 没跑完）。文件在那儿、大小也像回事，最容易被人拿去做性能账。
- 报错出现在"编译到 99%"的时候，而真正的原因（环境里缺 tsc）在第一条命令里就该说清。

⇒ 检查提前到 **configure**：找不到就 `FATAL_ERROR`，连构建目录都不生成。

### 12.2 CMake 里长什么样

```cmake
if(EMSCRIPTEN)
  # emcc 自己按这个顺序找 tsc：<emscripten>/node_modules/.bin/tsc(.cmd)，然后 PATH。
  #   我们按同一顺序先找一遍，只为把"没有 tsc"从链接期挪到 configure 期。
  unset(HEDITOR_TSC CACHE)     # 不吃缓存：删了 tsc 之后 configure 也要当场红
  find_program(HEDITOR_TSC NAMES tsc tsc.cmd
               HINTS "${EMSCRIPTEN_ROOT_PATH}/node_modules/.bin")
  if(NOT HEDITOR_TSC)
    message(FATAL_ERROR
      "wasm 构建需要 TypeScript 的 tsc（emcc --emit-tsd 用它生成 wasm 层声明），但没找到。\n"
      "emcc 的查找顺序是 <emsdk>/upstream/emscripten/node_modules/.bin 与 PATH。\n"
      "修法：npm install --prefix .toolchain/emsdk/upstream/emscripten typescript@5.6\n"
      "（scripts/deps_init.ps1 已带这一步。）")
  endif()
  execute_process(COMMAND "${HEDITOR_TSC}" --version
                  OUTPUT_VARIABLE _tsc_out OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" HEDITOR_TSC_VERSION "${_tsc_out}")
  if(HEDITOR_TSC_VERSION VERSION_LESS 5.0)
    message(FATAL_ERROR "tsc 太旧（${HEDITOR_TSC_VERSION}）：--emitDeclarationOnly 在 5.0 之前行为不稳")
  endif()
  message(STATUS "wasm 声明文件由 tsc ${HEDITOR_TSC_VERSION} 生成")

  target_link_options(heditor_wasm PRIVATE
    ...
    # ★ 绝对路径：emcc 里是 os.path.join(dirname(js_target), filename)，
    #   相对路径会拼成 build/wasm/web/wasm.gen.d.ts，web/ 那边就找不到了。
    --emit-tsd=${CMAKE_SOURCE_DIR}/web/types/wasm.gen.d.ts)
```

三个细节都是踩出来的：

- **`unset(... CACHE)`**：`find_program` 默认缓存。删了 tsc 再 configure 仍然绿，直到链接才红 —— 正是要避免的那条路。
- **绝对路径**：见注释。
- **版本**：`--emitDeclarationOnly` 在 5.0 之前不稳；这也是 emcc 优先用自己 `node_modules` 里那份的原因。

native 构建不受影响（整段在 `if(EMSCRIPTEN)` 里）—— 单测与审计宿主不需要 tsc。

### 12.3 产物与入库

`web/types/wasm.gen.d.ts` **生成、不入库**（`.gitignore` 加 `web/types/*.gen.*`），理由与 `build/` 一样：它是 wasm 的镜像，入库只会让每次 diff 夹着 14 KB 机器文本。

代价：`tsc --noEmit` 依赖一次 wasm 构建。可接受 —— 库本来就要 wasm 才能跑；`gen_payloads.py --check` 与 `idl_check.mjs` 才是"不构建也能红"的那一档。

### 12.4 新机器

`scripts/deps_init.ps1` 加一步：

```powershell
npm install --prefix .toolchain/emsdk/upstream/emscripten typescript@5.6 --no-audit --no-fund
```

版本钉在脚本里（与 `scripts/skia_rev.ps1` 同理）：升级 tsc 只影响声明文本，不影响 wasm；但要重生成一次并 review diff。

---

## 13. vite：库构建 + 开发服务器 + demo



### 13.1 目标

- `npm run build` → `dist/`：ESM + UMD + `.d.ts` + `heditor.wasm`（从 CMake 产物拷）。
- `npm run dev` → 开发服务器：`index.html` 直接跑，改 `src/` 热更新。
- `demo/` 与现在的 `web/index.html` 同构 —— 探针可以原样指过去，判据不用重写。
- **demo 页只加载 `heditor.js` 一个脚本**（§11 闸 1）；它不再知道 wasm、胶水、命令表的存在。

三件事的边界要分清：**CMake 管 wasm 与它的声明**、**vite 管库与 demo**、**tsc 管类型检查**。
vite 不做类型检查（只擦除类型），所以 `npm run build` 之后必须再跑一次 `tsc --noEmit`。

### 13.2 目录

```
web/                  前端工具链全在这里，仓库根只留 CMake 与 src/ tests/ scripts/
  package.json        exports / types / files / sideEffects / private（见 13.4）
                      scripts: build / dev / typecheck / check（四条判据）
  tsconfig.json       strict，noEmit（类型检查与构建分开；include 只有 src 与 types）
  tsconfig.build.json 声明产物：src/index.ts -> dist/*.d.ts
  vite.lib.config.ts  build.lib -> dist/（ESM + IIFE）
  vite.config.ts      示例页（index.html / multi.html）-> dist-app/
  vite.worker.config.ts  src/transport/worker_main.ts -> dist/heditor_worker.js

（下面这些在 web/src/ 下）
src/
  index.ts            createEditor 与类型出口
  editor.ts           Editor 实现（编排，不含 DOM 细节）
  guard.ts            §11 闸 2：公开面白名单 Proxy
  config.ts           配置表（键 / 类型 / 默认值 / 成本标签）
  events.ts           事件总线（合批：selection/caret 每拍最多一次）
  input/              §6 的输入层：keys.ts / pointer.ts / ime.ts / scroll.ts
  adapter/worker.ts   传输：Worker
  adapter/inline.ts   传输：主线程
  diag.ts             诊断面
types/
  editor.d.ts         手写契约（迁移期；重写后搬进 src/types.ts，§10.3）
  payloads.gen.d.ts   由 payloads.json 生成（§10.2）
  wasm.gen.d.ts       emcc --emit-tsd 产出（§12，不入库）
  wasm.shim.d.ts      八行 shim（§10.1）
demo/
  index.html          唯一入口是 <script src="../dist/heditor.js">
  demo.js             只用 HEditor.createEditor 与方法（§11 闸 3 只扫这一层）
```

### 13.3 三个必须处理的坑

1. **资源定位**。库里不能靠 `?wasm=`（那是 demo 页的约定）。`assets` 选项 + 默认
   `new URL('./heditor.wasm', import.meta.url)`（ESM 下可用；UMD 下不可用，必须显式给）。
   胶水仍由 `host.loadGlue` 加载（`importScripts` 或 `<script>`），**不需要** `-sEXPORT_ES6`。
2. **module worker 不支持 `importScripts`**。本版仍用 **classic worker + importScripts**，
   vite 用 `?worker&url` 拿 URL；要改 ESM 就得同时改 wasm 构建，那是另一笔。
3. **wasm 不进 bundle**。它由 Emscripten 构建、约 1 MB 起，打进 JS 会拖慢首屏解析；作为独立
   资源随包发布，`assets.wasm` 指过去。
4. **构建顺序**：`tsc` 生成的 `wasm.gen.d.ts` 是 `types/` 里的一等公民，而它由 **CMake 的
   wasm 链接**产出 —— 所以 `npm run build` 之前必须先跑一次 wasm 构建，否则类型检查读不到它。
   这条顺序写进 `package.json` 的 `prepare`（或 `build` 的第一步检查文件存在），不靠记忆。

### 13.4 迁移期的自包含问题

阶段 B 的库里还会**运行时加载旧的 `web/app.js` / `web/worker_host.js`**（它们太大，一次搬不完）。
所以那个阶段的产物**不可发布** —— 靠 `package.json` 的 `"private": true` 硬拦：
**没搬完就不许 npm publish**。这条不是洁癖：一个发布出去的包如果靠 `<script>` 注入另一个文件，
谁装谁坏。

这段过渡期正是 §11 闸 1 的压力点：内部脚本是 classic script，注入之后会挂到 `window` 上。
处置是"用完就拆"—— 入口记下引用后 `delete`、再把同名属性改成会抛的 getter。等 `src/` 把
它们吸收完，这一步自然消失（那时 window 上本来就什么也没有）。

### 13.5 示例页的外壳：组件与图标

示例页的顶部功能区与底部状态栏拆成一件组件一个文件（`web/app/ui/`）：

| 文件 | 管什么 |
| --- | --- |
| `types.ts` | 描述层：按钮 / 下拉框 / 竖排按钮列 / 组 / 页签 / 组件 / 外壳 |
| `icons.ts` | 按文件名取图标（`import.meta.glob('?raw')`，加一个图标不用改代码） |
| `button.ts` `choice.ts` `stack.ts` `group.ts` | 按钮、下拉框、竖排按钮列、一排动作 + 组名 |
| `ribbon.ts` | 页签条与面板（全部面板都在 DOM 里，切页签只改一个类） |
| `statusbar.ts` | 底部状态栏（状态文字 + 缩放） |
| `chrome.ts` | 装配：摆位置、连回调、把 refresh 转发下去 |

两条约定：

- **刷新是重读回显，不是重建 DOM**。重建会丢焦点与按下态，而光标每移动一次都要刷一遍。
- **描述与渲染分开**。加一个按钮只改动作表（`main.ts`），不动 DOM 与事件 —— 动作表同时是
  "这一页能做什么"的清单，没有浏览器也能读。

图标是 `web/app/icons/*.svg`，一个图形一个文件，同一套画法：16×16 视窗、内容占 2..14、
描边 1.25（加粗那一个 2.2）、圆头圆角、一律不填充、颜色只有 `currentColor`。
统一口径不是洁癖 —— 一排图标里混着填充与细线时，总有几个看着比别的"重"，而那种不齐
在 16 px 上比在 40 px 上更明显。

---

## 14. 迁移顺序

| 步 | 做什么 | 判据 |
| --- | --- | --- |
| A ✅ | 契约 + 本设计（当时在 `web/types/editor.d.ts`，D2 已搬进 `src/types.ts`） | `tsc --strict` 0 错（已过） |
| A2 ✅ | **构建与类型**：tsc 进 CMake（§12）、`--emit-tsd`、`.gitignore`、`scripts/deps_tsc.ps1` | 构建产出 `web/types/wasm.gen.d.ts`（14231 字节 / 107 个导出）；**藏起 tsc.cmd 后 configure 退出码 1**（已实测） |
| A3 ✅ | **载荷契约**：`gen_payloads.py` + `payloads.json` + `pk::checked` 接进 16 个写出函数 | 16 份 / 176 键；`--check` 绿；改一个键 → `--check` 红（已实测）；不重生成直接跑 → 报"载荷 auditPage 的键与契约不符（第 1 个）"（已实测） |
| B ✅ | `package.json` + vite + `web/src/`（公开层）+ `web/demo.html` | `tsc --noEmit` 0 错；`api_check.mjs` 通过；`demo_probe` 22 项全绿 |
| C ✅ | 把 `app.js` / `worker_host.js` 的必要部分搬进 `src/`：`engine/api.ts`（命令表真源）、`input/`（键位 / 连击 / 拖选）、`host/`（DOM 骨架 / 画布 / 滚动）、`transport/`（inline + worker 壳）、`editor.ts` | `tsc --strict` 0 错；`idl_check` 绿（协议表 107 条 / wasm 导出 110 个）；`demo_probe` 22 项 × 2 传输；**新增 `input_probe` 25 项 × 2 传输，两条传输的 `TRAIL` 逐位相同**；旧页面 `edit_probe` 369 × 2、`worker_probe` 74 一字未变 |
| D ✅ | 探针迁到 `diag`：`edit_probe --lib` 在新库的 demo 页上跑同一份 369 条断言，中间隔一层迁移垫片（`scripts/lib_adapter.js`） | inline：**368 条全绿**；worker 传输还差一段（见下） |
| D2 ✅ | **契约搬家**：`web/types/editor.d.ts` → `web/src/types.ts`；`web/dist/index.d.ts` 改由 `npm run build:types`（`tsc --emitDeclarationOnly`）生成 | 成员集合逐条相同（`public_members.gen.ts` 34 项 + `diag`，`--check` 绿）；`dist/index.d.ts` 只有 `createEditor` 与类型重导出，`dist/editor.d.ts` 只有函数签名 |
| E ✅ | `web/demo.html` + `demo_probe.mjs` | demo 里 `window.__heditor === undefined`；未知成员抛；`createEditor` 已被撤掉 |

A2/A3 排在最前面的理由：它们**不改任何运行行为**，却决定了后面每一步的判据能不能红。
反过来先做 B 的话，`src/` 一写出来就没有类型可依 —— 那正是"毛坯房"再来一次。

B 落地时与设计有三处出入，都是被现实逼出来的：

- **demo 放在 `web/demo.html`，不是 `demo/`**。旧宿主的字体与 wasm 路径都按 `new URL('../', location.href)`
  解析（约定是"页面在 web/ 下"），搬到 `demo/` 之后所有资源都 404。等 `assets` 选项真正接管路径
  再搬，那时页面只需要一个 `<script>`。
- **`transport` 只接了 `'inline'`，默认值当时也是它**。Worker 那条要等 C 步把输入层搬进库；
  现在强接只会把"消息表在两边各写一份"的老问题带进来。传 `'worker'` 会抛并指向 C 步。
- **`facade.__api` 是内部隐藏通道**（不可枚举）。`open` / `insertText` / `geometry` 这类方法在旧门面
  上没有对应入口，公开层要拿命令表实例。它不出现在 `Object.keys` 里、`api_check` 禁止 demo 使用，
  C 步之后消失。

两处按引擎能力修正了契约：`focusControl(dir)` 不是按序号（引擎只有"前进/后退"），
`setControlProp(index, prop, value)` 的 prop 是数字属性号、value 目前只有布尔。

`api_check.mjs` 是新判据（A2 就可以先用一个只查契约成员集合的版本）：把 `editor` 的成员集合
与 `editor.d.ts` 逐条比，多一个少一个都红。它拦的正是"实现偷偷多长出一个方法"——这一条现在
没有，所以 111 个成员才长得出来。

C 步收尾时又改了四处，都是被"它必须真的能用"逼出来的：

- **`ChangeEvent.json` 从属性改成方法**（`json(): Promise<string>`）。属性写法有两个问题：
  `JSON.stringify(ev)` 会顺带把 2000 段 / 55 ms 的序列化算进最糟的一拍；Worker 传输里同步读
  根本不可能（一次往返）。同一个版本仍只序列化一次（闭包缓存）。
- **`engine_api.js` 整个收掉了**。它先是从源码变成产物（`vite.engine.config.ts` 打经典脚本，
  给旧页面与经典 Worker 的 `importScripts`），旧页面与旧 Worker 删除之后只剩 `idl_check`
  在读它；于是那一条也改成直接把 `web/src/engine/api.ts` 转译执行 —— 源码树里不再有
  构建出来的 JS，`build:engine` 这一步随之取消。真源始终只有 `api.ts` 一份。
- **引擎只跑在 Worker 里**（`createEditor` 不再有 `transport` 选项）。同线程那一档已删除：
  它唯一的独占能力是"在 copy 事件里同步写 DataTransfer"，而 Worker 侧退化成
  `navigator.clipboard.write` 只差一次往返；反过来，字体的真按需读盘（同步 `fread` 落到
  OPFS）只有 Worker 能做 —— 两条都留等于永远缺一半能力，还要各自维护一套画布与贴图。
  壳在 `web/dist/heditor_worker.js`，主线程只剩槽画布与转移。
- **`flush()` 有了真语义**：等所有在途 do / ask 收到回话（`EngineLink.flush`）。原先
  `await Promise.resolve()` 是空转，于是 Worker 模式下 `await flush()` 之后读到的还是上一拍的几何。

D 步的 `--lib` 模式（2026-10-22）：垫片把旧门面名翻成公开面 + `diag`，**一条断言都不改**，
（该垫片当晚已拆除：`edit` / `stress` 现在直接调公开面与 `diag`，读数摊平只剩
`web/tests/support/readings.ts` 的前言，见 `docs/web测试工程化评估-Playwright.md` 第九节。）
先在库页面上跑，再拿旧页面当对照物。落差从 103 红一路降到 0，中间照出六个真问题：

| 症状 | 根因 |
| --- | --- |
| 表单样例装载后一个键都打不进去 | 装载器把引擎切成表单模式，而库的 `config.mode` 影子还是 document —— `configure` 按差集算成空操作 |
| 点一个像素，落点整体右偏一格 | `diag` 改引擎 zoom 之后，宿主的 `zoom.density` 没跟着改；命中的密度折算用了旧值 |
| 装载之后页面停在旧的滚动位置 | `scrollReset` 只有一个标记，`sendState` 打包时就清掉了，in-thread 宿主永远读到 false |
| `setText(text, {repeat})` 只出 1 段 | 契约里有 `repeat`，实现没传 |
| 装载命令的收尾（滚动归零 / 光标事件）不跑 | `loadBlocks` 回 `undefined`，而 `edit()` 按返回值决定要不要收尾 |
| `diag.removeControl` 永远回 true | 硬编码，引擎拒绝了也报成功 |

剩下的缺口是 **worker 传输**（46 条），集中在三处：贴图账（`px=0`，像素在另一条线程上贴）、
页面窗口（点第二页会把窗口拉到第 9 页）、以及若干像素取证。这三处都是"账在那边、读数在这边"，
要按 `state` 把账推过来，而不是在本机重算。

C 步抓到的两个真 bug（都不是新代码引入的，是判据缺席）：

| 症状 | 根因 | 判据 |
| --- | --- | --- |
| 单击落得准、拖选掉到下一行 | 输入层自己除密度、拖选没除（1.33 倍） | `input_probe` 拖选那节 |
| Worker 模式单击不落光标、拖选永远空 | `read()` 的 default 把必经命令 `hit` 吞成 `null` | 同上 |

两条都是 **API 面全绿**时存在的：`e.type()` 走命令层，碰不到输入层。这也是 `input_probe`
存在的理由 —— 它发的是真实键盘 / 鼠标 / 合成事件，不调 API。

---

## 15. 功能缺口（不是接口问题，是实现没有）

| 缺口 | 现状 | 挡住的场景 |
| --- | --- | --- |
| **插入图片 / 公式** | 没有 `InsertAtom` 命令，行内原子只能从文档载入 | 富文本 |
| **字符格式**（字体/字号/颜色/粗斜下划线） | 没有"对选区应用格式"的命令；`FormatRun` 里也没有粗斜下划线 | 富文本 |
| **查找 / 替换** | 没有 | 长文档 |
| **运行期改页度量** | `setPageMetrics` 只在装载路径 | 套打、信纸 |
| **多实例共享字体字节** | 每个实例各读 16.9 MB | 多实例页 |
| **双击选词** | `clickCountOf` 只判三击（选整格），seq=2 照常落光标。旧宿主也没有 | 常规文字编辑 |

前两条挡的是"这算不算编辑器"这个判断本身：**不能插图、不能加粗，就不算编辑器。**
它们都是引擎侧的工作，与 API 层并行，别指望 API 层绕过。

**剪贴板已经不是缺口了**（2026-10-22）：`copy()` / `cut()` / `paste()` 三个方法，加上
键盘的 Ctrl+C / X / V。做法与判据见第 15.1 节。

### 15.1 剪贴板：一份片段，两条通道

片段是**结构**而不是文字：`rev::captureRange` 按区间取出来的是一串 `NodeSpec`（文本节点、
段落标记、控件、表格、图片、公式连同各自的记录），`rev::writeFragment` 把它写成一段自带
字符串表与字体表的 JSON。字体表与文档落盘同一份（PostScript 名 → 族名 → 文件名），
于是**跨实例、跨标签页**粘回来是同一段内容，不依赖节点号，也不依赖两边的字体装载顺序。

三条粒度规则，都是"宁可整块"：

- 端点落在控件内部 → 整块控件一起复制；
- **碰到表格的一部分就复制整张表** —— 行与格离开表格不是可粘贴的内容，而"半张表"没有
  可表达的形状；要支持"复制几格"得先有"按格裁剪一张表"的构造；
- 节与文档是外壳，不复制成节点，碰到就下沉一层，把内容提上来。

两条通道按数据从哪来分：

- `application/x-heditor-fragment` 给本编辑器：copy / cut 事件里写进 `clipboardData`
  （同线程传输，同步拿到载荷），Worker 传输拿不到事件的 DataTransfer，退到
  `navigator.clipboard.write` 的 `ClipboardItem`；
- `text/plain` 给别的应用：段落标记折成换行、表格格之间是制表符；反过来粘贴时按
  CRLF / CR / LF 拆段（`pastePlainText`）。

粘贴与删选区都是命令（自带重排与呈现），与打字同一条落点判定；**粘一次是一条事务**，
撤销一次就回到粘贴之前。块级元素（表格）进不了段落内部，粘贴时会在它前后各补一枚段落
标记，让它自成段 —— 已经落在段首 / 段尾时不补，免得凭空多出空段落。

判据：`rev_test` [25]（22 条：控件不被摊平、纯文本投影、写两遍字节相同、读回来再写一遍
字节相同、插进空文档后段数与文本都与来源相同、表格扩到整张表）；`clipboard` spec 9 条
（文字、段落标记、控件保真、剪切、外部纯文本、撤销、键盘、跨实例、空选区）× 两条传输。


---

## 16. 开放问题

1. `input: false` 要不要暴露？无头探针需要它，产品页不需要。倾向暴露，并在文档里写"给自动化用"。
2. `open()` 的格式自辨够不够稳？"纯文本以 `{` 开头"会误判 —— 用 `format` 选项兜底。
3. 多实例：同一个容器 `createEditor` 两次要抛（现在不检查）。
4. `change` 是否也合批（连续输入）？倾向不合批（每次变更都要知道），但文档要写清
   "一次输入法提交可能产生一次 change"。
5. `payloads.json` 的粒度只到顶层键：嵌套对象在 `.d.ts` 里是 `Record<string, unknown>`。
   `sheet` / `table` 那几份嵌套是有形状的，要不要补一层 overlay，等真正用到 `diag` 的人出现再说。
6. `pk::checked` 在生产构建里也抛吗？倾向抛 —— 它是 bug，不是用户输入；但要在 wasm 边界
   留一条"抛出会被包成 JS 异常"的说明，别让宿主把它当成可恢复错误。
7. `Object.keys(editor)` 在 `debug: false` 时不含 `diag`：`api_check.mjs` 要按模式比，
   否则这条判据在两种模式下必有一种是红的。
