import type {
  Caret,
  CaretStep,
  ChangeEvent,
  CharFormatPatch,
  ControlForm,
  ControlInsertOptions,
  ControlSummary,
  ControlUpdateOptions,
  FormOptions,
  FormValues,
  RGBA,
  ControlValueInput,
  Diag,
  Editor,
  EditorConfig,
  EditorEvents,
  EditorOptions,
  FontInfo,
  FontInput,
  Geometry,
  CustomPaper,
  ImageBox,
  ImageHit,
  ImageInsertOptions,
  ImageLayout,
  ImageRegisterOptions,
  PageSetup,
  LineSpacingRule,
  ParaFormatPatch,
  Pos,
  Selection,
  SetCaretOptions,
  TableCellOptions,
  TableForm,
  TableOp,
  TableOptions,
  TableGrid,
  TableRowOptions,
  TableState,
  WatermarkOptions,
  WatermarkState,
} from './types';
import { PAPER_PT, paperSizePt } from './types';
import {
  controlFormOf,
  controlWriteOf,
  pageSetupFormOf,
  pageSetupWriteOf,
  tableFormOf,
  tableWriteOf,
} from './forms';
import { createDomHost, ensureMarkup, type ChangeSource, type DomHost, type EngineLink } from './host/dom';
import { createErrorSurface, type ErrorSurface } from './host/errors';
import { scrollHeightPx, type CanvasStore } from './host/pages';
import { stageOf } from './host/parts';
import { cleanMarkdownLines, then1, type Args, type ZoomControl } from './input/keymap';
import { assertSimd, simdProbeBytes, simdSupported } from './engine/simd';
import { resolveFonts, type AssetPaths } from './transport/assets';
import { createWorker } from './transport/worker';
import { guard } from './guard';

// 素材地址

/**
 * 库自己的地址。打包成 IIFE 时靠 document.currentScript 推；ESM 下退回落 document.baseURI。
 *
 * 都在"页面所在地"与"库所在地"之间二选一，而不是猜仓库结构：使用者的目录布局与这里
 *   无关，所以 assets 里的每一项都能单独覆盖。
 */
const SELF_SRC: string =
  (document.currentScript instanceof HTMLScriptElement && document.currentScript.src) || '';

function selfDir(): string {
  if (SELF_SRC) return new URL('.', SELF_SRC).href;
  return new URL('.', document.baseURI).href;
}

/**
 * 素材地址。
 *
 * 默认布局是"库产物在 web/dist/、页面在 web/、wasm 在 build/wasm/web/"。这不是约定，
 *   只是本仓库开发时的摆法 —— 产品里三项都该显式给（assets.glue / assets.wasm / assets.base）。
 */
async function resolveAssets(opts: EditorOptions): Promise<AssetPaths> {
  const a = opts.assets ?? {};
  // 默认布局：库在 web/dist/，素材在仓库根下（assets/、build/wasm/web/）。
  //   所以默认 base 是"库目录往上两级"。这不是约定，只是本仓库开发时的摆法 ——
  //   产品里三项都该显式给。
  const base = a.base ? new URL(a.base, document.baseURI).href : new URL('../../', selfDir()).href;
  const glue = a.glue ? new URL(a.glue, document.baseURI).href : new URL('build/wasm/web/heditor.js', base).href;
  // wasm 默认由胶水按自己的目录推；显式给了就按它取（换 CDN、多版本并存时用）。
  const wasm = a.wasm ? new URL(a.wasm, document.baseURI).href : '';
  // Worker 壳默认与库产物同一目录（web/dist/heditor_worker.js），也可以显式给 ——
  //   示例页与探针用的是后一条（它们的页面不在库产物那一层）。
  const worker = a.worker ? new URL(a.worker, document.baseURI).href : new URL('heditor_worker.js', selfDir()).href;
  return { glue, base, worker, wasm, bust: '', fonts: await resolveFonts(base, opts.fonts) };
}

// 事件

class Emitter {
  private map = new Map<string, Set<(ev: never) => void>>();

  /**
   * 订阅一个事件。
   * @param event 事件名
   * @param cb 回调
   * @returns 退订函数
   */
  on<K extends keyof EditorEvents>(event: K, cb: (ev: EditorEvents[K]) => void): () => void {
    let set = this.map.get(event);
    if (!set) {
      set = new Set();
      this.map.set(event, set);
    }
    set.add(cb as (ev: never) => void);
    return () => set?.delete(cb as (ev: never) => void);
  }

  /**
   * 发一次事件（没有监听者时什么都不做）。
   * @param event 事件名
   * @param ev 事件载荷
   */
  emit<K extends keyof EditorEvents>(event: K, ev: EditorEvents[K]): void {
    const set = this.map.get(event);
    if (!set) return;
    for (const cb of set) (cb as (e: EditorEvents[K]) => void)(ev);
  }
}

// 默认配置

/**
 * 编辑模式 -> 引擎的整数。顺序与 EditMode 枚举一致。
 */
const MODE_CODE: Record<EditorConfig['mode'], number> = {
  document: 0,
  form: 1,
  readOnly: 2,
};

/**
 * 编辑标记档位 -> 引擎的整数。顺序与 MarksMode 枚举一致。
 */
const MARKS_CODE: Record<EditorConfig['showMarks'], number> = {
  none: 0,
  paragraph: 1,
  all: 2,
};

/**
 * 控件选项 -> 传给引擎的那一份。
 *
 * 只放给出的键：没给的保持不出现，引擎读成"这一项不动"。空数组与没给是两件事
 * （清空选项与不改选项），所以这里按键判断，不用空数组代替。
 * @param o 控件选项
 * @returns 引擎侧载荷
 */
function controlOptsFrom(o: ControlInsertOptions | ControlUpdateOptions): Record<string, unknown> {
  const opts: Record<string, unknown> = {};
  if (o.type !== undefined) opts.type = String(o.type);
  if (o.key !== undefined) opts.key = String(o.key);
  if (o.label !== undefined) opts.label = String(o.label);
  // 提示文字与 label 是两件事：label 进内容（文本类），提示只在内容为空时出现。
  if (o.placeholder !== undefined) opts.ph = String(o.placeholder);
  // 选项表：值与标签分开送，值进数据、标签进纸面。
  if (o.options !== undefined) opts.options = o.options.map((x) => ({ v: x.value, l: x.label }));
  if (o.group !== undefined) opts.group = String(o.group);
  if (o.freeInput !== undefined) opts.free = !!o.freeInput;
  if (o.format !== undefined) opts.format = String(o.format);
  if (o.dateUnits !== undefined) opts.units = dateUnitsMask(o.dateUnits);
  if (o.glyphColor !== undefined) opts.glyph = o.glyphColor;
  return opts;
}

/**
 * 日期单位 -> 位掩码。顺序与 kDateUnitXXX 一致（引擎按它决定选择器里能选哪些单位）。
 */
const DATE_UNIT_BITS: Record<string, number> = {
  year: 1,
  month: 2,
  day: 4,
  hour: 8,
  minute: 16,
  second: 32,
};

/**
 * 位掩码 -> 日期单位名列表。顺序按年月日时分秒，与界面一致。
 * @param mask 位掩码
 * @returns 单位名
 */
function dateUnitsOf(mask: number): Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'> {
  const names = ['year', 'month', 'day', 'hour', 'minute', 'second'] as const;
  return names.filter((_, i) => (mask & (1 << i)) !== 0);
}

/**
 * 日期单位名列表 -> 位掩码。
 * @param units 单位名（year / month / day / hour / minute / second）
 * @returns 位掩码
 */
function dateUnitsMask(units: string[]): number {
  let mask = 0;
  for (const u of units) mask |= DATE_UNIT_BITS[u] ?? 0;
  return mask;
}

/**
 * 值形态 -> 引擎的整数。顺序与 ValueShape 枚举一致（引擎按它决定值放哪、怎么读）。
 */
const SHAPE_CODE: Record<NonNullable<ControlInsertOptions['shape']>, number> = {
  text: 0,
  bool: 1,
  enum: 2,
  list: 3,
  resource: 4,
};

/**
 * 页间隙（磅）：分页档用它，连页档是 0。
 */
const kPageGapPt = 14;

/**
 * 默认页边距（磅）= 2 厘米。与 api.ts 里 boot 用的那一组数是同一个值：两边不一致的话，
 * 首帧按一组数排、配置影子里记着另一组，第一次 configure 会把页面平白挪一次。
 */
const kPageMarginPt = 56.7;

/**
 * 页面设置的归一：纸张按表认，方向取布尔，四边页边距按磅收。
 *
 * 边距不设"合理的下限"，但必须给正文留下位置 —— 上下（左右）各占一半页高（页宽）时
 *   contentWidthPt / contentHeightPt 就变成 0，断行的可用范围是零，整篇内容会挤成一列
 *   零宽的行。所以每一对边距的上限是"一半再减 1 磅"。
 *
 * @param patch 调用方给的那一份（可以只给其中几项）
 * @param base 当前生效的那一份（没给的项从这里取）
 * @returns 归一之后的页面设置
 */
