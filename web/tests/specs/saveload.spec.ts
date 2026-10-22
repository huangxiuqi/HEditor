// 保存与打开：存盘、改一笔、再打开，正文与帧指纹要回到存盘那一刻。
//
// 指纹含瞬态（光标、选区），所以每次取样前都把光标钉在同一处并停表。
// 这一份跑在示例页上，两条传输各跑一遍。
import { check } from '../support/check';
import { test } from '../support/probe';

// 取样的三元组：帧指纹、正文、页数。
type Snapshot = { digest: string; text: string; pages: number };

// 打开之后是否还能编辑。
type Usable = { before: number; typed: number; ok: boolean; undone: number };

// 坏文件的两种表现。
type Rejection = { threw: boolean; same: boolean };

// 取样前先停表并把光标钉住：闪烁相位与光标位置都会进指纹。
const SNAP = `
  window.__demo.diag.stopBlink();
  await window.__demo.setCaret({ para: 0, offset: 0 });
  window.__demo.diag.present();
  await window.__demo.flush();
  return {
    digest: await window.__demo.diag.frameDigest(),
    text: window.__demo.text(),
    pages: window.__demo.geometry().pages,
  };`;

test.describe('保存与打开', () => {
  test('存盘往返：正文、帧指纹、页数都回到存盘那一刻', async ({ open }) => {
    const p = await open('index.html', {
      query: { debug: '1' },
      viewport: { width: 1280, height: 900 },
    });

    let saved = '';
    let stored: Snapshot = { digest: '', text: '', pages: 0 };

    await test.step('存出来的是一份文档', async () => {
      stored = await p.ev<Snapshot>(`
        const e = window.__demo, d = e.diag;
        await d.reset(6, 1.5);
        await e.type('保存');
        await e.type('测试');
        ${SNAP}`);
      saved = await p.ev<string>(`
        const e = window.__demo;
        return await e.save();`);
      check('保存返回非空 JSON 对象',
            typeof saved === 'string' && saved.length > 100 && saved.trimStart().startsWith('{'),
            saved.length + ' 字节');
      check('文字按 UTF-8 写进文件（不是码点数组）',
            saved.includes('"t":"') && !saved.includes('"t":['),
            saved.includes('"t":[') ? '仍是数组' : '字符串');
      check('字体表里有真名字（PostScript 名 + 文件名）',
            /"ps":"[^"]+"/.test(saved) && /"file":"[^"]+\.ttf"/.test(saved),
            (saved.match(/"fonts":\{[\s\S]{0,160}/) || ['没有 fonts'])[0].slice(0, 120));
    });

    await test.step('存盘、改一笔、再打开', async () => {
      const mutated = await p.ev<Snapshot>(`
        const e = window.__demo;
        await e.type('污染');
        ${SNAP}`);
      check('改过之后正文确实变了（前置条件，否则下面那条恒真）',
            mutated.text !== stored.text, stored.text.length + ' 字变 ' + mutated.text.length + ' 字');
      const reopened = await p.ev<Snapshot>(`
        const e = window.__demo;
        await e.open(${JSON.stringify(saved)});
        ${SNAP}`);
      check('打开之后正文与存盘那一刻逐字相同', reopened.text === stored.text,
            reopened.text.length + ' / ' + stored.text.length + ' 字');
      check('打开之后帧指纹与存盘那一刻相同（含光标与选区）',
            reopened.digest === stored.digest, stored.digest + ' 变 ' + reopened.digest);
      check('打开之后页数也回到存盘那一刻', reopened.pages === stored.pages,
            stored.pages + ' 页变 ' + reopened.pages + ' 页');
    });

    await test.step('打开之后编辑器仍然可用', async () => {
      const r = await p.ev<Usable>(`
        const e = window.__demo;
        const before = e.text().length;
        const ok = await e.type('继续');
        const typed = e.text().length;
        await e.undo();
        return { before, typed, ok, undone: e.text().length };`);
      check('打开之后还能打字', r.ok === true && r.typed === r.before + 2,
            r.before + ' 字变 ' + r.typed + ' 字');
      check('打开之后的编辑可以撤销', r.undone === r.before, r.typed + ' 字变 ' + r.undone + ' 字');
    });

    await test.step('坏文件：拒绝并且不动当前文档', async () => {
      const r = await p.ev<Rejection>(`
        const e = window.__demo;
        const before = e.text();
        let threw = false;
        try { await e.open('{"v":1,"blocks":[{"nope":true}]}'); } catch (err) { threw = true; }
        return { threw, same: e.text() === before };`);
      check('坏 JSON 打开失败并抛错', r.threw === true);
      check('坏文件没有污染当前文档（失败即不生效）', r.same === true);
    });
  });
});
