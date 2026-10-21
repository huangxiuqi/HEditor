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
| **A 只读展示** | 合同预览、报告归档页 | `createEditor` `open` `geometry` `scrollTo` `configure({readOnly})` `on('error')` `destroy` |
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
  transport: 'worker',            // 'worker' | 'inline'
  assets: { glue?, wasm? },       // 资源定位，默认按当前脚本地址推
  fonts: [...],                   // 不传用内置默认
  config: { readOnly: true },     // 初始配置
  chrome: false,                  // 是否画工具条
});

await editor.open(savedJson);     // 内容走方法，不走构造参数
```

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

## 5. 方法表（33 个方法 + 2 个只读属性）

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

### 5.3 光标与选区（程序化定位）

| 签名 | 备注 |
| --- | --- |
| `caret()` | 同步读本地镜像 |
| `setCaret(pos, opts?)` | `{ side?: 'before'\|'after', line?: 'first'\|'last' }` —— 合并原来 4 个方法 |
| `moveCaret(step)` | `'left'\|'right'\|'up'\|'down'\|'lineStart'\|'lineEnd'\|'docStart'\|'docEnd'`，合并 6 个 |
| `select(anchor, focus)` / `selectAll()` / `clearSelection()` | |
| `selection()` | 同步读，含跨页矩形与单元格区间 |

### 5.4 控件（表单）

| 签名 | 备注 |
| --- | --- |
| `focusControl(index)` | |
| `setControlProp(index, prop, value)` | `prop` 是字符串（`text` / `checked` …），值按控件种类校验 |
| `form()` | 精简视图：`[{ index, kind, label, value, focused }]`，给"整表读一遍"的宿主 |

`removeControl` 不进产品面：现实里删控件是编辑态动作，由输入层负责（选中控件 + Delete）；
程序化删控件只在测试里出现 → `diag`。

### 5.5 视图与配置

| 签名 | 备注 |
| --- | --- |
| `geometry()` | 页尺寸 / 间距 / 页数 / 滚动高度 / 当前窗口 |
| `scrollTo({ page?, y? })` | |
| `setZoom(z)` | 转发 `configure({ zoom })`，为可读性保留 |
| `configure(patch)` | 唯一配置写入口（§7） |
| `config()` | 只读快照 |

### 5.6 生命周期与事件

`on(event, cb)` · `flush()` · `destroy()` · `element` · `diag`

`flush()` 只在 `transport: 'worker'` 时有用（in-thread 上是已 resolve 的 Promise）：
批量 fire-and-forget 之后用它一次，比每个方法都 `await` 便宜。

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
| 构造期固定 | `container` `transport` `assets` `fonts` | 换容器要重建 DOM；换传输要重建 Worker；字体装一次最贵 |
| 运行期可改 | `zoom` `pageMode` `readOnly` `input` `showBrackets` `showControlDots` `undoLimit` `caretStyle` `selectionStyle` | 有明确语义，代价可分级 |
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
| `paint` | `readOnly` `input` `showControlDots` `caretStyle` `selectionStyle` |
| `relayout` | `zoom` `pageMode` `showBrackets` |
| `reload` | `fonts`（构造期） |

### 7.4 规范化与错误

类型不符 / 枚举越界 / `NaN` / 未声明的键 / 只读项 → **同步抛**（入队之前）。
数值越界（`zoom: 100`）→ 夹取，返回值里给生效后的数。

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
| 迁移期（B–C） | `web/types/editor.d.ts`（手写）+ JSDoc `@typedef` | `tsc --noEmit`；实现对象的类型必须**推断**出来 |
| 重写后（D） | `src/types.ts` 的 `export interface Editor` | `createEditor(): Promise<Editor>` 显式返回类型 |
| 发布 | `dist/index.d.ts`（生成） | `src/index.ts` **只导出契约，不导出实现** |

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
- `npm run dev` → 开发服务器：`demo/index.html?transport=worker|inline` 直接跑，改 `src/` 热更新。
- `demo/` 与现在的 `web/index.html` 同构 —— 探针可以原样指过去，判据不用重写。
- **demo 页只加载 `heditor.js` 一个脚本**（§11 闸 1）；它不再知道 wasm、胶水、命令表的存在。

三件事的边界要分清：**CMake 管 wasm 与它的声明**、**vite 管库与 demo**、**tsc 管类型检查**。
vite 不做类型检查（只擦除类型），所以 `npm run build` 之后必须再跑一次 `tsc --noEmit`。

### 13.2 目录

```
package.json          exports / types / files / sideEffects / private（见 13.4）
                      scripts: build / dev / typecheck（tsc --noEmit）/ check（四条判据）
tsconfig.json         strict，noEmit（类型检查与构建分开；include 只有 src 与 types）
vite.config.ts        build.lib + dts 插件 + wasm 拷贝 + dev 中间件
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

---

## 14. 迁移顺序

