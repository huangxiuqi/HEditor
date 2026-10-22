import type {
  Caret,
  CaretStep,
  ChangeEvent,
  ControlSummary,
  Diag,
  Editor,
  EditorConfig,
  EditorEvents,
  EditorOptions,
  Geometry,
  Pos,
  Selection,
  SetCaretOptions,
} from './types';
import { createDomHost, type ChangeSource, type DomHost, type EngineLink } from './host/dom';
import type { CanvasStore } from './host/pages';
import { cleanMarkdownLines, then1, type Args } from './input/keymap';
import { createInline, defaultFonts, type AssetPaths } from './transport/inline';
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
function resolveAssets(opts: EditorOptions): AssetPaths {
  const a = opts.assets ?? {};
  // 默认布局：库在 web/dist/，素材在仓库根下（assets/、build/wasm/web/）。
  //   所以默认 base 是"库目录往上两级"。这不是约定，只是本仓库开发时的摆法 ——
  //   产品里三项都该显式给。
  const base = a.base ? new URL(a.base, document.baseURI).href : new URL('../../', selfDir()).href;
  const glue = a.glue ? new URL(a.glue, document.baseURI).href : new URL('build/wasm/web/heditor.js', base).href;
  // Worker 壳与库产物同一目录（web/dist/heditor_worker.js）。
  const worker = new URL('heditor_worker.js', selfDir()).href;
  return { glue, base, worker, bust: '', fonts: defaultFonts(base, opts.fonts) };
}

// 事件

class Emitter {
  private map = new Map<string, Set<(ev: never) => void>>();

