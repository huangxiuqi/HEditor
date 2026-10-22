// 错误降级判据：失败必须变成页面上看得见的东西。
//
// 要证伪的事：引擎的失败只有错误事件与内部数组知道，而事件不一定有人监听。于是失败在
//   页面上完全看不见 —— 页面要么空白、要么停在一个看起来能输入、实际打不进字的编辑器上。
//   这里按三档取证：正常态、单次失败（降级提示且编辑器仍可用）、致命失败（常驻横幅）。
//
// 致命那一档用 glue 参数指向一个不存在的产物：这是真实会发生的配置错误，
//   不需要往产品代码里塞测试开关。
import { check } from '../support/check';
import { test } from '../support/probe';

// 第一档的读数。
type Normal = {
  ready: boolean;
  startError: string;
  entries: number;
  banner: boolean;
  toast: boolean;
  marked: number;
};

// 第二档的读数。
type Degraded = { entries: string[]; toast: boolean; banner: boolean; role: string };

// 第三档的读数。
type Fatal = {
  editor: boolean;
  startError: string;
  hasBanner: boolean;
  title: string;
  detail: string;
  hint: string;
  role: string;
  rect: [number, number] | null;
  style: string;
  retry: boolean;
  marked: string | null;
  canvases: number;
};

test.describe('错误降级', () => {
  test('正常态没有错误界面，单次失败降级，致命失败给横幅', async ({ page, open }) => {
    const errors: string[] = [];
    page.on('console', (m) => {
      if (m.type() === 'error') errors.push(m.text());
    });
    page.on('pageerror', (e) => errors.push('uncaught: ' + e.message));

    const p = await open('index.html', {
      query: { debug: '1' },
      viewport: { width: 1280, height: 900 },
    });

    await test.step('第一档：正常态', async () => {
      const r = await p.ev<Normal>(`
        const h = window.__demo;
        return { ready: !!h, startError: window.__demoStartError || '',
                 entries: h ? h.diag.errors().length : -1,
                 banner: !!document.querySelector('[data-host-error="fatal"]'),
                 toast: !!document.querySelector('[data-host-error="warn"]'),
                 marked: document.querySelectorAll('[data-host-fatal="1"]').length };`);
      check('启动成功（前置条件）', r.ready === true, JSON.stringify(r));
      check('没有致命横幅', !r.banner && r.marked === 0);
      check('没有提示条残留', !r.toast);
      check('降级记账为空（反向一半）', r.entries === 0, 'entries=' + r.entries);
      const boot = errors.filter((t) => !/favicon/i.test(t));
      check('启动过程没有 console 错误', boot.length === 0, boot.slice(0, 2).join(' | '));
    });

    await test.step('第二档：单次失败', async () => {
      const before = await p.ev<number>('return window.__demo.text().length;');
      await p.ev(`setTimeout(() => { throw new Error('error.spec 故意抛的一次失败'); }, 0);
        return true;`);
      await page.waitForTimeout(200);
      const r = await p.ev<Degraded>(`
        const h = window.__demo;
        const warn = document.querySelector('[data-host-error="warn"]');
        return { entries: h.diag.errors(),
                 toast: !!warn,
                 banner: !!document.querySelector('[data-host-error="fatal"]'),
                 role: warn ? warn.getAttribute('role') : '' };`);
      check('记下了一条降级', r.entries.length >= 1,
            'entries=' + r.entries.length + '：' + (r.entries[0] || '').slice(0, 60));
      check('没有升级成致命', !r.banner);
      check('页面上有提示条', r.toast === true, 'DOM 里应存在 [data-host-error="warn"]');
      check('提示条是无障碍可见的（role=status）', r.role === 'status', r.role);

      // 反向一半：失败之后编辑器必须还能用。只看"没有致命"不够 ——
      //   引擎可能已经失效而没有任何标志位反映出来。
      const alive = await p.ev<{ pages: number; len: number }>(`
        const h = window.__demo;
        await h.type('活');
        return { pages: h.geometry().pages, len: h.text().length };`);
      check('失败之后编辑器仍然可用', alive.pages >= 1 && alive.len === before + 1,
            '页 ' + alive.pages + '，字数 ' + before + ' 变 ' + alive.len);
    });

    await test.step('第三档：致命失败', async () => {
      const bad = new URL(p.url, 'http://x');
      bad.searchParams.set('glue', 'build/__missing__/web/heditor.js');
      await page.goto(bad.pathname + bad.search);
      // 等"启动失败"落定：这里等的是 createEditor 拒绝之后的那份横幅。
      await page.waitForFunction(() => {
        const w = window as Window & { __demoStartError?: string };
        return !!w.__demoStartError || !!document.querySelector('[data-host-error="fatal"]');
      }, undefined, { timeout: 20_000 });
      await page.waitForTimeout(300);
      const r = await p.ev<Fatal>(`
        const el = document.querySelector('[data-host-error="fatal"]');
        const box = el ? el.getBoundingClientRect() : null;
        const host = document.getElementById('ed');
        return { editor: !!window.__demo,
                 startError: window.__demoStartError || '',
                 hasBanner: !!el,
                 title: el ? el.querySelector('.he-t').textContent : '',
                 detail: el ? el.querySelector('.he-d').textContent : '',
                 hint: el ? el.querySelector('.he-h').textContent : '',
                 role: el ? el.getAttribute('role') : '',
                 rect: box ? [Math.round(box.width), Math.round(box.height)] : null,
                 style: el ? getComputedStyle(el).display : '',
                 retry: !!document.querySelector('[data-host-error="fatal"] button[data-he-retry]'),
                 marked: host ? host.getAttribute('data-host-fatal') : null,
                 canvases: document.querySelectorAll('#pages canvas').length };`);
      check('引擎确实没起来（前置条件）', r.editor === false && !!r.startError,
            'editor=' + r.editor + ' startError=' + String(r.startError).slice(0, 60));
      check('出现致命横幅', r.hasBanner === true);
      check('横幅在 DOM 里且真的可见',
            !!r.rect && r.rect[0] > 0 && r.rect[1] > 0 && r.style !== 'none',
            'rect=' + JSON.stringify(r.rect) + ' display=' + r.style);
      check('横幅是无障碍可见的（role=alert）', r.role === 'alert', r.role);
      check('横幅带失败原因', r.detail.length > 0, r.detail.slice(0, 60));
      check('横幅带下一步（提示）', r.hint.length > 0, r.hint.slice(0, 40));
      check('容器被标成不可用', r.marked === '1', String(r.marked));
      check('给了恢复入口（重试按钮）', r.retry === true);
      check('不再假装有内容（一页画布都没有）', r.canvases === 0, r.canvases + ' 张');
    });
  });
});