function normalizePageSetup(patch: Partial<PageSetup>, base: PageSetup): PageSetup {
  const paper = patch.paper ?? base.paper;
  if (paper !== 'Custom' && !(paper in PAPER_PT)) {
    throw new TypeError(`未知纸张规格 ${String(paper)}`);
  }
  const landscape = patch.landscape === undefined ? base.landscape : !!patch.landscape;
  const raw = { ...base.margins, ...(patch.margins ?? {}) };
  // 自定义宽高：'Custom' 时必须有正的宽高，固定规格时只把旧值留着（切回去不用重输）。
  const custom = normalizeCustomPaper(patch.custom ?? base.custom, paper);
  const dims = paperSizePt(paper, landscape, custom);
  const clamp = (v: unknown, limit: number): number => {
    const n = Number(v);
    if (!Number.isFinite(n) || n < 0) return 0;
    return Math.min(n, limit);
  };
  const halfH = Math.max(0, dims.h / 2 - 1);
  const halfW = Math.max(0, dims.w / 2 - 1);
  return {
    paper,
    landscape,
    ...(custom ? { custom } : {}),
    margins: {
      top: clamp(raw.top, halfH),
      right: clamp(raw.right, halfW),
      bottom: clamp(raw.bottom, halfH),
      left: clamp(raw.left, halfW),
    },
  };
}

/**
 * 自定义纸张宽高的归一。上限 5000 磅（约 1.76 米）、下限 36 磅（半英寸）：再小就放不下
 * 一行字，再大只是把排版撑到没人看。
 *
 * @param c 调用方给的宽高（磅，纵向口径）
 * @param paper 纸张规格：'Custom' 时缺宽高就直接抛错，别的规格只是把旧值留着
 * @returns 归一之后的宽高；没有可留的值时为 undefined
 */
function normalizeCustomPaper(
  c: Partial<CustomPaper> | undefined,
  paper: PageSetup['paper'],
): CustomPaper | undefined {
  const limit = (v: unknown): number => Math.min(5000, Math.max(36, Number(v)));
  const has = c && Number.isFinite(Number(c.w)) && Number.isFinite(Number(c.h));
  if (!has) {
    if (paper === 'Custom') throw new TypeError('paper = "Custom" 时必须给 custom: { w, h }（磅）');
    return undefined;
  }
  return { w: limit(c!.w), h: limit(c!.h) };
}

const DEFAULT_CONFIG: EditorConfig = {
  zoom: 1,
  pageMode: 'paged',
  page: {
    paper: 'A4',
    landscape: false,
    margins: { top: kPageMarginPt, right: kPageMarginPt, bottom: kPageMarginPt, left: kPageMarginPt },
  },
  mode: 'document',
  input: true,
  showBrackets: true,
  showControlDots: false,
  showMarks: 'paragraph',
  marginMarks: true,
  undoLimit: 100,
  caretStyle: { color: [0, 0, 0, 255], widthPt: 0.5 },
  selectionStyle: { color: [31, 111, 235, 64] },
};

// 工厂

/**
 * 造一个编辑器。返回时已就绪（排版跑完、首帧画完、输入层已接上）。
 *
 * 两个传输共用这一份实现：差别只有"结果何时到"（同步值 / Promise）与"片元在哪贴"
 *   （这里 / Worker），两者都收在 EngineLink 里。
   * @param opts 挂载点、传输、素材、初始配置
   * @returns 已就绪的编辑器实例（排版跑完、首帧画完、输入层已接上）
 */
