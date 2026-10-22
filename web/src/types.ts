/**
 * HEditor 外部 API 契约（真源）。
 *
 * 实现就在同目录的 editor.ts 里，与这份契约共用同一套类型 —— 不再有一份手写的
 *   `.d.ts` 与实现并行维护：`web/dist/index.d.ts` 由 `tsc --declaration` 从这份文件生成。
 * 设计说明见 docs/外部API设计.md。
 *
 * 四条约定：
 *   · 坐标一律 CSS 像素、页内相对坐标（页号单独给）；`offset` 是段内**码点**，不是 UTF-16 单元。
 *   · 键盘、快捷键、IME、拖选、滚动都在库内部 —— 外部不需要写任何 DOM 事件。
 *   · 返回 Promise 的方法在"引擎处理完 + 状态回推完"之后 resolve；查询类同步返回本地镜像。
 *   · diag 的成员不承诺跨版本稳定，参数与返回暂为 unknown（载荷形状由 schema 生成后替换）。
 */

/** 文档内位置。offset 是段内码点序号（0 起）。 */
export interface Pos {
  para: number;
  offset: number;
}

/** 一块页面矩形（CSS 像素，页内相对坐标）。 */
export interface Rect {
  page: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

export type RGBA = [number, number, number, number];

/** 光标：位置 + 落在哪一页 + 页内矩形。 */
export interface Caret {
  pos: Pos;
  page: number;
  rect: { x: number; y: number; w: number; h: number };
  visible?: boolean;
}

/** 选区：两端位置 + 每页上的矩形（跨页会是多条）。 */
export interface Selection {
  anchor: Pos;
  focus: Pos;
  empty: boolean;
  rects: Rect[];
  /** 单元格选区时给出格区间；文字选区为 null。 */
  cells: { rows: [number, number]; cols: [number, number] } | null;
}

/** 视口与页度量（CSS 像素）。 */
export interface Geometry {
  page: { w: number; h: number };
  gap: number;
  pages: number;
  scrollHeight: number;
  window: { first: number; count: number };
  dpr: number;
}

export interface CaretStyle {
  color: RGBA;
  widthPt: number;
}

export interface SelectionStyle {
  color: RGBA;
}

/**
 * 运行期可改的配置。每项带一个代价标签（none / paint / relayout / reload，见设计文档 §7.3）；
 * 一次 configure 只做其中最贵的那一档 —— `configure({zoom, showControlDots})` 是一次重排，
 * 不是"一次重排又一次重绘"。
 */
export interface EditorConfig {
  /** 倍数，1 = 100%（96 dpi）。引擎内部收的是密度（4/3 基准），换算只在接缝上做。 */
  zoom: number;
  pageMode: 'paged' | 'continuous';
  /**
   * 编辑模式。三档都是产品语义，不是探针口径：
   *   document —— 随便改（默认）；
   *   form     —— 光标自由，但只有可编辑控件里的字改得动（填表）；
   *   readOnly —— 光标自由，一个字都不许改（预览）。
   * 只读时库的编辑方法直接返回 false；真正的拒绝仍由引擎判定（blockedEdits 记账）。
   */
  mode: 'document' | 'form' | 'readOnly';
  /** 是否由库内部接管键盘/指针/IME。默认 true；false 只给"自己驱动"的自动化宿主。 */
  input: boolean;
  /** 段落标记括号。会改变行宽，**要重排**。 */
  showBrackets: boolean;
  /** 控件小点。只改光栅签名，不重排。 */
  showControlDots: boolean;
  /** 撤销栈深度，0 = 不限。 */
  undoLimit: number;
  caretStyle: CaretStyle;
  selectionStyle: SelectionStyle;
}

/** 资源定位。不传时按当前脚本地址推（ESM 下可用；UMD 下必须显式给）。 */
export interface Assets {
  glue?: string;
  wasm?: string;
  /**
   * 迁移期：旧宿主脚本（app.js 等）所在目录。默认按本库脚本的地址推（`../`）。
   * C 步把它们搬进 src/ 之后这一项会删掉。
   */
  base?: string;
}

export interface FontSource {
  /** font-face 名（同时是三级匹配的第一级）。 */
  family: string;
  /** 取字体的地址（相对或绝对）。 */
  url: string;
  /** 角色：正文 / 界面 / 等宽。不传按顺序补位。 */
  role?: string;
}

/** 构造期参数：只管"挂在哪、怎么配"，内容走 open() / setText()。 */
export interface EditorOptions {
  container: HTMLElement;
  /** 默认 'worker'。'inline' 在主线程跑（测试环境、或不允许 Worker 的宿主）。 */
  transport?: 'worker' | 'inline';
  assets?: Assets;
  fonts?: FontSource[];
  config?: Partial<EditorConfig>;
  /** 是否画工具条。默认 false：产品页只给舞台。 */
  chrome?: boolean;
  /**
   * 诊断面开关。默认 false：实例上没有 `diag`，访问会抛。
   *
   * 探针与审计带 true；产品页不带 —— "能不能依赖 diag"在代码里是显式的，
   * 而不是靠一句"不承诺稳定"的注释。
   */
  debug?: boolean;
}

/** 光标移动的档位：行的首尾 vs 整篇的首尾。 */
export type CaretStep =
  | 'left'
  | 'right'
  | 'up'
  | 'down'
  | 'lineStart'
  | 'lineEnd'
  | 'docStart'
  | 'docEnd';

export interface SetCaretOptions {
  /** 落在字符前还是后（点击落在字的左半边还是右半边）。 */
  side?: 'before' | 'after';
  /** 同一段里有多行时落第几行。 */
  line?: 'first' | 'last';
}

/** 表单里的一个控件（form() 的精简视图）。 */
export interface ControlSummary {
  index: number;
  kind: string;
  label: string;
  value: unknown;
  focused: boolean;
}

/** 内容变更。 */
export interface ChangeEvent {
  source: 'user' | 'api';
  /** 内容版本，每次变更 +1。可用来跳过重复处理。 */
  version: number;
  /** 整篇纯文本（段间 \n）。引擎里本来就坐着，几乎零成本。 */
  text: string;
  /**
   * 带格式的整篇 JSON。**方法**而不是属性：调它才序列化，同一版本只算一次。
   *
   * 2000 段文档序列化一次约 55 ms（Worker 传输还多一次往返）—— 每次 change 都读会
   *   直接卡住键盘。写成方法是为了让"读它"显式：属性会被 JSON.stringify(ev) 顺带算出来，
   *   而那正好是最糟的一拍。大文档请只标脏，debounce 之后调 `await editor.save()`。
   */
  json(): Promise<string>;
}

/** 事件表。on() 的键集就是这六个。 */
export interface EditorEvents {
  change: ChangeEvent;
  selection: Selection;
  caret: Caret;
  /** 呈现完成。pages = 这一拍重画的页数。 */
  render: { pages: number; ms: number };
  /** 生效值真的变了才发（设成同一个值不发）。 */
  configchange: { changed: Partial<EditorConfig>; config: EditorConfig };
  error: { message: string; fatal: boolean };
}

/**
 * 诊断面：给无头探针与审计用。**不承诺跨版本稳定**，产品代码不该依赖它。
 *
 * 参数与返回暂为 unknown：它们对应引擎内部的账本载荷（19 份 / 245 个键），
 * 形状由 schema 生成到 payloads.gen.d.ts 之后再把这里换成具体类型。
 */
export interface Diag {
  stats(): unknown;
  editStats(): unknown;
  edit(): unknown;
  auditFrame(): unknown;
  auditBase(): unknown;
  auditPage(): unknown;
  auditLayout(): unknown;
  auditCompile(): unknown;
  auditGlyphPages(): unknown;
  selfCheck(): unknown;
  markCheck(): unknown;
  rawFrame(): unknown;
  frameDigest(): number;
  frameW(): number;
  frameH(): number;
  present(): void;
  controls(): unknown;
  bracketSlots(): unknown;
  hit(x: number, y: number): unknown;
  controlAt(x: number, y: number): unknown;
  describeControls(): string;
  blockedEdits(): number;
  dirtyPages(): number[];
  pagePx(): { w: number; h: number; gap: number };
  pageCanvases(): unknown;
  pageCanvasList(): unknown;
  prewarmPage(page: number): void;
  bench(): unknown;
  caretReport(): string;
  caretLive(): number;
  env(): unknown;
  fonts(): unknown;
  blitStats(): { ms: number; pages: number; px: number };
  lastBlit(): unknown;
  /** 按坐标点一下（自动化用；产品面不该用它替代 setCaret）。 */
  clickAt(pt: { x: number; y: number }): Promise<void>;
  /** 选中 pos 所在单元格（自动化用）。 */
  selectCell(pos: Pos): Promise<void>;
  /** 删控件；keepContent = true 是拆壳（内容留下来变成普通正文）。 */
  removeControl(index: number, keepContent?: boolean): Promise<boolean>;

