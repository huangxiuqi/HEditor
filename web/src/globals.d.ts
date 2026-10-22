/**
 * 迁移期的全局声明。
 *
 * 库现在还是"运行时加载旧的宿主脚本（web/app.js）"，那些脚本把工厂挂在 window 上。
 * 这份声明只描述库自己要用的那几个，别的一律不认 —— 类型上就把门关小。
 *
 * C 步（把宿主搬进 src/）之后，这个文件与 loader 里的注入逻辑一起删掉。
 */

/**
 * 命令表实例（engine_api.js 造出来的）。只写库真正用到的那几个方法。
 */
export interface EngineApi {
  read(name: string, args?: Record<string, unknown>): unknown;
  write(name: string, args?: Record<string, unknown>): boolean;
  engine(): Record<string, (...args: never[]) => unknown>;
}

/**
 * 旧门面：只列迁移垫片真正调到的成员。其余的在运行期存在，但库不依赖。
 */
export interface LegacyHost {
  whenReady: Promise<unknown>;
  ready: boolean;
  /**
   * 页面级异常（旧宿主收集的）。装不上时这里才有内容。
   */
  readonly errors?: string[];
  readonly allText: string;
  readonly zoomFactor: number;
  /**
   * [页宽, 页高, 页间隙]，CSS 像素。
   */
  readonly pagePx: [number, number, number] | null;
  readonly caret: { para: number; offset: number; page: number; x: number; y: number; w: number; h: number; visible: boolean } | null;
  paragraphText(i: number): string;
  saveDocument(): string;
  loadDocument(text: string): unknown;
  loadTextDocument(text: string, repeat?: number): unknown;
  type(s: string): unknown;
  backspace(): unknown;
  deleteForward(): unknown;
  splitParagraph(): unknown;
  mergeWithPreviousParagraph(): unknown;
  mergeWithNextParagraph(): unknown;
  undo(): unknown;
  redo(): unknown;
  indent(): unknown;
  outdent(): unknown;
  move(dcp: number, extend?: boolean): unknown;
  vertical(dir: number, extend?: boolean): unknown;
  docStart(extend?: boolean): unknown;
  docEnd(extend?: boolean): unknown;
  selectAll(): unknown;
  setSelection(anchor: unknown, focus: unknown): unknown;
  clearSelection(): unknown;
  setCaretAtSideLine(para: number, offset: number, side: number, line: number): unknown;
  focusControl(forward?: boolean): unknown;
  setControlProp(i: number, prop: number, value: boolean): unknown;
  setShowBrackets(on: boolean): unknown;
  setShowControlDots(on: boolean): unknown;
  setUndoLimit(n: number): unknown;
  setCaretStyle(style: unknown): unknown;
  setSelectionStyle(style: unknown): unknown;
  zoomToFactor(factor: number): unknown;
  present(): unknown;
  dispose?(): void;
  /**
   * 内部隐藏通道（不可枚举），见 app.js 里 defineProperty 那一段。
   */
  __api?: EngineApi;
}

declare global {
  interface Window {
    /**
     * 旧宿主的工厂（app.js / worker_host.js 自己挂上去的）。
     */
    createEditor?: (container: HTMLElement, opts?: { chrome?: boolean }) => LegacyHost;
    createWorkerEditor?: (container: HTMLElement, opts?: { chrome?: boolean }) => LegacyHost;
    /**
     * 页面自动启动开关：库要把实例攥在自己手里，所以先关掉它。
     */
    __heditorAutoBoot?: boolean;
  }
}

export {};