export async function createEditor(opts: EditorOptions): Promise<Editor> {
  if (!opts || !(opts.container instanceof HTMLElement)) {
    throw new TypeError('createEditor({ container }) 需要一个 HTMLElement');
  }
  if (opts.container.childElementCount > 0 && !stageOf(opts.container)) {
    // 不清空别人的内容：只提示。要复用一块已有标记的容器就让它带着舞台进来。
    throw new TypeError('container 里已经有内容了：请传一个空元素（或带 heditor-stage 的那一份）');
  }
  // 装载之前先确认这台浏览器能跑这份产物。它判的是"这份 wasm 能不能实例化"，与引擎
  //   在哪条线程上无关；而失败时唯一的目击者是 createEditor 的调用方 —— 让它拿到带
  //   标题与下一步的拒绝，而不是一条 CompileError。
  assertSimd();

  // 字体清单要在建引擎之前落地：地址按产物目录解析，字节当场读（File 是异步的）。
  //   读失败就当场拒绝 —— 留到 Worker 里抛的话，报出来的是"引擎启动失败"，与真实原因隔一层。
  const assets = await resolveAssets(opts);
  const emitter = new Emitter();
  const config: EditorConfig = { ...DEFAULT_CONFIG, ...(opts.config ?? {}) };
  let version = 0;
  let disposed = false;
  // 页面级错误历史（diag.errors 读它）。与 error 事件同源 —— 事件是"发生的那一刻"，
  //   这份是"发生过哪些"：探针在收尾时才读，那时事件早就过去了。
  const pageErrors: string[] = [];

  const alive = (): void => {
    if (disposed) throw new Error('编辑器已销毁');
  };

  let link: EngineLink;
  let dom: DomHost;
  let starter: () => Promise<void>;
  let destroyer: () => void;

  // 失败要看得见：这一层挂在 .heditor-body 上（绝对定位需要一个定位过的祖先），所以先把
  //   标记建出来。两档的分界见 host/errors.ts 的文件头。
  ensureMarkup(opts.container);
  const surface: ErrorSurface = createErrorSurface({
    mount: opts.container.querySelector<HTMLElement>('.heditor-body')!,
    // 标在调用方给的容器上（不是库自己建的 .heditor-body）：页面 CSS 认的是它。
    mark: opts.container,
  });

  /**
   * 报一次失败：记账 + 上屏 + 事件。三处同源，少一处就会有一类失败只在某一个入口可见。
   */
  const reportError = (
    message: string,
    fatal: boolean,
    info?: { detail?: string; hint?: string },
  ): void => {
    if (disposed) return;
    pageErrors.push(message);
    // 恢复入口由调用方给（库不能重启自己），只在致命那一档才用得上。
    if (fatal) surface.fatal(message, { ...info, retry: opts.onFatalRetry });
    else surface.warn(message);
    emitter.emit('error', { message, fatal });
  };

  const transport = createWorker({
    container: opts.container,
    assets,
    dpr: window.devicePixelRatio || 1,
    zoom: config.zoom * (4 / 3) * (window.devicePixelRatio || 1),
    empty: true,
    makeHost: (l, store, remoteBlit) => createDomHost(hostOptions(l, store, remoteBlit)),
    // 起来之后才死的那一类：没有 Promise 可拒，只剩"让用户看见"。
    onFatal: (message) => reportError(message, true, {
      detail: message,
      hint: '这一页的编辑器已经不可用；重新加载页面会重建引擎。',
    }),
  });
  link = transport.link;
  dom = transport.host;
  starter = () => transport.start();
  destroyer = () => transport.destroy();

  /**
   * 宿主用的选项。link 显式传进来 —— 它由传输造，宿主的回调不能靠闭包去猜。
   */
  function hostOptions(
    l: EngineLink,
    store: (pagesEl: HTMLElement) => CanvasStore,
    remoteBlit: () => { ms: number; pages: number; px: number },
  ): Parameters<typeof createDomHost>[0] {
    return {
      container: opts.container,
      link: l,
      input: config.input !== false,
      canvasStore: store,
      blitStats: remoteBlit,
      readOnly: () => config.mode === 'readOnly',
      status: (text, isErr) => {
        if (isErr) reportError(text, false);
      },
      onEdit: (changed: boolean, source: ChangeSource) => {
        if (!changed || disposed) return;
        ++version;
        emitter.emit('change', makeChange(source, version));
      },
      onCaret: () => {
        if (!disposed) emitter.emit('caret', caretOf(l));
      },
      onSelection: () => {
        if (!disposed) emitter.emit('selection', selectionOf(l));
      },
      onRender: (pages: number, ms: number) => {
        if (!disposed) emitter.emit('render', { pages, ms });
      },
      onPageError: (message: string) => reportError(message, false),
      // 前置回调：宿主不接就当"一律允许"（切换是常规交互，不该强制每个宿主都写钩子）。
      beforeControlChange: (change) =>
        typeof opts.onBeforeControlChange === 'function'
          ? opts.onBeforeControlChange(change)
          : true,
      onControlTap: (index, rect, readonly) => opts.onControlTap?.(index, rect, readonly),
      onContextMenu: (target, at) => opts.onContextMenu?.(target, at),
      onZoomBy: (delta: number) => queueZoom(delta),
      onZoomReset: () => void editor.setZoom(1),
    };
  }

  /**
   * 滚轮缩放的合并器：把一串增量累起来，每帧下发一次。
   *
   * 触控板捏合一次会推几十条带 ctrlKey 的 wheel 事件，每条都整篇重排的话画面会排队
   *   卡住；合并成一帧一档之后，捏合的手感是连续的，而排版的次数与帧数同阶。
   */
  let zoomPending = 0;
  let zoomFrame = 0;
  function queueZoom(delta: number): void {
    zoomPending += delta;
    if (zoomFrame) return;
    zoomFrame = requestAnimationFrame(() => {
      zoomFrame = 0;
      const d = zoomPending;
      zoomPending = 0;
      if (d === 0 || disposed) return;
      // 走公开面那一条（它自己夹取上下限），不直接碰引擎：缩放要同时改密度与配置影子。
      void editor.setZoom(editor.config().zoom + d);
    });
  }

  try {
    await starter();
  } catch (e) {
    // 启动失败必须同时做两件事：页面上看得见（横幅），调用方拿得到（拒绝）。
    //   只拒绝的话，那些不写 catch 的页面会停在空白容器上 —— 而那是这类失败最常见的形态。
    const err = e as Error & { heditorTitle?: string; heditorHint?: string };
    reportError(err?.heditorTitle ?? '编辑器启动失败', true, {
      detail: String(err?.message ?? e),
      hint: err?.heditorHint ?? '这一页的编辑器已经不可用；重新加载页面会重建引擎。',
    });
    throw e;
  }

  // 引擎自己的默认值不必等于库的默认值（撤销上限在引擎里是 0 = 不限，库给 100 条
  //   有界栈）。装载时把两者对齐一次，之后 configure 只推变化项。
  await Promise.resolve(then1(link.write('setUndoLimit', { n: config.undoLimit }), () => undefined));

  // 读写

  /**
   * 字符格式补丁 → 引擎的键名（与落盘同一套：b / i / u / fi / sz / col）。
   *
   * 转换放在这一层而不是让调用方写引擎键名：公开面用的是可读名，
   *   换个内部键名不该改到使用者的代码。
   * @param patch 公开面的补丁
   * @returns 引擎认识的载荷
   */
  function charPatchJson(patch: CharFormatPatch): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (patch.bold !== undefined) out.b = patch.bold;
    if (patch.italic !== undefined) out.i = patch.italic;
    if (patch.underline !== undefined) out.u = patch.underline;
    if (patch.font !== undefined) out.fi = fontIndexOf(patch.font);
    if (patch.size !== undefined) out.sz = patch.size;
    if (patch.color !== undefined) out.col = patch.color;
    return out;
  }

  /**
   * 段落格式补丁 → 引擎键名（fi / sz / lr / lf / lx / sb / sa / pbb）。
   * @param patch 公开面的补丁
   * @returns 引擎认识的载荷
   */
  function paraPatchJson(patch: ParaFormatPatch): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (patch.font !== undefined) out.fi = fontIndexOf(patch.font);
    if (patch.size !== undefined) out.sz = patch.size;
    if (patch.lineSpacing !== undefined) {
      out.lr = patch.lineSpacing.rule;
      if (patch.lineSpacing.factor !== undefined) out.lf = patch.lineSpacing.factor;
      if (patch.lineSpacing.exact !== undefined) out.lx = patch.lineSpacing.exact;
    }
    if (patch.spaceBefore !== undefined) out.sb = patch.spaceBefore;
    if (patch.spaceAfter !== undefined) out.sa = patch.spaceAfter;
    if (patch.pageBreakBefore !== undefined) out.pbb = patch.pageBreakBefore;
    return out;
  }

  /**
   * 字体引用 → 下标。数字原样，名字按已注册清单查：族名、PostScript 名、文件名、显示名。
   *
   * 显示名也认：界面上写的就是它，用户照着界面写 format({ font: '思源黑体' }) 时不该落空。
   * 名字要在主线程查完再下发：引擎只认下标，而"没注册的名字"若留给引擎去判，
   *   在 Worker 传输下会变成一次 15 秒的超时，报出来的是"命令超时"而不是"字体没注册"。
   * @param ref 下标或名字
   * @returns 字体下标
   */
  function fontIndexOf(ref: number | string): number {
    if (typeof ref === 'number') return Math.trunc(ref);
    const want = String(ref).trim();
    const list = (link.read('fontList') || []) as FontInfo[];
    const hit = list.find(
      (f) => f.family === want || f.postScript === want || f.file === want || f.label === want,
    );
    if (!hit) {
      const names = list.map((f) => f.label).join('、');
      throw new TypeError(`没有注册这个字体：${want}（已注册：${names || '无'}）`);
    }
    return hit.index;
  }

  /**
   * 把引擎给的格式读数读成对象。
   *
   * 两种形状都要收：读命令在协议表里已经把 JSON 解析成对象了（api.ts 那几条读都这么写），
   *   而同线程宿主给的是原样的 JSON 文本。只认其中一种的后果不是"少几个字段"，而是
   *   整条工具栏永远回显成"没有格式"—— 回显只是点亮按钮，它不会因为读不到而报错。
   * 坏数据当空对象：为一次坏包抛异常会把整条工具栏打死。
   * @param raw 对象或 JSON 文本
   * @returns 对象（解析失败时为空对象）
   */
  function parseJsonObject(raw: unknown): Record<string, any> {
    if (raw && typeof raw === 'object') return raw as Record<string, any>;
    if (typeof raw !== 'string' || !raw) return {};
    try {
      const v = JSON.parse(raw);
      return v && typeof v === 'object' ? v : {};
    } catch {
      return {};
    }
  }

  /**
   * 引擎的字符标记 → 公开面的补丁。
   * @param raw 引擎给的读数（对象或 JSON 文本）
   * @returns 只含出现过字段的补丁
   */
  function charPatchOf(raw: unknown): CharFormatPatch {
    const o = parseJsonObject(raw);
    const out: CharFormatPatch = {};
    if (typeof o.b === 'boolean') out.bold = o.b;
    if (typeof o.i === 'boolean') out.italic = o.i;
    if (typeof o.u === 'boolean') out.underline = o.u;
    if (typeof o.fi === 'number') out.font = o.fi;
    if (typeof o.sz === 'number') out.size = o.sz;
    if (Array.isArray(o.col) && o.col.length >= 3) {
      out.color = [Number(o.col[0]), Number(o.col[1]), Number(o.col[2])];
    }
    return out;
  }

  /**
   * 引擎的段落补丁 → 公开面的补丁。
   * @param raw 引擎给的读数（对象或 JSON 文本）
   * @returns 只含出现过字段的补丁
   */
  function paraPatchOf(raw: unknown): ParaFormatPatch {
    const o = parseJsonObject(raw);
    const out: ParaFormatPatch = {};
    if (typeof o.fi === 'number') out.font = o.fi;
    if (typeof o.sz === 'number') out.size = o.sz;
    if (typeof o.lr === 'string' || typeof o.lf === 'number' || typeof o.lx === 'number') {
      // 引擎只写出现过的那几个键，两边都不补缺省值：补 0 会让读回与写进去不是一件事
      //   （exact 20 会读成 factor 0）。
      const spacing: { rule: LineSpacingRule; factor?: number; exact?: number } = {
        rule: typeof o.lr === 'string' ? (o.lr as LineSpacingRule) : 'auto',
      };
      if (typeof o.lf === 'number') spacing.factor = o.lf;
      if (typeof o.lx === 'number') spacing.exact = o.lx;
      out.lineSpacing = spacing;
    }
    if (typeof o.sb === 'number') out.spaceBefore = o.sb;
    if (typeof o.sa === 'number') out.spaceAfter = o.sa;
    if (typeof o.pbb === 'boolean') out.pageBreakBefore = o.pbb;
    return out;
  }

  /**
   * 复合命令的排队：读—写必须一前一后。
   *
   * 两次点按之间不排队时，第二次读到的还是第一次写之前的值，于是两次算出的目标值一样，
   *   表现是"连点两下只动了一档"。串行只在复合命令之间需要：单条写命令在通道上本来就是
   *   先进先出。
   */
  let composedChain: Promise<unknown> = Promise.resolve();

  /**
   * 排一个复合命令到队尾。
   * @param task 要做的事
   * @returns 这一次的完成信号
   */
  function composed<T>(task: () => Promise<T>): Promise<T> {
    const next = composedChain.then(task, task);
    composedChain = next.catch(() => undefined);
    return next;
  }

  const edit = (name: string, args?: Args): Promise<boolean> => {
    alive();
    if (config.mode === 'readOnly') return Promise.resolve(false);
    return Promise.resolve(then1(link.write(name, args), (changed) => {
      if (!changed) return false;
      return then1(dom.afterEdit(name, 'api'), () => true);
    }) as boolean | Promise<boolean>);
  };

  /**
   * 光标 / 选区类命令的收尾：只上屏，不算内容变更。
   *
   * 不能走 edit()：它按引擎返回值判断"有没有改动"，而这批命令的实现返回 undefined
   *   （改的是光标，不是文档）。走 edit() 的后果是"引擎里光标动了、画面与账都没动" ——
   *   实测 setCaret 之后 caret().pos.offset 永远是 0，而打字却落在正确位置。
   *
   * reveal = "把光标滚进视野"。它只属于"我要看这个位置"的那几个入口（程序化定位、
   *   行首行尾、文档首尾）；选区类（select / selectAll / clearSelection）不做 ——
   *   选区可以在任何地方，把视口拽到焦点端是另一回事。
   */
  const caretOp = (name: string, args?: Args, reveal = false): Promise<void> => {
    alive();
    return Promise.resolve(
      then1(link.write(name, args), () =>
        then1(link.settle(name), (st) => {
          const stats = (st as { stats?: unknown } | null)?.stats ?? link.read('editStats');
          // 与宿主 afterEdit 同一口径：滚动要先把位置交给引擎（可能换窗），收尾再按
          //   新窗口建画布 —— 顺序颠倒就是"这一拍贴的是旧窗口那几页"。
          if (!reveal) {
            dom.finish(stats as never);
            return undefined;
          }
          return then1(dom.scrollCaretIntoView(stats as never), () => {
            dom.finish();
            return undefined;
          });
        }),
      ),
    ) as Promise<void>;
  };

  function caretOf(l: EngineLink): Caret {
    const st = l.read('editStats') ?? {};
    return {
      pos: { para: st.caretPara ?? 0, offset: st.caretOffset ?? 0 },
      page: st.caretPage ?? 0,
      rect: { x: st.caretX ?? 0, y: st.caretY ?? 0, w: st.caretW ?? 0, h: st.caretH ?? 0 },
      visible: !!st.caretVisible,
    };
  }

  function selectionOf(l: EngineLink): Selection {
    const raw = (l.read('selectionReport') ?? {}) as {
      anchor?: number;
      focus?: number;
      empty?: boolean;
      rects?: Array<{ page?: number; x?: number; y?: number; w?: number; h?: number }>;
    };
    const dpr = window.devicePixelRatio || 1;
    const rects = (raw.rects ?? []).map((r) => ({
      page: r.page ?? 0,
      x: (r.x ?? 0) / dpr,
      y: (r.y ?? 0) / dpr,
      w: (r.w ?? 0) / dpr,
      h: (r.h ?? 0) / dpr,
    }));
    const at = (flow: number | undefined): Pos => {
      if (flow === undefined) return { para: 0, offset: 0 };
      // 两端的位置随 selection 一起推过来（Worker 传输下是镜像，同步值）。
      const p = l.read('textPosOf', { flow }) as Pos | null;
      return p && typeof p.para === 'number' ? p : { para: 0, offset: 0 };
    };
    return { anchor: at(raw.anchor), focus: at(raw.focus), empty: !!raw.empty, rects, cells: null };
  }

  const editor: Editor = {
    // 内容

    async open(data, o) {
      alive();
      const text = String(data ?? '');
      const looksJson = text.trimStart().startsWith('{');
      if (o?.format === 'json' || (o?.format !== 'text' && looksJson)) {
        await edit('loadDocument', { text });
      } else {
        await edit('textDoc', { text });
      }
    },

    async setText(text, opts) {
      alive();
      // repeat 是契约里写着的（大文档压测要一次铺几千段），漏传的后果是"设置了没效果"：
      //   调用方拿到 1 段，而它按 300 段算的判据全落在错的分母上。
      const repeat = Math.max(1, Math.trunc(Number(opts?.repeat ?? 1)) || 1);
      await edit('textDoc', { text: String(text ?? ''), repeat });
    },

    text() {
      alive();
      return String(link.read('textAll') ?? '');
    },

    paragraph(i) {
      alive();
      return String(link.read('text', { i }) ?? '');
    },

    toggleFormat(field) {
      alive();
      return composed(async () => {
        const now = await editor.formatAt({ effective: true });
        return editor.format({ [field]: !now[field] });
      });
    },

    stepFontSize(dir, sizes) {
      alive();
      const ladder = sizes.filter((n) => Number.isFinite(n) && n > 0).sort((a, b) => a - b);
      if (ladder.length === 0) return Promise.resolve(false);
      return composed(async () => {
        const now = (await editor.formatAt({ effective: true })).size;
        const cur = typeof now === 'number' ? now : ladder[0];
        const next =
          dir > 0
            ? ladder.find((s) => s > cur)
            : [...ladder].reverse().find((s) => s < cur);
        if (next === undefined) return false;
        return editor.format({ size: next });
      });
    },

    counts() {
      alive();
      // 两个数都在每一拍推过来的 stats 里：读它不额外往返，也不必把全文搬到主线程。
      const st = (link.read('editStats') ?? {}) as { docCodepoints?: number; wordCount?: number };
      return { chars: Number(st.docCodepoints ?? 0), words: Number(st.wordCount ?? 0) };
    },

    paragraphs() {
      alive();
      return String(link.read('textAll') ?? '').split('\n');
    },

    async save() {
      alive();
      return String((await Promise.resolve(link.serialize())) ?? '');
    },

    // 字体

    async addFont(...sources) {
      alive();
      // 参数摊平：addFont(a, b) 与 addFont([a, b]) 都要能用（两者混着写也认）。
      const flat = ([] as FontInput[]).concat(...sources);
      if (!flat.length) return [];
      const entries = await resolveFonts(String(assets.base), flat);
      await Promise.resolve(link.write('addFont', { list: entries }));
      await Promise.resolve(link.settle('addFont'));
      // 回哪几份由"这一次交出去的地址或文件名"对回来：引擎只认自己装了什么，
      //   而清单里那一条正是它。重复注册同一份时对回来的是既有那一条 —— 那正是想要的。
      const list = editor.fonts();
      const out: FontInfo[] = [];
      for (const e of entries) {
        const key = String(e.name || e.url || '').split('/').pop()!.split('?')[0];
        const hit = list.find((f) => f.file === key);
        if (!hit) throw new Error('字体装载之后在清单里找不到：' + key);
        out.push(hit);
      }
      return out;
    },

    fonts() {
      alive();
      const raw = link.read('fontList');
      return (Array.isArray(raw) ? raw : []) as FontInfo[];
    },

    async insertText(pos, text) {
      return edit('insertText', { para: pos.para, offset: pos.offset, s: String(text) });
    },

    async insertTable(opts) {
      alive();
      // 行列原样传：0 与负数由引擎拒掉（返回 false、文档不动）。在这里改成 1 会让
      //   "参数写错了"看起来像"插进去了一张一格的表" —— 静默纠正比失败更难查。
      const rows = Math.trunc(Number(opts?.rows ?? 0)) || 0;
      const cols = Math.trunc(Number(opts?.cols ?? 0)) || 0;
      return edit('insertTable', { rows, cols, header: !!opts?.header });
    },

    async tableOp(op) {
      alive();
      // 动词表在这里折成整数：编号是引擎那侧的枚举顺序，宿主只认名字。
      const codes: Record<TableOp, number> = {
        insertRowAbove: 0,
        insertRowBelow: 1,
        removeRow: 2,
        insertColumnLeft: 3,
        insertColumnRight: 4,
        removeColumn: 5,
        mergeCells: 6,
        splitCell: 7,
        removeTable: 8,
      };
      const code = codes[op];
      if (code === undefined) throw new Error('不认识的表格命令：' + String(op));
      return edit('tableOp', { op: code });
    },

    async setTableCell(opts) {
      alive();
      const o: TableCellOptions = opts ?? {};
      const patch: Args = {};
      if (o.vAlign !== undefined) {
        const codes: Record<string, number> = { top: 0, middle: 1, bottom: 2 };
        const code = codes[o.vAlign];
        if (code === undefined) throw new Error('不认识的垂直对齐：' + String(o.vAlign));
        patch.vAlign = code;
      }
      return edit('setCellSpec', { opts: patch });
    },

    async setTableRow(opts) {
      alive();
      const o: TableRowOptions = opts ?? {};
      const patch: Args = {};
      if (o.header !== undefined) patch.header = !!o.header;
      if (o.split !== undefined) patch.split = !!o.split;
      if (o.breakBefore !== undefined) patch.breakBefore = !!o.breakBefore;
      if (o.heightPt !== undefined) patch.heightPt = Number(o.heightPt);
      if (o.heightRule !== undefined) {
        const codes: Record<string, number> = { auto: 0, atLeast: 1, exact: 2 };
        const code = codes[o.heightRule];
        if (code === undefined) throw new Error('不认识的行高规则：' + String(o.heightRule));
        patch.heightRule = code;
      }
      return edit('setRowSpec', { opts: patch });
    },

    async setTable(opts) {
      alive();
      const o: TableOptions = opts ?? {};
      const patch: Args = {};
      if (o.widthPt !== undefined) patch.widthPt = Number(o.widthPt);
      if (o.align !== undefined) {
        const codes: Record<string, number> = { start: 0, center: 1, end: 2 };
        const code = codes[o.align];
        if (code === undefined) throw new Error('不认识的水平位置：' + String(o.align));
        patch.align = code;
      }
      return edit('setTableSpec', { opts: patch });
    },

    async tableState() {
      alive();
      // 现状是读：走查询而不是命令 —— 属性面板打开时读一次，不该因此重画一次画面。
      return (await Promise.resolve(link.read('tableState'))) as TableState;
    },

    async tableGrid(xPt, yPt) {
      alive();
      // 几何是读：拖拽判定每一下都问一次，不能因此重画。
      return (await Promise.resolve(
        link.read('tableGrid', { x: Number(xPt), y: Number(yPt) }),
      )) as TableGrid;
    },

    async setTableColWidths(grid, widths) {
      alive();
      const cols = Array.isArray(widths) ? widths.map((w) => Number(w)) : [];
      // 列数对不上就不发：引擎照给定列宽排，多一条少一条都会把表排歪，
      //   而"少一条"在画面上只表现为最后一列被拉满，看不出是宿主算错了。
      if (cols.length !== Number(grid.cols.length) - 1) {
        throw new Error('列宽数量与栅格列数不符：' + cols.length + ' / ' + (grid.cols.length - 1));
      }
      return edit('setTableColWidths', { handle: Number(grid.table), widths: cols });
    },

    async setWatermark(opts) {
      alive();
      const o: WatermarkOptions = opts ?? {};
      // 只给要改的项：没给的字段沿用现状，命令表那一侧收的是完整记录。
      const cur = await editor.watermark();
      return edit('setWatermark', {
        enabled: o.enabled === undefined ? cur.enabled : !!o.enabled,
        text: o.text === undefined ? cur.text : String(o.text),
        sizePt: o.sizePt === undefined ? cur.sizePt : Number(o.sizePt),
        color: Array.isArray(o.color) ? o.color : cur.color,
        angle: o.angle === undefined ? cur.angle : Number(o.angle),
      });
    },

    async watermark() {
      alive();
      return (await Promise.resolve(link.read('watermark'))) as WatermarkState;
    },

    async setImageLayout(index, layout) {
      alive();
      const codes: Record<string, number> = {
        inline: 0,
        square: 1,
        topAndBottom: 2,
        behind: 3,
        inFront: 4,
      };
      const code = codes[layout];
      if (code === undefined) throw new Error('不认识的图片版式：' + String(layout));
      return edit('setImageLayout', { i: index, layout: code });
    },

    describeTable(state, opts) {
      return tableFormOf(state, opts);
    },

    async applyTableForm(values) {
      alive();
      const st = await editor.tableState();
      if (!st.inTable) return false;
      const w = tableWriteOf(st, values);
      let changed = false;
      if (Object.keys(w.table).length > 0) changed = (await editor.setTable(w.table)) || changed;
      if (Object.keys(w.row).length > 0) changed = (await editor.setTableRow(w.row)) || changed;
      if (Object.keys(w.cell).length > 0) changed = (await editor.setTableCell(w.cell)) || changed;
      return changed;
    },

    async insertControl(opts) {
      alive();
      const o = opts ?? {};
      return edit('insertControl', {
        opts: {
          ...controlOptsFrom(o),
          ro: !!o.readonly,
          req: !!o.required,
          // 不给 shape 就按类型推断：强制成 text 会把单选组的枚举值形态冲掉。
          shape: o.shape === undefined ? undefined : SHAPE_CODE[o.shape],
        },
      });
    },

    async removeControl(index, opts) {
      alive();
      return edit('removeControl', { i: Math.trunc(index), keepContent: !!opts?.keepContent });
    },

    async updateControl(index, opts) {
      alive();
      const o = opts ?? {};
      let changed = false;
      // 只改给出的项：与插入共用同一份键映射，两边不会各认一半字段。
      const patch = controlOptsFrom(o);
      if (Object.keys(patch).length > 0) {
        changed = (await edit('setControlSpec', { i: Math.trunc(index), opts: patch })) || changed;
      }
      // 只读与必填不在定义里，是运行态属性，走另一个入口。分开提交是为了让"只改标签"
      //   仍然只有一步撤销。
      if (o.readonly !== undefined) {
        const done = await edit('setControlProp', {
          i: Math.trunc(index),
          prop: 1,
          value: !!o.readonly,
        });
        changed = done || changed;
      }
      if (o.required !== undefined) {
        const done = await edit('setControlProp', {
          i: Math.trunc(index),
          prop: 2,
          value: !!o.required,
        });
        changed = done || changed;
      }
      return changed;
    },

    async setControlValue(index, value) {
      alive();
      const v: ControlValueInput = value ?? {};
      return edit('setControlValue', {
        i: Math.trunc(index),
        flag: !!v.flag,
        text: v.text === undefined ? '' : String(v.text),
        items: v.items ?? [],
      });
    },

    async toggleControl(index, option) {
      alive();
      return edit('toggleControl', {
        i: Math.trunc(index),
        option: option === undefined ? -1 : Math.trunc(option),
      });
    },

    async insertImage(opts) {
      alive();
      const o: ImageInsertOptions = opts ?? ({} as ImageInsertOptions);
      // 像素尺寸与显示尺寸一起交给引擎：前者折算 Auto，后者钉死。两者都不给时它是
      //   一个 4:3 的占位框 —— 引擎不猜图片多大。
      return edit('insertImage', {
        opts: {
          res: o.resourceId === undefined ? '' : String(o.resourceId),
          pw: o.pxWidth === undefined ? undefined : Math.trunc(o.pxWidth),
          ph: o.pxHeight === undefined ? undefined : Math.trunc(o.pxHeight),
          dpi: o.dpi === undefined ? undefined : Math.trunc(o.dpi),
          w: o.width,
          h: o.height,
        },
      });
    },

    async registerImage(opts) {
      alive();
      const data = opts?.data;
      if (!(data instanceof Uint8Array)) throw new TypeError('registerImage 需要 Uint8Array');
      await Promise.resolve(link.write('registerImage', {
        id: String(opts?.id ?? ''),
        w: Math.trunc(Number(opts?.width ?? 0)),
        h: Math.trunc(Number(opts?.height ?? 0)),
        data,
      }));
      await Promise.resolve(link.settle('registerImage'));
      dom.finish();
    },

    async dropImage(id) {
      alive();
      await Promise.resolve(link.write('dropImage', { id: String(id ?? '') }));
      await Promise.resolve(link.settle('dropImage'));
      dom.finish();
    },

    async images() {
      alive();
      // 读要经一次往返（Worker 传输），所以是异步的 —— 与 fonts() 不同：那一份是随状态
      //   推过来的副本，这里是现场查询。
      const raw = (await Promise.resolve(link.read('imageList'))) as { images?: unknown[] } | null;
      const list = raw && Array.isArray(raw.images) ? (raw.images as any[]) : [];
      // 引擎给的版式是 0..4 的档位（三个字段的投影），对外只认名字：
      //   折在这里而不是让调用方去比数字，两种传输、两个宿主就只有一个口径。
      const names: ImageLayout[] = ['inline', 'square', 'topAndBottom', 'behind', 'inFront'];
      return list.map((b) => {
        const code = Number(b.layout);
        return { ...b, layout: names[code] ?? names[0] } as ImageBox;
      });
    },

    async imageHit(x, y) {
      alive();
      const raw = (await Promise.resolve(link.read('imageHit', { x, y }))) as ImageHit | null;
      return raw && typeof raw.index === 'number' ? raw : { index: -1, handle: -1 };
    },

    async selectImage(index) {
      alive();
      // 选中一张图片改的是"哪套选区"，文档一个字都没变 —— 走 edit() 会被当成一次
      //   内容变更（change 事件里会带一份没变的文本）。所以只写命令再收尾。
      await Promise.resolve(link.write('selectImage', { i: Math.trunc(index) }));
      await Promise.resolve(link.settle('selectImage'));
      dom.finish();
    },

    async resizeImage(index, widthPt, heightPt, opts) {
      alive();
      // merge 是"与上一帧合成一步撤销"：拖拽的后续每一帧都带它，一次拖拽只留一步。
      return edit('resizeImage', {
        i: Math.trunc(index),
        w: Number(widthPt),
        h: Number(heightPt),
        merge: !!opts?.merge,
      });
    },

    // 编辑

    async type(text) {
      return edit('type', { s: String(text) });
    },

    async deleteBackward(n = 1) {
      let changed = false;
      for (let i = 0; i < Math.max(1, n); ++i) {
        if (!(await edit('backspace'))) break;
        changed = true;
      }
      return changed;
    },

    async deleteForward(n = 1) {
      let changed = false;
      for (let i = 0; i < Math.max(1, n); ++i) {
        if (!(await edit('deleteForward'))) break;
        changed = true;
      }
      return changed;
    },

    async splitParagraph() {
      return edit('splitParagraph');
    },

    async mergeParagraph(dir = 'prev') {
      return edit(dir === 'next' ? 'mergeNext' : 'mergePrev');
    },

    async undo() {
      return edit('undo');
    },

    async redo() {
      return edit('redo');
    },

    /**
     * 复制选区到系统剪贴板。选区为空时返回 false。
     *
     * 走的是与 Ctrl+C 同一条路：引擎产出的片段原样进剪贴板，所以跨实例、跨标签页粘回来
     * 仍是同一段内容（样式、控件、表格都在片段里）。
     */
    async copy() {
      alive();
      return dom.copy();
    },

    /**
     * 剪切：先复制，成功之后再删 —— 复制失败时内容还在，用户不会白丢一段。
     */
    async cut() {
      alive();
      return dom.cut();
    },

    /**
     * 粘贴：自家格式优先，其次纯文本（按行拆段）。
     *
     * 读系统剪贴板需要浏览器授权（键盘那条不需要）；被拒时返回 false 并在状态栏说明。
     */
    async paste() {
      alive();
      return dom.paste();
    },

    async format(patch) {
      return edit('charFormat', { s: JSON.stringify(charPatchJson(patch)) });
    },

    async paragraphFormat(patch) {
      return edit('paraFormat', { s: JSON.stringify(paraPatchJson(patch)) });
    },

    async formatAt(opts) {
      const raw = await Promise.resolve(
        link.read('charFormatAt', opts?.effective ? { effective: 1 } : undefined),
      );
      return charPatchOf(raw);
    },

    async paragraphFormatAt() {
      const raw = await Promise.resolve(link.read('paraFormatAt'));
      return paraPatchOf(raw);
    },

    async setIndent(levels) {
      const n = Math.abs(Math.trunc(levels));
      if (n === 0) return false;
      // 先收起到选区左端：引擎那两条命令（Tab / Shift+Tab）改的是"把选区换成缩进"，
      //   而这里是"给这一段加缩进"。带着选区过去等于把用户选中的字删掉；收到右端则
      //   会把空格插进句子中间。两种都不对，所以收到左端再插。
      const sel = selectionOf(link);
      if (!sel.empty) {
        const a = sel.anchor;
        const f = sel.focus;
        const left = a.para < f.para || (a.para === f.para && a.offset <= f.offset) ? a : f;
        await caretOp('setCaretAt', { para: left.para, offset: left.offset, plain: 1 }, true);
      }
      let changed = false;
      for (let i = 0; i < n; ++i) {
        if (!(await edit(levels > 0 ? 'indent' : 'outdent'))) break;
        changed = true;
      }
      return changed;
    },

    // 光标与选区

    caret() {
      alive();
      return caretOf(link);
    },

    async setCaret(pos, o?: SetCaretOptions) {
      alive();
      // 地址入口一条就够：给了侧别走"槽序号 + 行别"那支（夹屏障，与点击同档）；
      //   没给就是 plain —— 只挪位置，属性面板要能把光标停进只读字段。
      const args: Args = { para: pos.para, offset: pos.offset };
      if (o?.side) {
        args.side = o.side === 'after' ? 1 : 0;
        args.line = o.line === 'last' ? 1 : 0;
      } else {
        args.plain = 1;
      }
      await caretOp('setCaretAt', args, true);
    },

    async moveCaret(step: CaretStep) {
      alive();
      switch (step) {
        case 'left':
          await edit('move', { dcp: -1 });
          break;
        case 'right':
          await edit('move', { dcp: 1 });
          break;
        case 'up':
          await edit('vertical', { dir: -1 });
          break;
        case 'down':
          await edit('vertical', { dir: 1 });
          break;
        case 'docStart':
          await caretOp('docStart', undefined, true);
          break;
        case 'docEnd':
          await caretOp('docEnd', undefined, true);
          break;
        case 'lineStart':
          await caretOp('lineStart', undefined, true);
          break;
        case 'lineEnd':
          await caretOp('lineEnd', undefined, true);
          break;
      }
    },

    async select(anchor, focus) {
      // 只读只是"不许改文档"，光标与选区照样能动 —— 所以这里不看 readOnly。
      // 段内位置 -> 流位置的换算在引擎里（selectPos）；宿主不碰这个口径。
      await caretOp('selectPos', {
        anchorPara: anchor.para,
        anchorOffset: anchor.offset,
        focusPara: focus.para,
        focusOffset: focus.offset,
      });
    },

    async selectAll() {
      await caretOp('selectAll');
    },

    async clearSelection() {
      await caretOp('clearSelection');
    },

    focus() {
      // 与诊断面那条同一个实现：宿主用完工具栏、菜单之后要把键盘还回来。
      dom.focus();
    },

    selection() {
      alive();
      return selectionOf(link);
    },

    // 控件

    async focusControl(dir = 'next') {
      await edit('focusControl', { forward: dir === 'next' });
    },

    async setControlProp(index, prop, value) {
      return edit('setControlProp', { i: index, prop, value });
    },

    form(): ControlSummary[] {
      alive();
      const raw = (link.read('controls') ?? {}) as {
        items?: Array<{
          i?: number;
          kind?: string;
          type?: string;
          key?: string;
          label?: string;
          group?: string;
          text?: string;
          ro?: boolean;
          req?: boolean;
          free?: boolean;
          flag?: boolean;
          sel?: string;
          fmt?: string;
          ph?: string;
          du?: number;
          glyphColor?: RGBA;
          selItems?: string[];
          opts?: Array<[string, string]>;
          start?: [number, number];
          end?: [number, number];
        }>;
        caret?: number;
      };
      const items = raw.items ?? [];
      const focused = raw.caret ?? -1;
      return items.map((it, index) => ({
        index,
        kind: String(it.kind ?? ''),
        type: String(it.type ?? ''),
        key: String(it.key ?? ''),
        label: String(it.label ?? ''),
        readonly: !!it.ro,
        required: !!it.req,
        freeInput: !!it.free,
        group: String(it.group ?? ''),
        text: String(it.text ?? ''),
        placeholder: String(it.ph ?? ''),
        value: {
          flag: !!it.flag,
          text: String(it.sel ?? ''),
          items: Array.isArray(it.selItems) ? it.selItems.map(String) : [],
        },
        options: Array.isArray(it.opts)
          ? it.opts.map((pair) => ({ value: String(pair[0]), label: String(pair[1]) }))
          : [],
        // 区间照原样给出来：宿主画浮层、判"光标在不在这个字段里"都要它。
        start: { para: Number(it.start?.[0] ?? 0), offset: Number(it.start?.[1] ?? 0) },
        end: { para: Number(it.end?.[0] ?? 0), offset: Number(it.end?.[1] ?? 0) },
        format: String(it.fmt ?? ''),
        dateUnits: dateUnitsOf(Number(it.du ?? 0)),
        glyphColor: (Array.isArray(it.glyphColor) && it.glyphColor.length === 4
          ? it.glyphColor.map(Number)
          : [0, 0, 0, 0]) as RGBA,
        focused: index === focused,
      }));
    },

    describeControl(control, opts) {
      return controlFormOf(control, opts);
    },

    async applyControlForm(index, values) {
      alive();
      const c = editor.form()[index];
      if (!c) return false;
      const w = controlWriteOf(c, values);
      let changed = false;
      if (Object.keys(w.update).length > 0) {
        changed = (await editor.updateControl(index, w.update)) || changed;
      }
      // 文本类的值就是内容：它不进定义，改它也不重建显示（内容本身就是显示）。
      if (w.value !== null) {
        changed = (await editor.setControlValue(index, { text: w.value })) || changed;
      }
      return changed;
    },

    // 视图

    geometry(): Geometry {
      alive();
      const g = link.geom();
      const dpr = g.dpr || 1;
      const pages = Math.max(1, Math.round(g.totalPages));
      return {
        page: { w: g.pw / dpr, h: g.ph / dpr },
        gap: g.gap / dpr,
        pages,
        // 与页容器实际设的那个高度同一条公式：连页下它跟着内容走，不是整数页。
        scrollHeight: Math.max(1, scrollHeightPx(pages, g) / dpr),
        window: { first: g.first, count: g.count },
        dpr,
      };
    },

    async scrollTo(target) {
      alive();
      const g = this.geometry();
      const y = target.y ?? (target.page ?? 0) * (g.page.h + g.gap);
      const scroller =
        stageOf(opts.container) ?? opts.container;
      scroller.scrollTop = y;
      await Promise.resolve(then1(link.view(y * g.dpr), () => dom.finish()));
    },

    async setZoom(zoom) {
      await editor.configure({ zoom });
    },

    // 配置

    async configure(patch) {
      alive();
      const next: EditorConfig = { ...config };
      for (const key of Object.keys(patch) as Array<keyof EditorConfig>) {
        if (!(key in DEFAULT_CONFIG)) throw new TypeError(`未知配置项 ${String(key)}`);
      }
      // 夹取：缩放的范围与引擎一致（0.375 ~ 2.25 倍）。
      if (patch.zoom !== undefined) {
        const z = Number(patch.zoom);
        if (!Number.isFinite(z)) throw new TypeError('zoom 必须是数字');
        next.zoom = Math.min(2.25, Math.max(0.375, z));
      }
      if (patch.pageMode !== undefined) {
        if (patch.pageMode !== 'paged' && patch.pageMode !== 'continuous') {
          throw new TypeError("pageMode 只能是 'paged' 或 'continuous'");
        }
        next.pageMode = patch.pageMode;
      }
      if (patch.page !== undefined) next.page = normalizePageSetup(patch.page, config.page);
      if (patch.mode !== undefined) {
        if (patch.mode !== 'document' && patch.mode !== 'form' && patch.mode !== 'readOnly') {
          throw new TypeError("mode 只能是 'document' / 'form' / 'readOnly'");
        }
        next.mode = patch.mode;
      }
      if (patch.input !== undefined) next.input = !!patch.input;
      if (patch.showBrackets !== undefined) next.showBrackets = !!patch.showBrackets;
      if (patch.showControlDots !== undefined) next.showControlDots = !!patch.showControlDots;
      if (patch.marginMarks !== undefined) next.marginMarks = !!patch.marginMarks;
      if (patch.showMarks !== undefined) {
        const m = patch.showMarks;
        if (m !== 'none' && m !== 'paragraph' && m !== 'all') {
          throw new TypeError("showMarks 只能是 'none' / 'paragraph' / 'all'");
        }
        next.showMarks = m;
      }
      if (patch.undoLimit !== undefined) next.undoLimit = Math.max(0, Math.trunc(patch.undoLimit));
      if (patch.caretStyle !== undefined) next.caretStyle = patch.caretStyle;
      if (patch.selectionStyle !== undefined) next.selectionStyle = patch.selectionStyle;

      const changed: Partial<EditorConfig> = {};
      for (const key of Object.keys(next) as Array<keyof EditorConfig>) {
        if (JSON.stringify(next[key]) !== JSON.stringify(config[key])) {
          (changed as Record<string, unknown>)[key] = next[key];
        }
      }
      // mode 与 undoLimit 无代价且幂等：只要调用方提到，就真的推一次。JS 侧那份配置
      //   是影子，而诊断面（装载样例、setControlProp 之类）会绕过它直接改引擎 ——
      //   影子落后于引擎时，"设成文档模式"如果按差集算就是空操作，表现是
      //   "一个字都打不进去"。这两条便宜到不必省。
      if (patch.mode !== undefined && changed.mode === undefined) changed.mode = next.mode;
      if (patch.undoLimit !== undefined && changed.undoLimit === undefined) {
        changed.undoLimit = next.undoLimit;
      }
      if (Object.keys(changed).length === 0) return { ...config };

      // 一次 configure 只做最贵的那一档：先重排类，再重绘类，最后无代价类。
      //   顺序不能反：重排会把页位图整体作废，之后的重绘才有意义。
      const steps: Array<() => Promise<unknown> | unknown> = [];

      if (changed.zoom !== undefined) {
        const dpr = window.devicePixelRatio || 1;
        const density = next.zoom * (4 / 3) * dpr;
        // 先记下旧密度：dom.zoom.set 之后就读不到它了，而折算滚动位置要用这个比值。
        const prevDensity = dom.zoom.density;
        dom.zoom.set(density);
        steps.push(() =>
          then1(settleAfter('zoom', { zoom: density, dpr }), () =>
            dom.keepScrollAcrossZoom(prevDensity, density),
          ),
        );
      }
      if (changed.page !== undefined) {
        const dims = paperSizePt(next.page.paper, next.page.landscape, next.page.custom);
        const pm = next.page.margins;
        steps.push(() =>
          settleAfter('pageSetup', {
            w: dims.w,
            h: dims.h,
            top: pm.top,
            right: pm.right,
            bottom: pm.bottom,
            left: pm.left,
          }),
        );
      }
      if (changed.pageMode !== undefined) {
        const continuous = next.pageMode === 'continuous';
        steps.push(() =>
          Promise.resolve(
            then1(link.setViewMode(continuous ? 0 : kPageGapPt, continuous), () =>
              settleAfter('viewMode'),
            ),
          ),
        );
      }
      if (changed.showBrackets !== undefined) {
        steps.push(() => settleAfter('setShowBrackets', { on: next.showBrackets }));
      }
      if (changed.showControlDots !== undefined) {
        steps.push(() => settleAfter('setShowControlDots', { on: next.showControlDots }));
      }
      if (changed.showMarks !== undefined) {
        steps.push(() => settleAfter('setShowMarks', { mode: MARKS_CODE[next.showMarks] }));
      }
      if (changed.marginMarks !== undefined) {
        steps.push(() => settleAfter('setShowMarginMarks', { on: next.marginMarks }));
      }
      if (changed.undoLimit !== undefined) {
        steps.push(() => settleAfter('setUndoLimit', { n: next.undoLimit }));
      }
      if (changed.caretStyle !== undefined) {
        steps.push(() => settleAfter('caretStyle', { style: next.caretStyle }));
      }
      if (changed.selectionStyle !== undefined) {
        steps.push(() => settleAfter('selectionStyle', { style: next.selectionStyle }));
      }
      if (changed.mode !== undefined) {
        // 三档都要落到引擎：只读要引擎的 ReadOnly 才保证"光标可动、一个字都不许改"，
        //   表单要引擎拿可编辑性判定归属。宿主侧那个 edit() 只保护库自己的方法，
        //   键盘走输入层，只改 config 拦不住它。
        steps.push(() => settleAfter('setEditMode', { mode: MODE_CODE[next.mode] }));
      }

      for (const run of steps) await Promise.resolve(run());

      Object.assign(config, next);
      // 样式由引擎夹取（光标宽度下限、颜色通道），读回来才是"生效值" ——
      //   契约写的是返回夹取后的配置，在 JS 里再抄一份夹取规则就会与引擎分叉。
      //   读是异步的传输下必须先等到值：拿到一个 Promise 写进配置影子，之后每一次
      //   config() 都把它当对象读（widthPt 是 undefined），而那正是"夹取没生效"的样子。
      if (changed.caretStyle !== undefined) {
        config.caretStyle = (await Promise.resolve(
          link.read('caretStyleJson'),
        )) as typeof config.caretStyle;
      }
      if (changed.selectionStyle !== undefined) {
        config.selectionStyle = (await Promise.resolve(
          link.read('selectionStyleJson'),
        )) as typeof config.selectionStyle;
      }
      emitter.emit('configchange', { changed, config: { ...config } });
      return { ...config };
    },

    config() {
      return Object.freeze({ ...config });
    },

    describePageSetup(opts) {
      return pageSetupFormOf(config.page, opts);
    },

    async applyPageSetupForm(values) {
      alive();
      const next = pageSetupWriteOf(config.page, values);
      // 整篇重排是这一层最贵的代价：面板没改就不该付它。
      if (!next) return false;
      await editor.configure({ page: next });
      return true;
    },

    // 事件与生命周期

    on(event, cb) {
      alive();
      return emitter.on(event, cb);
    },

    async flush() {
      alive();
      await link.flush();
    },

    async destroy() {
      if (disposed) return;
      // 先抹错误面：destroy 之后容器会被清空，横幅留着就成了"页面上有一块说编辑器坏了、
      //   可编辑器已经不在"的残迹。
      surface.clear();
      disposed = true;
      destroyer();
      dom.destroy();
    },

    get element() {
      return opts.container;
    },

    get diag() {
      if (!opts.debug) {
        throw new TypeError('diag 只在 createEditor({ debug: true }) 时可用');
      }
      return makeDiag(link, dom, assets, pageErrors, () => syncConfigFromEngine(link, config, dom.zoom));
    },
  };

  /**
   * 一次配置类命令的收尾：写完就 present + 贴页（与 doOp 的口径同一条）。
   */
  function settleAfter(opName: string, args?: Args): Promise<unknown> | unknown {
    // 写必须等它落地再收尾。Worker 传输下 write 是一次往返，而 settle 在那一侧是同步的
    //   （收尾已经在 doOp 里做完了）—— 不等 write 就等于"配置还没生效就去读生效值"，
    //   症状是每次读回的都是上一次的样式，而调用方以为只是"夹取规则不对"。
    return then1(args ? link.write(opName, args) : null, () =>
      then1(link.settle(opName), (st) => {
        // settle 回的是"收尾结果"（{ moved, scrolled, stats }），不是 stats 本身。
        //   两种传输必须同形 —— 少了这一层解包，收尾拿到的是包装对象，光标位置与页号
        //   全是 undefined（IME 被收成一像素），而 editStats 看上去仍然是对的。
        dom.finish((st as { stats?: unknown } | null)?.stats ?? link.read('editStats'));
        return st;
      }),
    );
  }

  /**
   * 变更事件。json 是方法而不是属性：2000 段序列化一次约 55 ms（Worker 模式下还是一次
   *   往返），所以"读它才算"必须是显式的 —— 属性写法会让 JSON.stringify(ev) 顺带把它
   *   算一遍，而那正好是最糟的一拍。同一个版本只算一次（闭包缓存）。
   */
  function makeChange(source: ChangeSource, v: number): ChangeEvent {
    let cached: string | undefined;
    return {
      source,
      version: v,
      get text() {
        return String(link.read('textAll') ?? '');
      },
      async json() {
        if (cached === undefined) cached = String((await Promise.resolve(link.serialize())) ?? '');
        return cached;
      },
    };
  }

  if (opts.config) {
    // 影子先退回默认值再配一次：configure 按"影子与目标值的差集"决定下发，
    //   而影子在构造时已经并进了调用方给的那一份 —— 不退回的话差集恒为空，
    //   初始配置会被静默吞掉（只有 mode 与 undoLimit 那两条强制下发，所以只靠它们是漏的）。
    Object.assign(config, DEFAULT_CONFIG);
    await editor.configure(opts.config);
  }
  return guard(editor, !!opts.debug) as Editor;
}

