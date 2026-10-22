// 同线程传输：引擎就在这一页上。
//
// 这一份只做三件事，别让它长回来：
//   一、把胶水脚本按地址装进来（一次，多个实例共用同一份模块工厂）；
//   二、造命令表实例，把"贴页"这条钩子接到宿主上；
//   三、把 EngineLink 的六个动词映到命令表上 —— 同步值，不是 Promise。
//
// 为什么坚持同步：单线程宿主是"调用即生效"的对照物。把这里改成 await 一层，
//   输入事件处理完就不再是"已经生效"，探针的同步假设连带失效（实测 edit_probe
//   14 项红、耗时 42 → 158 s）。
import { createEngineApi, type EngineApi, type EngineHostHooks } from '../engine/api';
import type { DomHost, EngineLink, HostView } from '../host/dom';
import type { Args } from '../input/keymap';
import type { FontSource } from '../types';

/** 素材地址：胶水、字体、wasm。 */
export interface AssetPaths {
  /** 胶水脚本的绝对地址（heditor.js）。 */
  glue: string;
  /** 缓存戳（`?v=` 之后那一串），会一路带到 wasm 的 locateFile。 */
  bust?: string;
  fonts: Array<{ role: string; path: string; label?: string }>;
  /** 产物目录（相对地址按它解析）。 */
  base: string;
  /** Worker 壳的地址（`transport: 'worker'` 用）。 */
  worker?: string;
}

export interface InlineOptions {
  container: HTMLElement;
  assets: AssetPaths;
  dpr: number;
  /** 密度（设备像素 / 磅）。 */
  zoom: number;
  /** 初始文档：true = 一份空文档（库的默认），false = 演示场景。 */
  empty?: boolean;
}

/** 胶水脚本只注入一次：多个实例各要一个引擎，但脚本本身只该装一遍。 */
const gluePromises = new Map<string, Promise<void>>();

function loadScript(src: string): Promise<void> {
  let p = gluePromises.get(src);
  if (!p) {
    p = new Promise<void>((resolve, reject) => {
      const s = document.createElement('script');
      s.src = src;
      s.onload = () => resolve();
      s.onerror = () => reject(new Error(`无法加载 ${src}`));
      document.head.appendChild(s);
    });
    // 失败之后允许下一次重试：不清掉的话后续实例会直接复用同一个失败结果。
    p.catch(() => gluePromises.delete(src));
    gluePromises.set(src, p);
  }
  return p;
}

export function createInline(opts: InlineOptions): {
  link: EngineLink;
  start(dom: DomHost): Promise<void>;
  destroy(): void;
} {
  const { container, assets } = opts;
  let api: EngineApi | null = null;
  let dom: DomHost | null = null;

  const dpr = (): number => window.devicePixelRatio || opts.dpr || 1;
  const viewSize = (): { w: number; h: number } => {
    const el = container.querySelector<HTMLElement>('.heditor-body') ?? container;
    return {
      w: Math.max(1, Math.round(el.clientWidth * dpr())),
      h: Math.max(1, Math.round(el.clientHeight * dpr())),
    };
  };

  /**
   * 呈现钩子。真正干活的只有贴页那一条 —— 其余三条是 worker 侧的概念
   *   （槽、缺页、画布到达），同线程这一侧不成立，所以显式给空实现而不是 null：
   *   万一以后误调，报的是"这里没有槽"而不是一个空引用。
   */
  const hooks: EngineHostHooks = {
    blitPages: () => dom?.blitPages() ?? 0,
    blitStats: () => ({ ms: 0, pages: 0, px: 0 }),
    invalidateSlots: () => dom?.invalidatePages(),
    addCanvas: () => {},
    setViewMode: () => {},
    slotCount: () => 0,
    pagesMissing: () => 0,
    ctxOfPage: () => null,
    canvasCount: () => 0,
    loadGlue: async (url, exportName) => {
      await loadScript(url);
      const f = (window as unknown as Record<string, unknown>)[exportName];
      if (typeof f !== 'function') {
        throw new Error(`胶水层已加载，但没有导出 ${exportName}`);
      }
      return f;
    },
  };

  function geom(): HostView {
    const st = api?.read('editStats');
    return {
      pw: Number(api?.read('pagePxWidth') ?? 0),
      ph: Number(api?.read('pagePxHeight') ?? 0),
      gap: Number(api?.read('pageGapPx') ?? 0),
      dpr: dpr(),
      totalPages: Math.max(1, Number(st?.totalPages ?? 1)),
      first: Number(api?.read('windowFirstPage') ?? 0),
      count: Math.max(1, Number(api?.read('windowPageCount') ?? 1)),
    };
  }

  const link: EngineLink = {
    read: (name: string, args?: Args) => api?.read(name, args),
    write: (name: string, args?: Args) => (api ? api.write(name, args) : false),
    settle: (opName: string) => {
      const vs = viewSize();
      return api?.settle(opName, {
        scrollY: (dom?.stage.scrollTop ?? 0) * dpr(),
        viewW: vs.w,
        viewH: vs.h,
      });
    },
    view: (scrollY: number) => {
      if (!api) return { moved: false };
      const vs = viewSize();
      return api.view({ scrollY, viewW: vs.w, viewH: vs.h, leadPx: 0 });
    },
    geom,
    setViewMode: (gap: number, continuous: boolean) => {
      api?.setViewMode({ gap, continuous });
      return true;
    },
    takeScrollReset: () => api?.takeScrollReset() ?? false,
    pixels: (page: number) => (api ? (api.engine().pageRgba(page) as Uint8ClampedArray) : null),
    serialize: () => String(api?.read('serialize') ?? ''),
    // 同线程里每条命令都是当场跑完的，队列本来就没有积压。
    flush: () => Promise.resolve(),
  };

  return {
    link,
    async start(target: DomHost): Promise<void> {
      dom = target;
      api = createEngineApi({ post: () => {}, host: hooks });
      const vs = viewSize();
      await api.boot({
        glue: assets.glue,
        bust: assets.bust ?? '',
        exportName: 'CreateHEditor',
        fonts: assets.fonts,
        fontMode: 'full',
        dpr: dpr(),
        viewW: vs.w,
        viewH: vs.h,
        zoom: opts.zoom,
        empty: !!opts.empty,
      });
      target.start();
      // 首帧：present 一次并贴页。装载路径里 relayoutAndPaint 已经把页位图画好了，
      //   这里缺的只是"把像素搬到画布上"那一步。
      api.write('present', {});
      api.settle('present', { scrollY: 0, viewW: vs.w, viewH: vs.h });
      target.finish(api.read('editStats'));
    },
    destroy(): void {
      dom = null;
      api = null;
    },
  };
}

/** 字体清单：默认三份 Noto（正文 / 蒙文 / 拉丁），路径相对产物目录。 */
export function defaultFonts(base: string, list?: FontSource[]): Array<{ role: string; path: string }> {
  const src = list && list.length
    ? list.map((f, i) => ({ role: f.role ?? ['cjk', 'mongol', 'latin'][i] ?? 'cjk', path: f.url }))
    : [
        { role: 'cjk', path: 'assets/fonts/NotoSansSC-VF.ttf' },
        { role: 'mongol', path: 'assets/fonts/NotoSansMongolian-Regular.ttf' },
        { role: 'latin', path: 'assets/fonts/NotoSans-Latin-VF.ttf' },
      ];
  return src.map((f) => ({ role: f.role, path: new URL(f.path, base).href }));
}