  on<K extends keyof EditorEvents>(event: K, cb: (ev: EditorEvents[K]) => void): () => void {
    let set = this.map.get(event);
    if (!set) {
      set = new Set();
      this.map.set(event, set);
    }
    set.add(cb as (ev: never) => void);
    return () => set?.delete(cb as (ev: never) => void);
  }

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

const DEFAULT_CONFIG: EditorConfig = {
  zoom: 1,
  pageMode: 'paged',
  mode: 'document',
  input: true,
  showBrackets: true,
  showControlDots: false,
  undoLimit: 0,
  caretStyle: { color: [0, 0, 0, 255], widthPt: 1 },
  selectionStyle: { color: [31, 111, 235, 64] },
};

/**
 * 页间隙（磅）：分页档用它，连页档是 0。
 */
const kPageGapPt = 14;

// 工厂

/**
 * 造一个编辑器。返回时已就绪（排版跑完、首帧画完、输入层已接上）。
 *
 * 两个传输共用这一份实现：差别只有"结果何时到"（同步值 / Promise）与"片元在哪贴"
 *   （这里 / Worker），两者都收在 EngineLink 里。
 */
export async function createEditor(opts: EditorOptions): Promise<Editor> {
  if (!opts || !(opts.container instanceof HTMLElement)) {
    throw new TypeError('createEditor({ container }) 需要一个 HTMLElement');
  }
  if (opts.container.childElementCount > 0 && !opts.container.querySelector('#stage')) {
    // 不清空别人的内容：只提示。要复用一块已有标记的容器就让它带着 #stage 进来。
    throw new TypeError('container 里已经有内容了：请传一个空元素（或带 #stage 的那一份）');
  }
  const transport = opts.transport ?? 'inline';
  if (transport !== 'inline' && transport !== 'worker') {
    throw new TypeError("transport 只能是 'inline' 或 'worker'");
  }

  const assets = resolveAssets(opts);
  const emitter = new Emitter();
  const config: EditorConfig = { ...DEFAULT_CONFIG, ...(opts.config ?? {}) };
  let version = 0;
  let disposed = false;
  let lastStats: any = null;
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

  if (transport === 'inline') {
    const t = createInline({
      container: opts.container,
      assets,
      dpr: window.devicePixelRatio || 1,
      zoom: config.zoom * (4 / 3) * (window.devicePixelRatio || 1),
      empty: true,
    });
    link = t.link;
    dom = createDomHost(hostOptions(link));
    starter = () => t.start(dom);
    destroyer = () => t.destroy();
  } else {
    const t = createWorker({
      container: opts.container,
      assets,
      dpr: window.devicePixelRatio || 1,
      zoom: config.zoom * (4 / 3) * (window.devicePixelRatio || 1),
      empty: true,
      makeHost: (l, store) => createDomHost(hostOptions(l, store)),
    });
    link = t.link;
    dom = t.host;
    starter = () => t.start();
    destroyer = () => t.destroy();
  }

  /**
   * 宿主用的选项。link 显式传进来 —— 它由传输造，宿主的回调不能靠闭包去猜。
   */
  function hostOptions(
    l: EngineLink,
    store?: (pagesEl: HTMLElement) => CanvasStore,
  ): Parameters<typeof createDomHost>[0] {
    return {
      container: opts.container,
      link: l,
      input: config.input !== false,
      canvasStore: store,
      readOnly: () => config.mode === 'readOnly',
      status: (text, isErr) => {
        if (!isErr) return;
        pageErrors.push(text);
        emitter.emit('error', { message: text, fatal: false });
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
      onPageError: (message: string) => {
        if (disposed) return;
        pageErrors.push(message);
        emitter.emit('error', { message, fatal: false });
      },
    };
  }

  await starter();

  // 读写

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
   */
  const caretOp = (name: string, args?: Args): Promise<void> => {
    alive();
    return Promise.resolve(
      then1(link.write(name, args), () =>
        then1(link.settle(name), (st) => {
          const stats = (st as { stats?: unknown } | null)?.stats;
          dom.finish((stats ?? link.read('editStats')) as never);
          return undefined;
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
      const p = l.read('flowTextPos', { flow }) as Pos | null;
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

    async setText(text) {
      alive();
      await edit('textDoc', { text: String(text ?? '') });
    },

    text() {
      alive();
      return String(link.read('textAll') ?? '');
    },

    paragraph(i) {
      alive();
      return String(link.read('text', { i }) ?? '');
    },

    paragraphs() {
      alive();
      return String(link.read('textAll') ?? '').split('\n');
    },

    async save() {
      alive();
      return String((await Promise.resolve(link.serialize())) ?? '');
    },

    async insertText(pos, text) {
      return edit('insertText', { para: pos.para, offset: pos.offset, s: String(text) });
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

    async setIndent(levels) {
      const n = Math.abs(Math.trunc(levels));
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
      if (o?.side) {
        await caretOp('setCaretAtSideLine', {
          para: pos.para,
          offset: pos.offset,
          side: o.side === 'after' ? 1 : 0,
          line: o.line === 'last' ? 1 : 0,
        });
        return;
      }
      await caretOp('setCaret', { para: pos.para, offset: pos.offset });
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
          await caretOp('docStart');
          break;
        case 'docEnd':
          await caretOp('docEnd');
          break;
        case 'lineStart':
          await caretOp('lineStart');
          break;
        case 'lineEnd':
          await caretOp('lineEnd');
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
        items?: Array<{ kind?: string; label?: string; ro?: boolean; i?: number }>;
        caret?: number;
      };
      const items = raw.items ?? [];
      const focused = raw.caret ?? -1;
      return items.map((it, index) => ({
        index,
        kind: String(it.kind ?? ''),
        label: String(it.label ?? ''),
        value: null,
        focused: index === focused,
      }));
    },

    // 视图

    geometry(): Geometry {
      alive();
      const g = link.geom();
      const dpr = g.dpr || 1;
      const pages = Math.max(1, Math.round(g.totalPages));
      const step = g.ph + g.gap;
      return {
        page: { w: g.pw / dpr, h: g.ph / dpr },
        gap: g.gap / dpr,
        pages,
        scrollHeight: Math.max(1, (pages * step - g.gap) / dpr),
        window: { first: g.first, count: g.count },
        dpr,
      };
    },

    async scrollTo(target) {
      alive();
      const g = this.geometry();
      const y = target.y ?? (target.page ?? 0) * (g.page.h + g.gap);
      const scroller =
        opts.container.querySelector<HTMLElement>('.heditor-body') ?? opts.container;
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
      if (patch.mode !== undefined) {
        if (patch.mode !== 'document' && patch.mode !== 'form' && patch.mode !== 'readOnly') {
          throw new TypeError("mode 只能是 'document' / 'form' / 'readOnly'");
        }
        next.mode = patch.mode;
      }
      if (patch.input !== undefined) next.input = !!patch.input;
      if (patch.showBrackets !== undefined) next.showBrackets = !!patch.showBrackets;
      if (patch.showControlDots !== undefined) next.showControlDots = !!patch.showControlDots;
      if (patch.undoLimit !== undefined) next.undoLimit = Math.max(0, Math.trunc(patch.undoLimit));
      if (patch.caretStyle !== undefined) next.caretStyle = patch.caretStyle;
      if (patch.selectionStyle !== undefined) next.selectionStyle = patch.selectionStyle;

      const changed: Partial<EditorConfig> = {};
      for (const key of Object.keys(next) as Array<keyof EditorConfig>) {
        if (JSON.stringify(next[key]) !== JSON.stringify(config[key])) {
          (changed as Record<string, unknown>)[key] = next[key];
        }
      }
      if (Object.keys(changed).length === 0) return { ...config };

      // 一次 configure 只做最贵的那一档：先重排类，再重绘类，最后无代价类。
      //   顺序不能反：重排会把页位图整体作废，之后的重绘才有意义。
      const steps: Array<() => Promise<unknown> | unknown> = [];

      if (changed.zoom !== undefined) {
        const dpr = window.devicePixelRatio || 1;
        const density = next.zoom * (4 / 3) * dpr;
        dom.zoom.set(density);
        steps.push(() => settleAfter('zoom', { zoom: density, dpr }));
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
      emitter.emit('configchange', { changed, config: { ...config } });
      return { ...config };
    },

    config() {
      return Object.freeze({ ...config });
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
      return makeDiag(link, dom, assets, pageErrors);
    },
  };

  /**
   * 一次配置类命令的收尾：写完就 present + 贴页（与 doOp 的口径同一条）。
   */
  function settleAfter(opName: string, args?: Args): Promise<unknown> | unknown {
    if (args) link.write(opName, args);
    return then1(link.settle(opName), (st) => {
      lastStats = st ?? link.read('editStats');
      dom.finish(lastStats);
      return st;
    });
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

  if (opts.config) await editor.configure(opts.config);
  return guard(editor, !!opts.debug) as Editor;
}

// 诊断面

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
): Diag {
  const read = (name: string, args?: Args): unknown => link.read(name, args);

  /**
   * 装载类收尾：写完等一次收尾，返回那一拍的账。
   */
  const settleAs = async (name: string, args: Args): Promise<unknown> => {
    await Promise.resolve(link.write(name, args));
    await Promise.resolve(link.settle(name));
    return read('editStats');
  };

  return {
    stats: () => read('stats'),
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
    frameDigest: () => Number(read('frameDigest') ?? 0),
    frameW: () => Number(read('frameWidth') ?? 0),
    frameH: () => Number(read('frameHeight') ?? 0),
    present: () => void dom.repaint(),
    controls: () => read('controls'),
    bracketSlots: () => read('bracketSlots'),
    hit: (x: number, y: number) => read('hit', { x, y }),
    controlAt: (x: number, y: number) => read('controlAt', { x, y }),
    describeControls: () => String(read('describeControls') ?? ''),
    blockedEdits: () => Number(read('blockedEdits') ?? 0),
    dirtyPages: () => (read('dirtyPages') as number[]) ?? [],
    pagePx: () => {
      const g = link.geom();
      return { w: g.pw, h: g.ph, gap: g.gap };
    },
    pageCanvases: () => dom.pagesEl.querySelectorAll('canvas').length,
    pageCanvasList: () => [...dom.pagesEl.querySelectorAll('canvas')],
    prewarmPage: (page: number) => void read('prewarmPage', { page }),
    bench: () => read('bench'),
    caretReport: () => String(read('caretReport') ?? ''),
    caretLive: () => Number(read('caretOffsetLive') ?? -1),
    env: () => ({
      ua: navigator.userAgent,
      url: location.href,
      dpr: window.devicePixelRatio || 1,
      heapMB: Number(read('heap') ?? 0) / 1048576,
    }),
    fonts: () => read('stats'),
    blitStats: () => ({ ms: 0, pages: 0, px: 0 }),
    lastBlit: () => null,
    clickAt: async (pt) => {
      const { x, y } = pt as { x: number; y: number };
      await Promise.resolve(dom.input.placeAt({ x, y }));
    },
    selectCell: async (pos: Pos) => {
      await Promise.resolve(link.write('selectCellAtPara', { para: pos.para }));
      dom.finish();
    },
    removeControl: async (index: number, keepContent?: boolean) => {
      await Promise.resolve(link.write('removeControl', { i: index, keepContent: !!keepContent }));
      dom.finish();
      return true;
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
    setCaretOnly: (para, offset) => link.write('setCaret', { para, offset }),
    showCaret: (on) => link.write('phase', { on: !!on }),
    startBlink: () => dom.startBlink(),
    stopBlink: () => dom.stopBlink(),

    // 字体与窗口

    reloadFonts: async (mode) => {
      const m = mode === 'lazy' ? 'lazy' : 'full';
      await Promise.resolve(link.write('fonts', { mode: m }));
      await Promise.resolve(link.settle('fonts'));
      // 字体字节的账在 editStats 里（按需读发生在排版之后，快照会漏掉大头）。
      const st = read('editStats') as { fontStorageBytes?: number; fontResidentBytes?: number } | null;
      return {
        mode: m,
        bytes: Number((m === 'lazy' ? st?.fontStorageBytes : st?.fontResidentBytes) ?? 0),
      };
    },
    setFontCacheConfig: (blockSize, maxBlocks) =>
      link.write('setFontCacheConfig', { blockSize, blocks: maxBlocks }),
    setViewportAnchor: (first, count) => settleAs('viewport', { first, count }),

    errors: () => errors.slice(),
  };
}