  /* ── 装载：内置场景与压力文档 ── */

  /** 装载内置场景（id 同旧宿主：6 单段演示、7 多段长文）。zoom 是密度，不是倍数。 */
  loadScenario(id: number, zoom?: number): Promise<unknown>;
  /** 回到默认场景。 */
  reset(id?: number, zoom?: number): Promise<unknown>;
  /** 造 N 段、每段 M 字的压力文档（整篇同一个字）。 */
  stress(paras: number, chars: number): Promise<unknown>;
  /** 同上，但每段字不同；量字形缓存要用它，否则全命中失真。 */
  stressDiverse(paras: number, chars: number): Promise<unknown>;
  /** 表单样例（控件区间与只读标记都在里面）。 */
  loadFormSample(variant?: number): Promise<unknown>;
  /** 病历样例：读 assets/samples/medical-records.md，清洗后按段落建文档。 */
  loadMedicalSample(repeat?: number): Promise<unknown>;
  /** 整批重建控件（区间树的父子关系是全局性质，只能整棵换）。 */
  setFormControls(list: Array<{ start: number; end: number; kind?: number; id?: string; ro?: boolean }>): Promise<unknown>;

  /* ── 光标与相位：复现手势、定格一帧 ── */

  /** 带归属的定位（field 是控件序号）：复现"点后一个字段的开头"。 */
  setCaretAt(para: number, offset: number, field?: number): Promise<unknown>;
  /** 只改位置、不呈现 —— 给"改位置"与"呈现"分账的对照实验。 */
  setCaretOnly(para: number, offset: number): unknown;
  /** 直接设定光标可见相位（不重绘）。 */
  showCaret(on: boolean): unknown;
  /** 重启闪烁（回到可见相位）。 */
  startBlink(): void;
  /** 停表：光标常亮，也不重绘。 */
  stopBlink(): void;

