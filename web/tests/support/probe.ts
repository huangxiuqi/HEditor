// 探针的场地：Playwright 负责浏览器生命周期，这一层只补探针自己的三件事。
//
// 三件事都不该由每个 spec 重复一遍：打开示例页并等到就绪、把一段代码包成异步 IIFE
// 再求值、求值之后等宿主排空。第三条是 Worker 传输的必要条件：门面是异步的，
//   不排空就拿到上一拍的镜像，断言会拿旧值判定，看起来只是某条莫名红了。
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { expect, test as base, type CDPSession, type Page } from '@playwright/test';

// 仓库根：从本文件往上三级（support/ → tests/ → web/ → 仓库）。
const REPO = fileURLToPath(new URL('../../..', import.meta.url)).replace(/[\\/]+$/, '');

// 传输口径。库有两个：页面线程内同步执行，或整条引擎放进 Worker。
export type Transport = 'inline' | 'worker';

// 配置里可以覆盖的探针选项，由 projects 分别赋值。
export type ProbeOptions = {
  // 传输方式，两个 project 各取一个值。
  transport: Transport;
  // wasm 产物目录，相对仓库根。
  wasmDir: string;
  // 页面根路径：构建产物在 /web/dist-app/，Vite 源码直连在 /。
  pageBase: string;
};

// 页面文件名与其余参数。
export type OpenOptions = {
  // 页面文件名，例如 index.html 或 multi.html。
  file: string;
  // 额外的查询参数，例如造致命档用的 glue。
  query?: Record<string, string>;
  // 视口；不传就用配置里的 1500x1200。
  viewport?: { width: number; height: number };
  // 注入迁移垫片（旧门面名 window.__heditor 挂到新公开面上），编辑探针用。
  adapter?: boolean;
};

// 按文件名打开探针页的函数，由 fixture 注入。
export type OpenPage = (file: string, more?: Omit<OpenOptions, 'file'>) => Promise<ProbePage>;

// 示例页与多实例页暴露的启动标志，测试里按它判就绪或失败。
type BootWindow = Window & {
  __demoReady?: boolean;
  __demoStartError?: string;
};

// 求值后的排空表达式：把页面上所有编辑器实例的待发消息推完。
const DRAIN = `(async () => {
  const w = window;
  const handles = [w.__demo, w.__demoA, w.__demoB, w.__heditor].filter(Boolean);
  // 已销毁的实例在读到它的一刻就会抛，排空不是判据，逐个别吞掉。
  for (const h of handles) {
    try {
      if (typeof h.flush === 'function') await h.flush();
      else if (typeof h.quiesce === 'function') await h.quiesce();
    } catch (e) { /* 已销毁 */ }
  }
  return true;
})()`;

// 带探针选项与 open 工厂的 test，写法与普通 Playwright 测试相同。
export const test = base.extend<
  ProbeOptions & { open: OpenPage }
>({
  transport: ['inline', { option: true }],
  wasmDir: ['build/wasm/web', { option: true }],
  pageBase: ['/web/dist-app/', { option: true }],
  open: async ({ page, transport, pageBase }, use) => {
    await use((file, more) =>
      ProbePage.open(page, { file, pageBase, transport, ...more }));
  },
});

export { expect };

/**
 * 一页探针对象：负责导航、求值、截图、按需下发原始 CDP 事件。
 *
 * 页面里执行的代码是字符串字面量，与旧探针逐条对应，便于迁移时逐条对拍。
 */
export class ProbePage {
  private session: CDPSession | null = null;

  private constructor(
    // 底层 Playwright 页面。
    readonly page: Page,
    // 实际打开的地址，报告里用它复核口径。
    readonly url: string,
  ) {}

  /**
   * 打开示例页并等到宿主报就绪。
   *
   * @param page Playwright 页面
   * @param opts 页面文件名、页面根路径与传输方式
   * @returns 就绪的探针页对象
   */
  static async open(page: Page, opts: OpenOptions & {
    transport: Transport;
    pageBase: string;
  }): Promise<ProbePage> {
    if (opts.viewport) await page.setViewportSize(opts.viewport);
    // 垫片必须早于导航注入：它自己等 __demoReady，再把旧门面名挂到公开面上。
    if (opts.adapter) {
      await page.addInitScript({ path: join(REPO, 'web', 'tests', 'support', 'lib_adapter.js') });
    }
    // debug 不给默认值：页面对它是 has 判断，给了 '0' 也算开。诊断面的判据要求它缺席。
    const params = new URLSearchParams({ transport: opts.transport, ...opts.query });
    const url = opts.pageBase + opts.file + '?' + params.toString();
    await page.goto(url);
    await page.waitForFunction(() => {
      const w = window as BootWindow & { __heditor?: { ready?: boolean } };
      // 垫片要等它自己把 __heditor 挂上；普通页面等到 __demoReady 就够了。
      return w.__demoReady === true || !!w.__demoStartError || !!w.__heditor?.ready;
    }, undefined, { timeout: 90_000 });
    const failed = await page.evaluate(() => (window as BootWindow).__demoStartError);
    if (failed) throw new Error('页面启动失败：' + failed);
    return new ProbePage(page, url);
  }

  /**
   * 在页面里求值，求完把宿主的待发消息推完。
   *
   * 代码片段用 return 交回结果，包进异步 IIFE 之后 await 与 return 都能直接写。
   *
   * @param body 函数体源码，按旧探针的写法
   * @returns 该片段的返回值
   */
  async ev<T = any>(body: string): Promise<T> {
    const out = (await this.page.evaluate('(async () => {\n' + body + '\n})()')) as T;
    await this.page.evaluate(DRAIN);
    return out;
  }

  /**
   * 下发一条原始 CDP 命令。
   *
   * 输入事件的形状（虚拟键码、modifiers、clickCount）是判据的一部分，
   * 换成 page.keyboard 的封装就不再是同一批事件。
   *
   * @param method 命令名，例如 Input.dispatchKeyEvent
   * @param params 命令参数
   * @returns 命令返回值
   */
  async send(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
    this.session ??= await this.page.context().newCDPSession(this.page);
    // 命令名在这里是字符串常量（协议表），CDPSession 的类型是逐命令枚举，
    //   直接放行会要求把每一种参数形状都写出来，而这一层只做转发。
    //   bind 不能省：把方法提出来单独调用会丢掉 this，报 _channel 未定义。
    const raw = this.session.send.bind(this.session) as unknown as
      (m: string, p?: Record<string, unknown>) => Promise<unknown>;
    return raw(method, params);
  }

  /**
   * 存一张截图，落在 Playwright 的产物目录里。
   *
   * @param name 文件名主干，扩展名由本方法补
   */
  async shot(name: string): Promise<void> {
    await this.page.screenshot({ path: test.info().outputPath(name + '.png'), fullPage: true });
  }
}