// 诊断面

/**
 * 把引擎那一侧的模式与缩放读回库的配置影子。
 *
 * 诊断面能绕过 configure 直接改引擎（装载表单样例就自带表单模式），而对外读的
 *   config() 读的是影子。不同步的后果不是"显示错了"，是下一次 configure 按过期的
 *   差集算成空操作 —— 表现是"设了文档模式还是一个键都打不进去"。
 */
function syncConfigFromEngine(
  link: EngineLink,
  cfg: EditorConfig,
  zoom: ZoomControl,
): Promise<void> | void {
  // 两条读都是"引擎现在是什么"，在 Worker 传输下是往返 —— 必须等到值再写影子，
  //   否则 Number(Promise) 是 NaN，配置影子被写成"文档模式 + 原始缩放"，
  //   而它下一拍又会被当成差集的基准。
  return then1(link.read('editMode'), (code) =>
    then1(link.read('stats'), (st) => {
      const c = Number(code);
      cfg.mode = c === 1 ? 'form' : c === 2 ? 'readOnly' : 'document';
      const density = Number((st as { zoom?: number } | null)?.zoom ?? 0);
      if (density > 0) {
        cfg.zoom = density / ((4 / 3) * (window.devicePixelRatio || 1));
        // 命中把设备像素折成磅时除的就是这个密度。它只跟着 configure 走过，而诊断面
        //   能直接改引擎的 zoom —— 不同步的症状不是"显示大小不对"，是点击落点整体偏：
        //   除错了倍数，点同一个像素会落到右边或左边的一格上。
        zoom.set(density);
      }
    }),
  );
}

