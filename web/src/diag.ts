import { simdProbeBytes, simdSupported } from './engine/simd';
import type { EngineLink, DomHost } from './host/dom';
import { then1, type Args, type Maybe } from './input/keymap';
import type { Diag, Direct, PageMetricsPt, Pos } from './types';

/**
 * diag 是"debug 开关下的公开面"：成员不承诺稳定，所以参数与返回暂时是 unknown。
 * 这里只做转发，不加工 —— 加工会把"引擎报了什么"变成对它的猜测。
 *
 * 三类例外都不是加工，而是补上收尾：装载类要等重排重绘落地，返回值是那一拍的
 *   editStats（探针拿它当分母）；定位类要 present，否则画面停在旧位置；闪烁与相位
 *   只动状态、绝不 present —— 呈现会把上一次全量呈现的账冲成"只画了个光标"。
 *
 * @param link 引擎通道
 * @param dom DOM 宿主（装载类要它把这一拍收尾）
 * @param errors 页面级错误表（原地追加，诊断面只给切片）
 * @param syncConfig 把引擎的编辑模式与缩放读回配置影子
 * @returns 诊断面
 */
export function makeDiag(
  link: EngineLink,
  dom: DomHost,
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
    auditFrame: () => read('auditFrame'),
    auditBase: () => read('auditBase'),
    auditPage: () => read('auditPage'),
    auditLayout: () => read('auditLayout'),
    auditCompile: () => read('auditCompile'),
    auditGlyphPages: () => read('auditGlyphPages'),
    selfCheck: () => read('selfCheck'),
    markCheck: () => read('markCheck'),
    pageMetrics: () => read('pageMetrics') as Direct<PageMetricsPt>,
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
    pagesWrittenThisFrame: () => (read('pagesWrittenThisFrame') as number[]) ?? [],
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