  /* ── 字体与窗口 ── */

  /** 重装字体；mode = 'lazy' 走 MEMFS 按需读，'full' 整份交给引擎。 */
  reloadFonts(mode?: 'full' | 'lazy'): Promise<{ mode: string; bytes: number }>;
  /** 字体块缓存配置（字节 / 块数）。改完要重装字体才干净。 */
  setFontCacheConfig(blockSize: number, maxBlocks: number): unknown;
  /** 直接设窗口锚点（绕过滚动事件），量"换窗口要读多少字形"。 */
  setViewportAnchor(first: number, count: number): unknown;

  /** 页面级错误历史（与 error 事件同源，按发生顺序）。 */
  errors(): string[];
}

/**
 * 编辑器实例。
 *
 * 33 个方法 + `element` / `diag` 两个只读属性。分组只为可读，实例上是平铺的。
 */
export interface Editor {
  /* ── 内容 ── */

  /** 打开一份文档。首字符 `{` 且能解析出版本头按 JSON，否则按纯文本；失败时引擎状态不变。 */
  open(data: string, opts?: { format?: 'json' | 'text' }): Promise<void>;
  /** 用纯文本重置。repeat 复用同一段（压测用）。 */
  setText(text: string, opts?: { repeat?: number }): Promise<void>;
  /** 整篇文本（段间 \n）。 */
  text(): string;
  /** 第 i 段的文本；越界返回空串。 */
  paragraph(i: number): string;
  /** 整篇分段。 */
  paragraphs(): string[];
  /** 序列化成 JSON 文本。 */
  save(): Promise<string>;
  /** 在指定位置插入一段文本（模板填充），进撤销栈。 */
  insertText(pos: Pos, text: string): Promise<boolean>;

