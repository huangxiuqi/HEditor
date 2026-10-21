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
  SelectionStyle,
  SetCaretOptions,
} from '../types/editor';
import type { EngineApi, LegacyHost } from './globals';
import { guard } from './guard';

/* ────────────────────────── 迁移期的脚本装载 ────────────────────────── */

/**
 * 旧宿主的脚本。顺序是硬要求：host_core 与 engine_api 都必须在 app.js 之前，
 *   前者是输入层（app.js 一加载就取它的工厂），后者是命令表。
 */
const LEGACY_SCRIPTS = [
  'host_error.js',
  'drag_select.js',
  'wasm_feature.js',
  'host_core.js',
  'engine_api.js',
  'app.js',
];

/** 本模块自己的地址 —— 打包成 IIFE 时靠它推出旧脚本目录。 */
const SELF_SRC: string =
  (document.currentScript instanceof HTMLScriptElement && document.currentScript.src) || '';

let loadOnce: Promise<{ create: (c: HTMLElement, o?: { chrome?: boolean }) => LegacyHost }> | null =
  null;

function baseUrl(opts: EditorOptions): string {
  if (opts.assets?.base) return opts.assets.base;
  if (SELF_SRC) return new URL('../', SELF_SRC).href;
  return new URL('./', location.href).href;
}

function injectScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const el = document.createElement('script');
    el.src = src;
    el.onload = () => resolve();
    el.onerror = () => reject(new Error(`加载脚本失败：${src}`));
    document.head.appendChild(el);
  });
}

/**
 * 装载旧宿主。只装一次（模块级 promise）。
 *
 * 为什么要关 `__heditorAutoBoot`：那些脚本见到页面上有 `#stage` 就会自己造一个实例并挂到
 *   `window.__heditor` 上。那样一来同一个容器上会有两个实例，而且外部拿到了内部门面 ——
 *   §11 的闸 1 正是要断掉这条路。所以先关自动启动，由库自己调工厂。
 */
function loadLegacy(
  opts: EditorOptions,
): Promise<{ create: (c: HTMLElement, o?: { chrome?: boolean }) => LegacyHost }> {
  if (loadOnce) return loadOnce;
  loadOnce = (async () => {
    window.__heditorAutoBoot = false;
    const base = baseUrl(opts);
    for (const name of LEGACY_SCRIPTS) {
      await injectScript(new URL(name, base).href);
    }
    const create = window.createEditor;
    if (!create) {
      throw new Error('旧宿主没有导出 createEditor —— 脚本装上了但没跑起来？');
    }
    // 闸 1：把内部工厂从 window 上撤掉。库自己攥着引用，外部拿不到 ——
    //   留着的话 demo 页只要写 `window.createEditor(...)` 就能再造一个「内部门面」。
    delete window.createEditor;
    delete window.createWorkerEditor;
    Object.defineProperty(window, 'createEditor', {
      configurable: false,
      get() {
        throw new TypeError('createEditor 是内部工厂，已经收起来了；用 HEditor.createEditor');
      },
    });
    return { create };
  })();
  return loadOnce;
}

/* ────────────────────────── 标记 ────────────────────────── */

/**
 * 造编辑器需要的 DOM 骨架。
 *
 * 迁移期这份标记还必须与旧宿主对齐（它按 id 找元素）。等 C 步把它们搬进 src/ 之后，
 *   标记由库自己拥有，这段与 app.js 的耦合才会断。
 */
function ensureMarkup(container: HTMLElement): void {
  if (container.querySelector('#stage')) return;
  container.innerHTML = [
    '<div class="heditor-body" style="position:relative;height:100%;overflow:auto">',
    '  <div id="stage" style="position:relative">',
    '    <div id="pages"></div>',
    '    <textarea id="ime" rows="1" spellcheck="false"',
    '      style="position:absolute;left:0;top:0;width:1px;height:1em;opacity:0;',
    '             border:0;padding:0;resize:none;overflow:hidden"></textarea>',
    '  </div>',
    '</div>',
  ].join('\n');
}

/* ────────────────────────── 段落与形状 ────────────────────────── */

type ControlsPayload = {
  items?: Array<{ kind?: string; label?: string; ro?: boolean; i?: number }>;
  caret?: number;
};

type SelectionPayload = {
  anchor?: Pos;
  focus?: Pos;
  empty?: boolean;
  rects?: Array<{ page?: number; x?: number; y?: number; w?: number; h?: number }>;
};

type EditStatsPayload = { totalPages?: number; caretPara?: number };

/* ────────────────────────── 事件 ────────────────────────── */

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

/* ────────────────────────── 工厂 ────────────────────────── */