/**
 * diag 是"debug 开关下的公开面"：成员不承诺稳定，所以参数与返回暂时是 unknown。
 * 这里只做转发，不加工 —— 加工会把"引擎报了什么"变成对它的猜测。
 *
 * 三类例外都不是加工，而是补上收尾：装载类要等重排重绘落地，返回值是那一拍的
 *   editStats（探针拿它当分母）；定位类要 present，否则画面停在旧位置；闪烁与相位
 *   只动状态、绝不 present —— 呈现会把上一次全量呈现的账冲成"只画了个光标"。
 */
function makeDiag(
  link: EngineLink,
  dom: DomHost,
  assets: AssetPaths,
  errors: string[],
  syncConfig: () => void,
): Diag {
  const read = (name: string, args?: Args): unknown => link.read(name, args);

  /**
   * 读一个标量并把形状钉住（数字 / 字符串 / 数组）。
   *
   * Worker 传输下 read 是往返，直接 Number(...) / String(...) 得到的是对 Promise 的
   *   NaN 与 "[object Promise]" —— 而且它们能一路流进判据里，看起来只是"这个字段
   *   没有值"。then1 把同步值与 Promise 折成一条链，两种传输共用一个写法。
   */
  const num = (name: string, args?: Args, fallback = 0): number | Promise<number> =>
    then1(read(name, args), (v) => (v === undefined || v === null ? fallback : Number(v)));
  const str = (name: string, args?: Args): string | Promise<string> =>
    then1(read(name, args), (v) => String(v ?? ''));

  /**
   * 装载类收尾：写完等一次收尾，返回那一拍的账。
   */
  const settleAs = async (name: string, args: Args): Promise<unknown> => {
    await Promise.resolve(link.write(name, args));
    await Promise.resolve(link.settle(name));
    // 装载与设置类命令会直接改引擎的编辑模式与缩放（样例自带表单模式、场景带缩放），
    //   而库那份配置是影子 —— 不同步回来的话，下一次 configure 会按过期的差集算，
    //   于是"切成文档模式"变成空操作。
    await Promise.resolve(syncConfig());
    return read('editStats');
  };

  return {
    stats: () => read('stats'),
    // 选区两端在引擎里是全局流位置，对外是 {段号, 段内偏移}；诊断要的是前者。
    selectionRaw: () => read('selectionReport'),
    dragState: () => (dom.drag ? dom.drag.state() : null),
    dragSettled: () => (dom.drag ? dom.drag.settled() : Promise.resolve()),
    editStats: () => read('editStats'),
    edit: () => read('editStats'),
    auditFrame: () => read('auditFrame'),
    auditBase: () => read('auditBase'),
    auditPage: () => read('auditPage'),
    auditLayout: () => read('auditLayout'),
    auditCompile: () => read('auditCompile'),
    auditGlyphPages: () => read('auditGlyphPages'),
    selfCheck: () => read('selfCheck'),
    markCheck: () => read('markCheck'),
    rawFrame: () => read('rawFrame'),
    frameDigest: () => num('frameDigest'),
    frameW: () => num('frameWidth'),
    frameH: () => num('frameHeight'),
    present: () => void dom.repaint(),
    controls: () => read('controls'),
    bracketSlots: () => read('bracketSlots'),
    hit: (x: number, y: number) => read('hit', { x, y }),
    // 两个换算：地址→位置、位置→地址。公开面只把前者当"唯一会猜的地方"，两条都在 diag 里
    //   给判据与调试用 —— 没有它们，判据就只能拿 (段, 偏移) 当位置判等，而那正是要证伪的写法。
    flowAt: (para: number, offset: number) => read('flowAt', { para, offset }),
    textPosOf: (flow: number) => read('textPosOf', { flow }),
    controlAt: (x: number, y: number) => read('controlAt', { x, y }),
    describeControls: () => str('describeControls'),
    // 下面三条读的是随状态推过来的镜像：两种传输都是同步值。
    blockedEdits: () => Number(read('blockedEdits') ?? 0),
    dirtyPages: () => (read('dirtyPages') as number[]) ?? [],
    pagePx: () => {
      const g = link.geom();
      return { w: g.pw, h: g.ph, gap: g.gap };
    },
    pageCanvases: () => dom.pagesEl.querySelectorAll('canvas').length,
    pageCanvasList: () => [...dom.pagesEl.querySelectorAll('canvas')],
    prewarmPage: (page: number) => {
      // 预热是"捎带的一件事"，不是命令：Worker 传输下它是一次异步往返，没人等它，
      //   拒掉的话会变成未处理的 Promise（页面级错误面会红）。这里只收掉拒绝。
      const p = read('prewarmPage', { page });
      if (p && typeof (p as Promise<unknown>).catch === 'function') {
        (p as Promise<unknown>).catch(() => undefined);
      }
    },
    bench: () => read('bench'),
    caretReport: () => String(read('caretReport') ?? ''),
    caretLive: () => Number(read('caretOffsetLive') ?? -1),
    env: () => ({
      ua: navigator.userAgent,
      url: location.href,
      dpr: window.devicePixelRatio || 1,
      // heap 是随状态推过来的镜像，两种传输都是同步读。
      heapMB: Number(read('heap') ?? 0) / 1048576,
    }),
    fonts: () => read('stats'),
    blitStats: () => dom.blitStats(),
    lastBlit: () => null,
    focus: () => dom.focus(),
    clickAt: async (pt) => {
      const { x, y } = pt as { x: number; y: number };
      await Promise.resolve(dom.input.placeAt({ x, y }));
    },
    selectCell: async (pos: Pos) => {
      await Promise.resolve(link.write('selectCellAtPara', { para: pos.para }));
      dom.finish();
    },
    removeControl: async (index: number, keepContent?: boolean) => {
      // 返回值必须是引擎的判定：表单模式里被 structureLock 拦下的删除是失败，
      //   硬写 true 会让"锁住了没删掉"看起来像"删掉了"。
      const ok = await Promise.resolve(link.write('removeControl', { i: index, keepContent: !!keepContent }));
      if (ok) dom.finish();
      return !!ok;
    },

    // 装载

    loadScenario: (id, zoom) => settleAs('scenario', { id, zoom }),
    reset: (id, zoom) => settleAs('reset', { id: id === undefined ? 6 : id, zoom }),
    stress: (paras, chars) => settleAs('stress', { paras, chars }),
    stressDiverse: (paras, chars) => settleAs('stressDiverse', { paras, chars }),
    loadFormSample: (variant) => settleAs('formSample', { variant: variant ?? 0 }),
    loadMedicalSample: async (repeat) => {
      // 病历正文是一份可替换的文件（服务端下发那条路），不编进产物。
      const res = await fetch(new URL('assets/samples/medical-records.md', assets.base));
      if (!res.ok) throw new Error(`病历样例读取失败：HTTP ${res.status}`);
      const text = cleanMarkdownLines(await res.text());
      return settleAs('textDoc', { text, repeat: Math.max(1, repeat || 1) });
    },
    setFormControls: (list) => settleAs('setFormControls', { list }),

    // 光标与相位

    setCaretAt: (para, offset, field) => settleAs('setCaretAt', { para, offset, field }),
    setCaretOnly: (para, offset) => link.write('setCaretAt', { para, offset, plain: 1 }),
    showCaret: (on) => link.write('phase', { on: !!on }),
    startBlink: () => dom.startBlink(),
    stopBlink: () => dom.stopBlink(),

    // 字体与窗口

    reloadFonts: async (mode) => {
      const m = mode === 'lazy' ? 'lazy' : 'full';
      await Promise.resolve(link.write('fonts', { mode: m }));
      await Promise.resolve(link.settle('fonts'));
      // 字体字节的账在 editStats 里（按需读发生在排版之后，快照会漏掉大头）。
      const st = (await Promise.resolve(read('editStats'))) as {
        fontStorageBytes?: number;
        fontResidentBytes?: number;
      } | null;
      return {
        mode: m,
        bytes: Number((m === 'lazy' ? st?.fontStorageBytes : st?.fontResidentBytes) ?? 0),
      };
    },
    setFontCacheConfig: (blockSize, maxBlocks) =>
      link.write('setFontCacheConfig', { blockSize, blocks: maxBlocks }),
    setViewportAnchor: (first, count) => settleAs('viewport', { first, count }),

    // 能力探测的现场：supported 是库的判断，probe 是那段探针字节本身（判据可以
    //   自己跑一次 validate、再检查里面有没有 v128 指令，而不必只信这个结论）。
    simd: () => ({ supported: simdSupported(), probe: Array.from(simdProbeBytes()) }),

    errors: () => errors.slice(),
  };
}