| 步 | 做什么 | 判据 |
| --- | --- | --- |
| A ✅ | 契约 `web/types/editor.d.ts` + 本设计 | `tsc --strict` 0 错（已过） |
| A2 ✅ | **构建与类型**：tsc 进 CMake（§12）、`--emit-tsd`、`.gitignore`、`scripts/deps_tsc.ps1` | 构建产出 `web/types/wasm.gen.d.ts`（14231 字节 / 107 个导出）；**藏起 tsc.cmd 后 configure 退出码 1**（已实测） |
| A3 ✅ | **载荷契约**：`gen_payloads.py` + `payloads.json` + `pk::checked` 接进 16 个写出函数 | 16 份 / 176 键；`--check` 绿；改一个键 → `--check` 红（已实测）；不重生成直接跑 → 报"载荷 auditPage 的键与契约不符（第 1 个）"（已实测） |
| B ✅ | `package.json` + vite + `web/src/`（公开层）+ `web/demo.html` | `tsc --noEmit` 0 错；`api_check.mjs` 通过；`demo_probe` 22 项全绿 |
| C | 把 `app.js` / `worker_host.js` 的必要部分搬进 `src/`（一次一个模块），输入层进库，`transport: 'worker'` 接通 | 每搬一个跑一遍探针；去掉 `private` 之前必须自包含；两个宿主共用一个 `Editor` 实现 |
| D | 探针迁到 `diag`；`__heditor` 变成 `createEditor` 的返回值 | 探针条数不变；旧成员清零 |
| D2 | **契约搬家**：`web/types/editor.d.ts` → `src/types.ts`；`dist/index.d.ts` 改由 `tsc --declaration` 生成 | `api_check.mjs` 证明搬前搬后成员集合逐条相同；`dist` 里不出现实现类的任何成员 |
| E ✅ | `web/demo.html` + `demo_probe.mjs` | demo 里 `window.__heditor === undefined`；未知成员抛；`createEditor` 已被撤掉 |

A2/A3 排在最前面的理由：它们**不改任何运行行为**，却决定了后面每一步的判据能不能红。
反过来先做 B 的话，`src/` 一写出来就没有类型可依 —— 那正是"毛坯房"再来一次。

B 落地时与设计有三处出入，都是被现实逼出来的：

- **demo 放在 `web/demo.html`，不是 `demo/`**。旧宿主的字体与 wasm 路径都按 `new URL('../', location.href)`
  解析（约定是"页面在 web/ 下"），搬到 `demo/` 之后所有资源都 404。等 `assets` 选项真正接管路径
  再搬，那时页面只需要一个 `<script>`。
- **`transport` 只接了 `'inline'`，默认值暂时也是它**。Worker 那条要等 C 步把输入层搬进库；
  现在强接只会把"消息表在两边各写一份"的老问题带进来。传 `'worker'` 会抛并指向 C 步。
- **`facade.__api` 是内部隐藏通道**（不可枚举）。`open` / `insertText` / `geometry` 这类方法在旧门面
  上没有对应入口，公开层要拿命令表实例。它不出现在 `Object.keys` 里、`api_check` 禁止 demo 使用，
  C 步之后消失。

两处按引擎能力修正了契约：`focusControl(dir)` 不是按序号（引擎只有"前进/后退"），
`setControlProp(index, prop, value)` 的 prop 是数字属性号、value 目前只有布尔。

`api_check.mjs` 是新判据（A2 就可以先用一个只查契约成员集合的版本）：把 `editor` 的成员集合
与 `editor.d.ts` 逐条比，多一个少一个都红。它拦的正是"实现偷偷多长出一个方法"——这一条现在
没有，所以 111 个成员才长得出来。

---

## 15. 功能缺口（不是接口问题，是实现没有）

| 缺口 | 现状 | 挡住的场景 |
| --- | --- | --- |
| **剪贴板 Ctrl+C/V/X** | `host_core.js` 对这三个键 `return null`（放行浏览器）→ 复制到的是承接层那行假文本，粘贴没接。`fragment.h` 的 `extractRange` 与 `InsertFragment` 步骤都在，缺引擎命令 | 所有编辑场景 |
| **插入图片 / 公式** | 没有 `InsertAtom` 命令，行内原子只能从文档载入 | 富文本 |
| **字符格式**（字体/字号/颜色/粗斜下划线） | 没有"对选区应用格式"的命令；`FormatRun` 里也没有粗斜下划线 | 富文本 |
| **查找 / 替换** | 没有 | 长文档 |
| **运行期改页度量** | `setPageMetrics` 只在装载路径 | 套打、信纸 |
| **多实例共享字体字节** | 每个实例各读 16.9 MB | 多实例页 |

前三条挡的是"这算不算编辑器"这个判断本身：**不能复制粘贴、不能插图、不能加粗，就不算编辑器。**
它们都是引擎侧的工作，与 API 层并行，别指望 API 层绕过。

建议先补**剪贴板**：基础设施已齐（`extractRange` + `InsertFragment`），缺两条命令与前端接线；
它同时是场景 C 与"快捷键在内部实现"这条要求的硬前置。

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
