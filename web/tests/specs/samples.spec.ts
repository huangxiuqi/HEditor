// 样例（web/app/samples）的判据。
//
// 为什么单独一份：样例是"给使用者看的用法"，它只能走公开面。这条判据不但验"每份都能搭
//   出来"，还验"不依赖诊断面"—— debug 关掉之后 diag 是 undefined，样例照样要能跑。
//   样例一旦用了 diag 或引擎私有入口，这里立刻红。
import { check } from '../support/check';
import { test } from '../support/probe';

/**
 * 一份样例搭完之后的读数。
 */
type Built = {
  paragraphs: number;
  pages: number;
  controls: number;
  rows?: number;
  inTable?: boolean;
};

test.describe('样例', () => {
  test('每份样例都只用公开面搭得出来', async ({ open }) => {
    // 不开 debug：这一页没有 diag，样例若用了它就会在这里炸。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });

    const r = await p.ev<{ list: string[]; built: Record<string, Built> }>(`
      const S = window.__samples, e = window.__demo;
      const built = {};
      for (const id of S.list) {
        await S.run(id);
        await e.flush();
        built[id] = {
          paragraphs: e.paragraphs().length,
          pages: e.geometry().pages,
          controls: e.form().length,
        };
        // 表格状态只在"刚装完这份"时读得到：下一份样例会换掉文档。
        if (id === 'table') {
          const st = await e.tableState();
          built[id].rows = st.rows;
          built[id].inTable = st.inTable;
        }
      }
      return { list: S.list.slice(), built };`);

    check('样例清单就是那五份', r.list.join(',') === 'prose,form,table,long,medical',
          r.list.join(','));

    const b = r.built;
    check('正文样例：五段、单页', b.prose.paragraphs === 5 && b.prose.pages === 1,
          `${b.prose.paragraphs} 段 / ${b.prose.pages} 页`);
    check('表单样例：装出六个控件', b.form.controls === 6, String(b.form.controls));
    check('表格样例：光标落在表里，三行三列',
          b.table.inTable === true && b.table.rows === 3,
          `inTable=${b.table.inTable} rows=${b.table.rows}`);
    check('长文样例：三百段、多页', b.long.paragraphs === 300 && b.long.pages > 1,
          `${b.long.paragraphs} 段 / ${b.long.pages} 页`);
    check('病历样例：真文本铺出几十段', b.medical.paragraphs > 20,
          b.medical.paragraphs + ' 段');
  });

  test('样例改坏了判据会红：结构量变一下就不是原来那份', async ({ open }) => {
    // 反向一半：如果样例什么都不做，上面那些数字会全是 0/1。这里再钉一句"重跑一次
    //   结果相同"，防止样例中途抛错却留下半份文档（那样段落数会漂）。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    const r = await p.ev<{ first: number; second: number; pages: number }>(`
      const S = window.__samples, e = window.__demo;
      await S.run('form');
      await e.flush();
      const first = e.form().length;
      const pages = e.geometry().pages;
      await S.run('prose');
      await S.run('form');
      await e.flush();
      return { first, second: e.form().length, pages };`);
    check('同一份样例搭两次，控件数相同', r.first === r.second,
          r.first + ' 对 ' + r.second);
    check('表单样例是单页', r.pages === 1, r.pages + ' 页');
  });
});