  /* ── 编辑（返回"有没有改动"） ── */

  type(text: string): Promise<boolean>;
  deleteBackward(n?: number): Promise<boolean>;
  deleteForward(n?: number): Promise<boolean>;
  splitParagraph(): Promise<boolean>;
  mergeParagraph(dir?: 'prev' | 'next'): Promise<boolean>;
  undo(): Promise<boolean>;
  redo(): Promise<boolean>;
  /** 增减缩进层级（正数加深，负数变浅）。 */
  setIndent(levels: number): Promise<boolean>;

  /* ── 光标与选区（程序化定位） ── */

  caret(): Caret;
  setCaret(pos: Pos, opts?: SetCaretOptions): Promise<void>;
  moveCaret(step: CaretStep): Promise<void>;
  select(anchor: Pos, focus: Pos): Promise<void>;
  selectAll(): Promise<void>;
  clearSelection(): Promise<void>;
  selection(): Selection;

  /* ── 控件（表单） ── */

  /**
   * 聚焦下一个 / 上一个控件。**不是按序号**：引擎的焦点只有"往前走/往回走"这一种操作，
   * 按序号聚焦得先知道每个控件的可编辑性（只读的会被跳过），那是引擎的判定不是宿主的。
   */
  focusControl(dir?: 'next' | 'prev'): Promise<void>;
  /** 改控件属性。prop 是引擎的属性号（见 ControlProp），value 目前只有布尔。 */
  setControlProp(index: number, prop: number, value: boolean): Promise<boolean>;
  form(): ControlSummary[];

  /* ── 视图 ── */

  geometry(): Geometry;
  /** 滚到某页或某个纵向位置（CSS 像素）。 */
  scrollTo(target: { page?: number; y?: number }): Promise<void>;
  /** 转发 configure({ zoom })，为可读性保留。 */
  setZoom(zoom: number): Promise<void>;

  /* ── 配置 ── */

  /**
   * 唯一配置写入口：一次调用 = 一次原子提交，只做最贵的那一档代价。
   * 返回**生效后**的完整配置（越界会被夹取）；拼错的键同步抛 TypeError。
   */
  configure(patch: Partial<EditorConfig>): Promise<EditorConfig>;
  config(): Readonly<EditorConfig>;

  /* ── 事件与生命周期 ── */

  /** 订阅；返回退订函数。 */
  on<K extends keyof EditorEvents>(event: K, cb: (ev: EditorEvents[K]) => void): () => void;
  /** 等队列排空（批量 fire-and-forget 之后用它一次，比逐个 await 便宜）。 */
  flush(): Promise<void>;
  /** 收起定时器、释放 Worker、移除 DOM。之后所有调用 reject。 */
  destroy(): Promise<void>;

  /** 宿主要的那一个 DOM 入口（内部画布/输入层不对外）。 */
  readonly element: HTMLElement;
  /**
   * 诊断面。**只有 `createEditor({ debug: true })` 时存在**；否则访问抛 TypeError。
   * 探针与审计用，产品代码不该依赖它。
   */
  readonly diag: Diag;
}

/** 造一个编辑器。返回时已就绪（排版跑完、首帧画完、输入层已接上）。 */
export declare function createEditor(opts: EditorOptions): Promise<Editor>;

declare global {
  interface Window {
    /** <script src="heditor.js"> 之后挂在全局（UMD 产物）。 */
    HEditor: {
      createEditor: typeof createEditor;
    };
  }
}