const DEFAULT_CONFIG: EditorConfig = {
  zoom: 1,
  pageMode: 'paged',
  readOnly: false,
  input: true,
  showBrackets: true,
  showControlDots: false,
  undoLimit: 0,
  caretStyle: { color: [0, 0, 0, 255], widthPt: 1 },
  selectionStyle: { color: [31, 111, 235, 64] },
};

/**
 * 造一个编辑器。返回时已就绪（排版跑完、首帧画完）。
 *
 * 这一版只接了 `transport: 'inline'`（主线程）。Worker 那条要等 C 步把输入层搬进库 ——
 *   现在强行接只会得到"消息表在两边各写一份"的老问题。
 */
export async function createEditor(opts: EditorOptions): Promise<Editor> {
  if (!opts || !(opts.container instanceof HTMLElement)) {
    throw new TypeError('createEditor({ container }) 需要一个 HTMLElement');
  }
  const transport = opts.transport ?? 'inline';
  if (transport !== 'inline') {
    throw new Error("transport: 'worker' 还没接入（见 docs/外部API设计.md §14 的 C 步）");
  }

  const legacy = await loadLegacy(opts);
  ensureMarkup(opts.container);
  const host = legacy.create(opts.container, { chrome: !!opts.chrome });
  if (!host) throw new Error('旧宿主返回了空实例');
  await host.whenReady;

  const api = host.__api;
  if (!api) {
    const errs = (host.errors ?? []).join(' | ');
    throw new Error(
      `旧宿主没有暴露命令表（就绪=${host.ready}）${errs ? '：' + errs : '；也没有页面错误'}`,
    );
  }

  const emitter = new Emitter();
  const config: EditorConfig = { ...DEFAULT_CONFIG, ...(opts.config ?? {}) };
  let version = 0;
  let disposed = false;

  const alive = (): void => {
    if (disposed) throw new Error('编辑器已销毁');
  };

  /** 改完一次之后统一收尾：版本号 +1，发一次 change。 */
  const afterWrite = (changed: boolean, source: 'user' | 'api'): boolean => {
    if (!changed) return false;
    ++version;
    emitter.emit('change', makeChange(host, source, version));
    return true;
  };

  const write = (name: string, args?: Record<string, unknown>): boolean => {
    alive();
    return api.write(name, args);
  };

  /**
   * 编辑类入口的统一闸。只读时返回 false，**不代表出错** —— 与"这次编辑被引擎拒绝"
   *   是同一种语义（见设计文档 §9 的错误三分）。
   */
  const canEdit = (): boolean => {
    alive();
    return !config.readOnly;
  };

  const editor: Editor = {
    /* ── 内容 ── */

    async open(data, o) {
      alive();
      const text = String(data ?? '');
      const looksJson = text.trimStart().startsWith('{');
      if (o?.format === 'json' || (o?.format !== 'text' && looksJson)) {
        host.loadDocument(text); // 失败会抛，引擎状态不变
      } else {
        host.loadTextDocument(text);
      }
      ++version;
      emitter.emit('change', makeChange(host, 'api', version));
    },

    async setText(text, o) {
      alive();
      host.loadTextDocument(String(text ?? ''), o?.repeat ?? 1);
      ++version;
      emitter.emit('change', makeChange(host, 'api', version));
    },

    text() {
      alive();
      return host.allText;
    },

    paragraph(i) {
      alive();
      return host.paragraphText(i);
    },

    paragraphs() {
      alive();
      return host.allText.split('\n');
    },

    async save() {
      alive();
      return host.saveDocument();
    },

    async insertText(pos, text) {
      if (!canEdit()) return false;
      return afterWrite(
        write('insertText', { para: pos.para, offset: pos.offset, s: String(text) }),
        'api',
      );
    },

    /* ── 编辑 ── */

    async type(text) {
      if (!canEdit()) return false;
      return afterWrite(!!host.type(String(text)), 'api');
    },

    async deleteBackward(n = 1) {
      if (!canEdit()) return false;
      let changed = false;
      for (let i = 0; i < Math.max(1, n); ++i) {
        if (!host.backspace()) break;
        changed = true;
      }
      return afterWrite(changed, 'api');
    },

    async deleteForward(n = 1) {
      if (!canEdit()) return false;
      let changed = false;
      for (let i = 0; i < Math.max(1, n); ++i) {
        if (!host.deleteForward()) break;
        changed = true;
      }
      return afterWrite(changed, 'api');
    },

    async splitParagraph() {
      if (!canEdit()) return false;
      return afterWrite(!!host.splitParagraph(), 'api');
    },

    async mergeParagraph(dir = 'prev') {
      if (!canEdit()) return false;
      const ok =
        dir === 'next' ? !!host.mergeWithNextParagraph() : !!host.mergeWithPreviousParagraph();
      return afterWrite(ok, 'api');
    },

    async undo() {
      if (!canEdit()) return false;
      return afterWrite(!!host.undo(), 'api');
    },

    async redo() {
      if (!canEdit()) return false;
      return afterWrite(!!host.redo(), 'api');
    },

    async setIndent(levels) {
      if (!canEdit()) return false;
      const n = Math.abs(Math.trunc(levels));
      let changed = false;
      for (let i = 0; i < n; ++i) {
        const r = levels > 0 ? host.indent() : host.outdent();
        if (!r) break;
        changed = true;
      }
      return afterWrite(changed, 'api');
    },

    /* ── 光标与选区 ── */

    caret() {
      alive();
      const c = host.caret;
      const pos = { para: c?.para ?? 0, offset: c?.offset ?? 0 };
      return {
        pos,
        page: c?.page ?? 0,
        rect: { x: c?.x ?? 0, y: c?.y ?? 0, w: c?.w ?? 0, h: c?.h ?? 0 },
        visible: !!c?.visible,
      } satisfies Caret;
    },

    async setCaret(pos, o?: SetCaretOptions) {
      alive();
      host.setCaretAtSideLine(
        pos.para,
        pos.offset,
        o?.side === 'after' ? 1 : 0,
        o?.line === 'last' ? 1 : 0,
      );
    },

    async moveCaret(step: CaretStep) {
      alive();
      switch (step) {
        case 'left':
          host.move(-1);
          break;
        case 'right':
          host.move(1);
          break;
        case 'up':
          host.vertical(-1);
          break;
        case 'down':
          host.vertical(1);
          break;
        case 'docStart':
          host.docStart();
          break;
        case 'docEnd':
          host.docEnd();
          break;
        case 'lineStart':
          write('lineStart');
          host.present();
          break;
        case 'lineEnd':
          write('lineEnd');
          host.present();
          break;
      }
    },

    async select(anchor, focus) {
      alive();
      host.setSelection(anchor, focus);
    },

    async selectAll() {
      alive();
      host.selectAll();
    },

    async clearSelection() {
      alive();
      host.clearSelection();
    },

    selection(): Selection {
      alive();
      const raw = (api.read('selectionReport') ?? {}) as SelectionPayload;
      const rects = (raw.rects ?? []).map((r) => ({
        page: r.page ?? 0,
        x: r.x ?? 0,
        y: r.y ?? 0,
        w: r.w ?? 0,
        h: r.h ?? 0,
      }));
      return {
        anchor: raw.anchor ?? { para: 0, offset: 0 },
        focus: raw.focus ?? { para: 0, offset: 0 },
        empty: !!raw.empty,
        rects,
        cells: null,
      };
    },

    /* ── 控件 ── */

    async focusControl(dir = 'next') {
      alive();
      write('focusControl', { forward: dir === 'next' });
      host.present();
    },

    async setControlProp(index, prop, value) {
      if (!canEdit()) return false;
      return afterWrite(!!host.setControlProp(index, prop, value), 'api');
    },

    form(): ControlSummary[] {
      alive();
      const raw = (api.read('controls') ?? {}) as ControlsPayload;
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

    /* ── 视图 ── */

    geometry(): Geometry {
      alive();
      const px = host.pagePx ?? [0, 0, 0];
      const st = (api.read('editStats') ?? {}) as EditStatsPayload;
      const pages = st.totalPages ?? 0;
      const first = Number(api.read('windowFirstPage') ?? 0);
      const count = Number(api.read('windowPageCount') ?? 0);
      return {
        page: { w: px[0], h: px[1] },
        gap: px[2],
        pages,
        scrollHeight: pages * (px[1] + px[2]),
        window: { first, count },
        dpr: window.devicePixelRatio || 1,
      };
    },

    async scrollTo(target) {
      alive();
      const g = editor.geometry();
      const y = target.y ?? (target.page ?? 0) * (g.page.h + g.gap);
      const scroller =
        opts.container.querySelector<HTMLElement>('.heditor-body') ?? opts.container;
      scroller.scrollTop = y;
      host.present();
    },

    async setZoom(zoom) {
      await editor.configure({ zoom });
    },

    /* ── 配置 ── */

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
      if (patch.readOnly !== undefined) next.readOnly = !!patch.readOnly;
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

      // 只做最贵的那一档：先重排类，再重绘类，最后无代价类。
      if (changed.zoom !== undefined) host.zoomToFactor(next.zoom);
      if (changed.pageMode !== undefined) {
        const eng = api.engine();
        const setContinuous = eng['setContinuous'] as ((on: boolean) => void) | undefined;
        const setPageGap = eng['setPageGap'] as ((px: number) => void) | undefined;
        setContinuous?.(next.pageMode === 'continuous');
        // 分页模式的页间隙取当前页度量的那一份（连页时为 0）。
        setPageGap?.(next.pageMode === 'continuous' ? 0 : (host.pagePx?.[2] ?? 12));
        // 重排一次：连页标志改的是排版形状，不重排就还是旧页的落点。
        host.zoomToFactor(host.zoomFactor);
      }
      if (changed.showBrackets !== undefined) host.setShowBrackets(next.showBrackets);
      if (changed.showControlDots !== undefined) host.setShowControlDots(next.showControlDots);
      if (changed.undoLimit !== undefined) host.setUndoLimit(next.undoLimit);
      if (changed.caretStyle !== undefined) host.setCaretStyle(next.caretStyle);
      if (changed.selectionStyle !== undefined) host.setSelectionStyle(next.selectionStyle);

      Object.assign(config, next);
      emitter.emit('configchange', { changed, config: { ...config } });
      return { ...config };
    },

    config() {
      return Object.freeze({ ...config });
    },

    /* ── 事件与生命周期 ── */

    on(event, cb) {
      alive();
      return emitter.on(event, cb);
    },

    async flush() {
      await Promise.resolve();
    },

    async destroy() {
      if (disposed) return;
      disposed = true;
      host.dispose?.();
      opts.container.innerHTML = '';
    },

    get element() {
      return opts.container;
    },

    get diag() {
      if (!opts.debug) {
        throw new TypeError('diag 只在 createEditor({ debug: true }) 时可用');
      }
      return makeDiag(host, api);
    },
  };

  if (opts.config) await editor.configure(opts.config);
  return guard(editor, !!opts.debug) as Editor;
}

