// 多实例：同页两个实例（一个同线程、一个 Worker）必须互不干扰。
//
// 判据三层：独立（往 A 敲字不动 B）、隔离（A 的错误与只读不进 B）、可拆
//   （destroy(A) 之后 B 照常编辑）。页面是 multi.html，两栏并排各有自己的容器与引擎。
import { check } from '../support/check';
import { test } from '../support/probe';

// 第一节的读数。
type Boot = {
  both: boolean;
  diff: boolean;
  ta: string;
  tb: string;
  stages: number;
  canvases: number;
};

// 第二、三节的读数。
type Pair = { before: { a: string; b: string }; after: { a: string; b: string } };

// 第三节的读数。
type Isolation = {
  cfg: { a: string; b: string };
  typedA: boolean;
  typedB: boolean;
  grew: { a: number; b: number };
};

// 第四节的读数。
type TearDown = { rejected: boolean; ok: boolean; grew: boolean; emptied: boolean };

test.describe('多实例', () => {
  test('两栏互不干扰，拆掉一个另一个照常', async ({ page, open }) => {
    const errors: string[] = [];
    page.on('console', (m) => {
      if (m.type() === 'error') errors.push(m.text());
    });
    page.on('pageerror', (e) => errors.push('uncaught: ' + e.message));

    // 这一页两个引擎各自带传输，不随 project 走：A 固定同线程，B 固定 Worker。
    const p = await open('multi.html', {
      query: { debug: '1' },
      viewport: { width: 1500, height: 1000 },
    });

    await test.step('两个实例各自起来了', async () => {
      const r = await p.ev<Boot>(`
        const a = window.__demoA, b = window.__demoB;
        return {
          both: !!a && !!b,
          diff: a !== b,
          ta: a.text(), tb: b.text(),
          stages: document.querySelectorAll('#stage').length,
          canvases: document.querySelectorAll('#pages canvas').length,
        };`);
      check('两个实例都拿到了', r.both === true && r.diff === true);
      check('各自装着自己的文本', r.ta.startsWith('实例 A') && r.tb.startsWith('实例 B'),
            r.ta.slice(0, 6) + ' / ' + r.tb.slice(0, 6));
      check('两套骨架同时存在（两组 #stage、至少两张页画布）',
            r.stages === 2 && r.canvases >= 2, r.stages + ' 组 / ' + r.canvases + ' 张');
    });

    await test.step('独立：往 A 敲字不动 B', async () => {
      const r = await p.ev<Pair>(`
        const a = window.__demoA, b = window.__demoB;
        const before = { a: a.text(), b: b.text() };
        await a.setCaret({ para: 0, offset: 6 });
        await a.type('X');
        await a.flush();
        return { before, after: { a: a.text(), b: b.text() } };`);
      check('A 的文本变了',
            r.after.a === r.before.a.slice(0, 6) + 'X' + r.before.a.slice(6),
            r.before.a.length + ' 字变 ' + r.after.a.length + ' 字');
      check('B 的文本一个字都没动', r.after.b === r.before.b);
    });

    await test.step('隔离：A 只读不影响 B', async () => {
      const r = await p.ev<Isolation>(`
        const a = window.__demoA, b = window.__demoB;
        const before = { a: a.text(), b: b.text() };
        await a.configure({ mode: 'readOnly' });
        const cfg = { a: a.config().mode, b: b.config().mode };
        const typedA = await a.type('Z');
        const typedB = await b.type('W');
        await b.flush();
        return { cfg, typedA, typedB,
                 grew: { a: a.text().length - before.a.length, b: b.text().length - before.b.length } };`);
      check('A 切到只读，B 仍是文档模式',
            r.cfg.a === 'readOnly' && r.cfg.b === 'document',
            r.cfg.a + ' / ' + r.cfg.b);
      check('只读那一栏不接受编辑，另一栏照常',
            r.typedA === false && r.typedB === true && r.grew.a === 0 && r.grew.b === 1,
            'A 改动 ' + r.grew.a + ' 字 / B 改动 ' + r.grew.b + ' 字');
    });

    await test.step('可拆：destroy(A) 之后 B 照常', async () => {
      const r = await p.ev<TearDown>(`
        const a = window.__demoA, b = window.__demoB;
        const bBefore = b.text();
        await a.destroy();
        let rejected = false;
        try { a.text(); } catch (e) { rejected = true; }
        // B 在 A 被拆掉之后仍然能编辑：两个实例共享的只有字体字节与 wasm 内存那一层。
        await b.setCaret({ para: 0, offset: 6 });
        const ok = await b.type('Y');
        await b.flush();
        return { rejected, ok, grew: b.text().length === bBefore.length + 1,
                 emptied: document.getElementById('ed-a').childElementCount === 0 };`);
      check('A 被拆之后调用一律抛', r.rejected === true);
      check('A 的容器被清空', r.emptied === true);
      check('B 仍然能编辑', r.ok === true && r.grew === true);
    });

    await test.step('没有 console 错误', async () => {
      const real = errors.filter((t) => !/favicon/i.test(t));
      check('没有 console 错误', real.length === 0, real.slice(0, 2).join(' | '));
    });
  });
});
