// 剪贴板：复制粘贴在同一个实例里要保真（文字、段落、控件、表格），跨实例也要保真；
// 从别的应用粘过来的纯文本按行拆段。
//
// 判据只看文档内容与控件集合，不比像素：剪贴板是内容层的功能，帧上的差异由别的 spec 管。
// 载荷来自一次 Worker 往返；复制出来的结构必须与源一致。
import { check } from '../support/check';
import { test } from '../support/probe';

// 一段选区复制粘贴之后的可读结果。
type PasteResult = { before: string; after: string; controls: number; pasted: number };

// 页面里的公共入口（示例页把它挂在 window.__demo 上）。
const D = 'window.__demo';

test.describe('剪贴板', () => {
  test.beforeEach(async ({ page }) => {
    // 程序化复制粘贴要读写系统剪贴板：键盘那条不需要授权，但 spec 里两条都要覆盖。
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  });

  test('复制一段文字再粘贴：内容与段落结构都回来', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<PasteResult>(`
      const e = ${D};
      await e.setText('第一段\\n第二段');
      // 选中第一段的前两个字，复制，再把光标摆到第二段末尾粘上。
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const copied = await e.copy();
      await e.setCaret({ para: 1, offset: 3 });
      const pasted = await e.paste();
      await e.flush();
      return {
        before: copied ? 'ok' : 'fail',
        after: e.paragraphs().join('|'),
        controls: e.form().length,
        pasted: pasted ? 1 : 0,
      };`);
    check('复制返回成功', r.before === 'ok', r.before);
    check('粘贴返回成功', r.pasted === 1, String(r.pasted));
    check('第二段末尾多了被复制的那两个字', r.after === '第一段|第二段第一', r.after);
    check('控件数量没变（纯文字粘贴不该造出控件）', r.controls === 0, String(r.controls));
  });

  test('编辑器内复制粘贴：样式跟着回来，段数不变', async ({ open }) => {
    // 浏览器不保证自定义类型穿过剪贴板（paste 事件只给标准类型、clipboard.read 没授权
    //   时整条拒绝），于是"复制带样式的一段、粘回自己"会退化成纯文本 —— 样式没了，
    //   末段还多出一行空段。这条判据钉的是"编辑器内往返不依赖权限"。
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = window.__demo;
      await e.setText('甲乙\\n丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.format({ bold: true, size: 20 });
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const copied = await e.copy();
      await new Promise((r) => setTimeout(r, 150));
      const before = { paras: e.paragraphs().length, text: e.text() };
      await e.setCaret({ para: 1, offset: 2 });
      const pasted = await e.paste();
      await e.flush();
      await e.select({ para: 1, offset: 2 }, { para: 1, offset: 3 });
      await e.flush();
      const eff = await e.formatAt({ effective: true });
      return { copied, pasted, before, after: { paras: e.paragraphs().length, text: e.text() }, eff };`);
    check('复制成功', r.copied === true, String(r.copied));
    check('粘贴成功（读不到剪贴板也不该失败）', r.pasted === true, String(r.pasted));
    check('★ 粘进来的字还带粗体', r.eff.bold === true, JSON.stringify(r.eff));
    check('★ 粘进来的字还是 20 磅', r.eff.size === 20, String(r.eff.size));
    check('★ 段数不变（没有多出一行空段）', r.after.paras === r.before.paras,
          JSON.stringify([r.before.paras, r.after.paras]));
  });


  test('空选区复制：返回 false，剪贴板内容不变', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ first: boolean; second: boolean; text: string }>(`
      const e = ${D};
      await e.setText('甲乙丙');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const first = await e.copy();
      // 取消选区之后再复制：剪贴板里应当还是上一次那份内容，而不是被清空。
      await e.setCaret({ para: 0, offset: 1 });
      await e.clearSelection();
      const second = await e.copy();
      await e.setCaret({ para: 0, offset: 3 });
      await e.paste();
      await e.flush();
      return { first, second, text: e.text() };`);
    check('有选区时复制成功', r.first, String(r.first));
    check('空选区复制返回 false', !r.second, String(r.second));
    check('空选区复制没有破坏剪贴板（粘回来的仍是上一次那份）', r.text === '甲乙丙甲乙', r.text);
  });

  test('整段复制：段落标记一起过来（段数增加）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ paras: string[]; count: number }>(`
      const e = ${D};
      await e.setText('甲\\n乙');
      // 从第一段开头选到第二段开头：区间里含着第一段的段落标记。
      await e.select({ para: 0, offset: 0 }, { para: 1, offset: 0 });
      await e.copy();
      await e.setCaret({ para: 1, offset: 1 });
      await e.paste();
      await e.flush();
      return { paras: e.paragraphs(), count: e.paragraphs().length };`);
    check('段数从 2 变成 3（段落标记跟着复制过来了）', r.count === 3, JSON.stringify(r.paras));
    check('插进来的那一段在第一段之前', r.paras[0] === '甲', JSON.stringify(r.paras));
  });

  test('控件保真：复制带控件的整篇，粘回来控件还在', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ before: number; after: number; text: string; labels: string[] }>(`
      const e = ${D};
      await e.diag.loadFormSample(0);
      await e.flush();
      const before = e.form().length;
      await e.selectAll();
      const copied = await e.copy();
      // 换一份空文档再粘：跨文档（也是跨实例内容）的往返。
      //   粘贴目标切回编辑模式：表单模式下"没有可编辑控件"时整篇拒绝编辑（策略，不是保真问题）。
      await e.setText('');
      await e.configure({ mode: 'document' });
      await e.paste();
      await e.flush();
      const after = e.form().length;
      return {
        before,
        after,
        text: e.text(),
        labels: copied ? e.form().map((c) => c.label) : [],
      };`);
    check('前提：示例里有控件', r.before > 0, String(r.before));
    check('★ 粘回来之后控件数量不变', r.after === r.before, r.before + ' → ' + r.after);
    check('文字也回来了（非空）', r.text.length > 0, r.text.slice(0, 20));
  });

  test('外部纯文本：按行拆成段落', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ paras: string[] }>(`
      const e = ${D};
      await e.setText('起点');
      // 模拟别的应用写进剪贴板的纯文本：没有自家格式，只有 text/plain。
      await navigator.clipboard.writeText('外部第一行\\n外部第二行');
      await e.setCaret({ para: 0, offset: 2 });
      const ok = await e.paste();
      await e.flush();
      return { paras: ok ? e.paragraphs() : ['粘贴失败'] };`);
    check('两行文本粘成两段', r.paras.length === 2, JSON.stringify(r.paras));
    check('第一行接在原文后面', r.paras[0] === '起点外部第一行', JSON.stringify(r.paras));
    check('第二行是独立的一段', r.paras[1] === '外部第二行', JSON.stringify(r.paras));
  });

  test('粘贴之后撤销：一次撤销回到粘贴之前', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ before: string; after: string; undone: string }>(`
      const e = ${D};
      await e.setText('甲乙\\n丙');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.copy();
      await e.setCaret({ para: 1, offset: 1 });
      const before = e.paragraphs().join('|');
      await e.paste();
      await e.flush();
      const after = e.paragraphs().join('|');
      await e.undo();
      await e.flush();
      return { before, after, undone: e.paragraphs().join('|') };`);
    check('粘贴确实改了文档', r.after !== r.before, r.before + ' → ' + r.after);
    check('★ 一次撤销就回到粘贴之前（粘贴是一条事务）', r.undone === r.before,
          r.undone + ' vs ' + r.before);
  });

  test('键盘 Ctrl+C / Ctrl+V：走的是产品里真正的那条路', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    await p.ev(`
      const e = ${D};
      await e.setText('键盘复制');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      return true;`);
    // 焦点交给承接层：copy / cut / paste 三个事件只挂在它身上。
    await p.page.locator('.heditor-ime').focus();
    await p.page.keyboard.press('Control+c');
    // Worker 传输的复制是一次往返，写剪贴板在它回来之后 —— 等一小会儿再粘。
    await p.page.waitForTimeout(300);
    await p.ev(`
      const e = ${D};
      await e.setCaret({ para: 0, offset: 4 });
      return true;`);
    await p.page.locator('.heditor-ime').focus();
    await p.page.keyboard.press('Control+v');
    await p.page.waitForTimeout(300);
    const after = await p.ev<string>(`return ${D}.text();`);
    check('键盘复制粘贴得到同一段文字（事件路径接上了）', after === '键盘复制键盘', after);
  });

  test('剪切：内容进了剪贴板，文档里也没了', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ cut: boolean; afterCut: string; afterPaste: string }>(`
      const e = ${D};
      await e.setText('甲乙丙');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const cut = await e.cut();
      await e.flush();
      const afterCut = e.text();
      await e.setCaret({ para: 0, offset: 1 });
      await e.paste();
      await e.flush();
      return { cut, afterCut, afterPaste: e.text() };`);
    check('剪切返回成功', r.cut, String(r.cut));
    check('剪掉之后文档里少了那两个字', r.afterCut === '丙', r.afterCut);
    check('★ 剪掉的内容还能粘回来', r.afterPaste === '丙甲乙', r.afterPaste);
  });

  test('连续粘贴把文档撑长：窗口与画布跟着走，末尾不留空白', async ({ open }) => {
    // 这条判的是"滚动位置、引擎窗口、页画布"三者会不会脱节。三者的更新分属三拍
    //   （滚动发生在宿主、换窗发生在引擎、建画布发生在收尾），任何一拍落后就会
    //   表现为"容器高度一直涨、末尾那一段永远没有画布、粘进来的字看不见"。
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<{ bad: string[]; pages: number; grew: boolean }>(`
      const e = window.__demo;
      const src = [];
      for (let i = 0; i < 300; i++) src.push('第' + (i + 1) + '段的文字内容，用来把文档撑长。');
      await e.setText(src.join('\\n'));
      await e.selectAll();
      await e.copy();
      await e.setCaret({ para: 299, offset: 4 });
      const before = e.geometry().pages;
      const bad = [];
      for (let k = 0; k < 4; ++k) {
        await e.paste();
        await e.flush();
        const g = e.geometry();
        const stage = e.element.querySelector('.heditor-stage');
        const step = g.page.h + g.gap;
        const top = stage.scrollTop;
        const first = Math.floor(top / step);
        const last = Math.floor((top + stage.clientHeight - 1) / step);
        const have = new Set([...e.element.querySelectorAll('.heditor-pages canvas')]
          .filter((c) => c.dataset.page !== undefined)
          .map((c) => Number(c.dataset.page)));
        for (let q = first; q <= last; ++q) if (!have.has(q)) bad.push(k + ':无画布' + q);
        const cp = e.caret().page;
        if (cp < g.window.first || cp >= g.window.first + g.window.count) {
          bad.push(k + ':光标在窗口外' + cp + '/' + g.window.first + '+' + g.window.count);
        }
      }
      return { bad, pages: e.geometry().pages, grew: e.geometry().pages > before };`);
    check('判据有效：文档被撑到了多页', r.grew && r.pages >= 24, String(r.pages));
    check('★ 每粘一次，可见页都有画布、光标都落在窗口里', r.bad.length === 0, r.bad.join(', '));
  });

  test('粘贴撑长之后：往下滚不弹回，可见页都有画布也有墨', async ({ open, page }) => {
    // 这条判的是宿主的两件事，都是"画布参与了文档流"才会出错的：
    //   一、换窗时 remove / appendChild 改变流的高度，浏览器做滚动锚定把 scrollTop
    //     拽回去 —— 症状是"滚轮往下滚自己弹回来，只能拖着滚动条下去"；
    //   二、画布只有窗口覆盖的那几页，滚动后新进视口的页必须现建现贴。
    const p = await open('index.html', { query: { debug: '1' } });
    await p.ev(`
      const e = window.__demo;
      await e.setText('起点');
      // 一段文字（没有换行）反复粘 —— 就是"复制一段文字然后按住 Ctrl+V"的样子。
      await navigator.clipboard.writeText('这是一段用来测试连续粘贴的文字内容'.repeat(40));
      await e.setCaret({ para: 0, offset: 2 });
      for (let k = 0; k < 40; ++k) { await e.paste(); await e.flush(); }
      return true;`);
    const box = await page.locator('.heditor-stage').boundingBox();
    if (!box) throw new Error('没有 .heditor-stage');
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    const tops: number[] = [];
    for (let i = 0; i < 6; ++i) {
      await page.mouse.wheel(0, 500);
      await page.waitForTimeout(140);
      tops.push(await p.ev<number>(`return Math.round(document.querySelector('.heditor-stage').scrollTop);`));
    }
    const state = await p.read<any>(`
      const e = window.__demo;
      const st = e.element.querySelector('.heditor-stage');
      const pages = e.element.querySelector('.heditor-pages');
      const g = e.geometry();
      const step = g.page.h + g.gap;
      // 页容器在滚动内容里带着外边距：文档坐标 = scrollTop 减去它在内容里的偏移
      //   （宿主那一侧由 docScrollTop 做同一件事，判据必须用同一个折算）。
      const top = st.scrollTop - pages.offsetTop;
      const visFirst = Math.max(0, Math.floor(top / step));
      const visLast = Math.max(0, Math.floor((top + st.clientHeight - 1) / step));
      const byPage = new Map();
      for (const el of e.element.querySelectorAll('.heditor-pages canvas')) {
        if (el.dataset.page !== undefined) byPage.set(Number(el.dataset.page), el);
      }
      const absent = [], blank = [];
      for (let q = visFirst; q <= visLast; ++q) {
        const el = byPage.get(q);
        if (!el) { absent.push(q); continue; }
        const c = document.createElement('canvas');
        c.width = 60; c.height = 60;
        const g2 = c.getContext('2d');
        g2.drawImage(el, 0, 0, 60, 60);
        const d = g2.getImageData(0, 0, 60, 60).data;
        let ink = 0;
        for (let k = 0; k < d.length; k += 4) if (d[k] < 200) ++ink;
        if (ink === 0) blank.push(q);
      }
      return { absent, blank, pages: g.pages, win: g.window.first + ':' + g.window.count,
               vis: visFirst + '-' + visLast };`);
    const drop = tops.filter((v, i) => i > 0 && v < tops[i - 1]);
    check('判据有效：文档确实被撑到了多页', state.pages >= 8, JSON.stringify(state));
    check('★ 滚轮往下滚，scrollTop 只增不减（没有回弹）', drop.length === 0, JSON.stringify(tops));
    check('★ 滚动之后可见页都有画布', state.absent.length === 0, JSON.stringify(state));
    check('★ 可见页的画布上真的有墨（不是白纸）', state.blank.length === 0, JSON.stringify(state));
  });

  test('跨实例：多实例页上 A 复制、B 粘贴', async ({ open, pageBase }) => {
    const p = await open('multi.html');
    const r = await p.ev<{ a: string; b: string; ok: boolean }>(`
      const a = window.__demoA, b = window.__demoB;
      await a.setText('实例甲的文字');
      await a.select({ para: 0, offset: 0 }, { para: 0, offset: 5 });
      const ok = await a.copy();
      await b.setText('实例乙：');
      await b.setCaret({ para: 0, offset: 4 });
      await b.paste();
      await a.flush();
      await b.flush();
      return { a: a.text(), b: b.text(), ok };`);
    check('A 复制成功', r.ok, String(r.ok));
    check('★ B 粘到了 A 的那一段内容（跨实例）', r.b === '实例乙：实例甲的文', r.b);
    check('A 自己的文档没被改动', r.a === '实例甲的文字', r.a);

    // 跨实例也要带样式：片段不能只靠"最近一份载荷"（那只活在一个页面里），它还得随
    //   text/html 一起走 —— 这一条直接验那条载体（片段藏进 HTML，再原样取回来）。
    //
    //   这两段问的是内部模块：库的入口刻意不导出它，构建产物里它只在带哈希的 chunk 里，
    //   按名字 import 不到。所以只有源码直连口径拿得到读数；上面那几条跨实例行为两种
    //   口径都跑。
    if (pageBase.includes('dist-app')) {
      console.log('（构建产物口径拿不到 host/clipboard 的内部模块，跳过两段载体读数）');
    } else {
      const carrier = await p.ev<{ same: boolean; hasAttr: boolean }>(`
        const m = await import('/src/host/clipboard.ts');
        const frag = '{"v":3,"n":[{"k":"t","t":"甲乙"}]}';
        const html = m.embedFragment('<p>甲乙</p>', frag);
        return { same: m.fragmentInHtml(html) === frag, hasAttr: html.includes('data-heditor-fragment') };`);
      check('★ 片段能藏进 text/html 并原样取回（跨浏览器靠它）', carrier.same === true,
            JSON.stringify(carrier));
      check('HTML 里确实带了那个属性', carrier.hasAttr === true, String(carrier.hasAttr));

      // 缓存只认"本实例写的那一份"：文本相等但来源不同（别处复制来同样的字）不该用缓存 ——
      //   否则拿旧格式覆盖新内容。判据直接问那个模块。
      const originRule = await p.ev<any>(`
        const m = await import('/src/host/clipboard.ts');
        const dt = new DataTransfer();
        m.writeToDataTransfer(dt, { fragment: '{"v":3}', text: '甲乙', html: '<p>甲乙</p>',
                                    origin: 'A' });
        return { same: m.recallPayload('甲乙', 'A') !== null,
                 other: m.recallPayload('甲乙', 'B') === null,
                 latestSame: m.latestPayload('A') !== null,
                 latestOther: m.latestPayload('B') === null };`);
      check('★ 同源：缓存命中', originRule.same === true, JSON.stringify(originRule));
      check('★ 异源：文本相同也不认（不拿旧格式覆盖新内容）', originRule.other === true,
            JSON.stringify(originRule));
    }
  });
});