/* ────────────────────────── 变更事件 ────────────────────────── */

/**
 * `json` 是惰性 getter：2000 段文档序列化一次约 55 ms，每次按键都算会卡住键盘。
 * 同一个版本只算一次（用闭包缓存）。
 */
function makeChange(host: LegacyHost, source: 'user' | 'api', version: number): ChangeEvent {
  let cached: string | undefined;
  return {
    source,
    version,
    get text() {
      return host.allText;
    },
    get json() {
      if (cached === undefined) cached = host.saveDocument();
      return cached;
    },
  };
}

/* ────────────────────────── 诊断面 ────────────────────────── */

/**
 * diag 是"debug 开关下的公开面"：成员不承诺稳定，所以参数与返回暂时是 unknown。
 * 这里只做转发，不加工 —— 加工会把"引擎报了什么"变成"我们以为引擎报了什么"。
 */
function makeDiag(host: LegacyHost, api: EngineApi): Diag {
  const read = (name: string): unknown => api.read(name);
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
    selfCheck: () => host.present(),
    markCheck: () => host.present(),
    rawFrame: () => null,
    frameDigest: () => String(read('frameDigest') ?? ''),
    frameW: () => Number(read('frameWidth') ?? 0),
    frameH: () => Number(read('frameHeight') ?? 0),
    present: () => void host.present(),
    controls: () => read('controls'),
    bracketSlots: () => read('bracketSlots'),
    hit: (x, y) => api.read('hit', { x, y }),
    controlAt: (x, y) => api.read('controlAt', { x, y }),
    describeControls: () => String(read('describeControls') ?? ''),
    blockedEdits: () => Number(read('blockedEdits') ?? 0),
    dirtyPages: () => (read('dirtyPages') as number[]) ?? [],
    pagePx: () => {
      const px = host.pagePx ?? [0, 0, 0];
      return { w: px[0], h: px[1], gap: px[2] };
    },
    pageCanvases: () => null,
    pageCanvasList: () => [],
    prewarmPage: (page: number) => void api.read('prewarmPage', { page }),
    bench: () => read('bench'),
    caretReport: () => String(read('caretReport') ?? ''),
    caretLive: () => Number(read('caretOffsetLive') ?? -1),
    env: () => read('stats'),
    fonts: () => read('stats'),
    blitStats: () => ({ ms: 0, pages: 0, px: 0 }),
    lastBlit: () => null,
    clickAt: async () => {
      throw new Error('diag.clickAt 还没接（它要的是画布坐标，等输入层进库）');
    },
    selectCell: async () => {
      throw new Error('diag.selectCell 还没接');
    },
    removeControl: async (index: number) => {
      api.write('removeControl', { i: index });
      host.present();
      return true;
    },
  };
}
