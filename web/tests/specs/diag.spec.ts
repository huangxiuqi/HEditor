// 诊断面（diag）的运行时判据。
//
// 为什么单独一份：demo.spec 只证明"公开面只有公开面"，diag 在 debug 未开时抛
//   TypeError，平时一条都不会被调用。类型对上不代表命令接通：diag 的每个成员都是
//   "读一个命令 / 写一个命令 + 补收尾"，接错一个只会让探针读到 undefined。
//
// 十节逐条对应旧 diag_probe：光标与相位、字体与窗口、错误历史、页画布可见性、
//   文档大小与字形数、SIMD 探测、多页点击、光标闪烁、console 错误。
// 装载不在这里：文档由样例（公开方法）或夹具（open）装，见 samples.spec。
import { check } from '../support/check';
import { test } from '../support/probe';

test.describe('诊断面', () => {
  test('debug 打开时 diag 每个成员都接通', async ({ page, open }) => {
    const errors: string[] = [];
    page.on('console', (m) => {
      if (m.type() === 'error') errors.push(m.text());
    });
    page.on('pageerror', (e) => errors.push('uncaught: ' + e.message));

    const p = await open('index.html', {
      query: { debug: '1' },
      viewport: { width: 1280, height: 900 },
    });

    await test.step('一、diag 只在 debug 下存在', async () => {
      const r = await p.ev<{ has: boolean; keys: number }>(`
        const e = window.__demo;
        return { has: !!e.diag, keys: Object.keys(e.diag || {}).length };`);
      check('debug: true 时 diag 可读', r.has === true);
      check('diag 成员数 > 40', r.keys > 40, String(r.keys));
    });

    await test.step('二、光标与相位', async () => {
      const r = await p.ev<{ at: { para: number; offset: number }; live: number }>(`
        const d = window.__demo.diag;
        // 相位与呈现要在"文档模式、光标在正文上"的前提里验：表单模式下光标只出现在
        //   可编辑控件里，落在正文上时它本来就不画。
        await window.__demo.configure({ mode: 'document' });
        await window.__demo.open(
          await (await fetch('/assets/samples/fixtures/scenario-6.json')).text());
        await window.__demo.configure({ zoom: 1.0 });
        await window.__demo.flush();
        await d.setCaretAt(0, 2, 0);
        const at = window.__demo.caret().pos;
        // 只改位置不呈现：真源在引擎里，读 caretLive（直读），不读 editStats（上一拍的账）。
        d.setCaretOnly(0, 5);
        await new Promise((r) => setTimeout(r, 30));
        return { at, live: d.caretLive() };`);
      check('setCaretAt 落到段 0 第 2 格', r.at.para === 0 && r.at.offset === 2,
            JSON.stringify(r.at));
      check('setCaretOnly 改了引擎里的位置', r.live === 5, String(r.live));

      const phase = await p.ev<{ off: boolean; on: boolean }>(`
        const d = window.__demo.diag;
        d.stopBlink();
        d.showCaret(false);
        await d.present();
        await new Promise((r) => setTimeout(r, 60));
        const off = window.__demo.caret().visible;
        d.showCaret(true);
        await d.present();
        await new Promise((r) => setTimeout(r, 60));
        const on = window.__demo.caret().visible;
        d.startBlink();
        d.stopBlink();
        return { off, on };`);
      // 相位是引擎状态，只改它不会自己上屏 —— 这里一并验证 present 之后读数跟着变。
      check('showCaret(false) 后呈现，光标不可见', phase.off === false, String(phase.off));
      check('showCaret(true) 后呈现，光标又出现', phase.on === true, String(phase.on));
      check('startBlink / stopBlink 不抛', true);
    });

    await test.step('三、字体与窗口', async () => {
      const r = await p.ev<{ lazy: { mode: string }; full: { mode: string }; anchor: unknown }>(`
        const d = window.__demo.diag;
        const lazy = await d.reloadFonts('lazy');
        const full = await d.reloadFonts('full');
        d.setFontCacheConfig(65536, 4);
        const anchor = await d.setViewportAnchor(0, 2);
        return { lazy, full, anchor };`);
      check('reloadFonts(lazy) 报告 lazy 模式', r.lazy && r.lazy.mode === 'lazy',
            JSON.stringify(r.lazy));
      check('reloadFonts(full) 报告 full 模式', r.full && r.full.mode === 'full',
            JSON.stringify(r.full));
      check('setViewportAnchor 返回那一拍的账',
            !!r.anchor && typeof r.anchor === 'object', JSON.stringify(r.anchor).slice(0, 40));
    });

    await test.step('四、错误历史与既有诊断', async () => {
      const r = await p.ev<{
        isArr: boolean; n: number; diff: number; sel: boolean; compared: number;
        diffPx: number; px: { w: number; h: number }; hasBrackets: boolean;
      }>(`
        const d = window.__demo.diag;
        const errs = d.errors();
        const au = await d.auditFrame();
        // 必须 await：Worker 传输下它是一次往返，不 await 拿到的是 Promise ——
        //   而 Promise 也是 object，于是"返回取证对象"这条会以空值恒真。
        const sc = await d.selfCheck();
        return { isArr: Array.isArray(errs), n: errs.length, diff: au && au.diff,
                 sel: !!sc && sc.compared > 0, compared: sc && sc.compared,
                 diffPx: sc && sc.screenVsEngineDiffPx, px: d.pagePx(),
                 hasBrackets: d.bracketSlots() !== undefined };`);
      check('errors() 是数组', r.isArr === true, String(r.n));
      // auditFrame 的载荷是 { diff, box }：diff = 0 表示"帧与从零重画逐字节相同"。
      check('auditFrame 自审通过（diff = 0）', r.diff === 0, String(r.diff));
      // compared > 0 是这一条的反向一半：没有它，缺画布时 diff 恒 0 也算过。
      check('selfCheck 真的比了像素且屏幕与引擎一致',
            r.sel === true && r.diffPx === 0, 'compared=' + r.compared + '，差 ' + r.diffPx + ' px');
      check('pagePx 是页度量', !!r.px && r.px.w > 0 && r.px.h > 0, JSON.stringify(r.px));
      check('bracketSlots 可读', r.hasBrackets === true);
    });

    await test.step('五、页画布：一页一张、都在窗口里、都真的可见', async () => {
      const r = await p.ev<{
        n: number; want: number; pages: number[]; hidden: number[]; first: number;
      }>(`
        const d = window.__demo.diag;
        await window.__demo.open(
          await (await fetch('/assets/samples/fixtures/scenario-6.json')).text());
        await window.__demo.configure({ zoom: 1.0 });
        // 回到首页：前面几节可能把视口滚走过，而"画布在屏幕上可见"要以首页居中为前提。
        await window.__demo.scrollTo({ page: 0 });
        await window.__demo.flush();
        const g = window.__demo.geometry();
        // Worker 传输按槽复用（槽数固定，窗口之外的槽挂 display:none），所以这里只看
        //   此刻真的在显示画布 —— 槽数是实现细节，页号才算数。
        const els = d.pageCanvasList().filter((el) => el.style.display !== 'none');
        const pages = els.map((el) => Number(el.dataset.page)).sort((a, b) => a - b);
        // 每张画布的中心点命中的必须还是它自己：容器被裁掉、被别的层盖住时这一条会红。
        const hidden = [];
        for (const el of els) {
          const q = el.getBoundingClientRect();
          const hit = document.elementFromPoint(q.left + q.width / 2, q.top + q.height / 2);
          if (hit !== el) hidden.push(Number(el.dataset.page));
        }
        return { n: els.length, want: g.window.count, pages, hidden, first: g.window.first };`);
      check('页画布张数等于窗口页数', r.n === r.want, r.n + ' / ' + r.want);
      check('每张画布都标着页号，且从窗口首页开始',
            r.pages.every((v, i) => v === r.first + i), JSON.stringify(r.pages));
      check('每张画布在屏幕上都真的可见（没被裁掉、没被盖住）',
            r.hidden.length === 0, '看不见的页：' + JSON.stringify(r.hidden));
    });

    await test.step('六、换文档：页数与字形数跟着变', async () => {
      const r = await p.ev<{ id: string; pages: number; glyphs: number; paras: number }[]>(`
        const d = window.__demo.diag;
        const rows = [];
        for (const id of ['prose', 'long']) {
          await window.__samples.run(id);
          await window.__demo.configure({ zoom: 1.0 });
          const st = window.__demo.diag.editStats();
          const lay = d.stats();
          rows.push({ id, pages: st.totalPages, glyphs: lay.glyphCount, paras: st.totalParagraphs });
        }
        return rows;`);
      // 短样例与长样例：页数与字形数都必须跟着变（恒等说明装载没真的换掉文档）。
      check('短样例是单页', r[0].pages >= 1 && r[0].paras <= 10,
            r[0].pages + ' 页 / ' + r[0].paras + ' 段');
      check('长样例是多页长文', r[1].pages > 1 && r[1].paras > 200,
            r[1].pages + ' 页 / ' + r[1].paras + ' 段');
      check('字形数是正数且随文档变', r[0].glyphs > 0 && r[1].glyphs > r[0].glyphs,
            r[0].glyphs + ' 变 ' + r[1].glyphs);
    });

    await test.step('七、能力探测：WebAssembly SIMD', async () => {
      const r = await p.ev<{ supported: boolean; valid: boolean; len: number; hasSimdOp: boolean }>(`
        const d = window.__demo.diag;
        const s = d.simd();
        const bytes = new Uint8Array(s.probe);
        // 判据自己跑一次 validate，并确认里面真的有一条 SIMD 指令（0xFD 前缀）——
        //   只信 supported 的话，"探针被常量折叠成空壳"与"环境真支持"分不开。
        let valid = false;
        try { valid = WebAssembly.validate(bytes) === true; } catch (e) { valid = false; }
        return { supported: s.supported, valid, len: bytes.length,
                 hasSimdOp: bytes.includes(0xFD) };`);
      check('探测结论是支持，且探针字节自己 validate 也是 true',
            r.supported === true && r.valid === true,
            'supported=' + r.supported + ' valid=' + r.valid);
      check('探针字节里真的有一条 SIMD 指令（不是被折叠掉的空壳）',
            r.len > 100 && r.hasSimdOp === true, r.len + ' 字节，含 0xFD=' + r.hasSimdOp);
    });

    await test.step('八、多页点击：落点跟着点走，视口不跟着跑', async () => {
      const r = await p.ev<{
        page: number; caretPage: number; before: number; after: number; dy: number; line: number;
      }[]>(`
        const H = window.__demo, d = H.diag;
        await window.__samples.run('long');
        await window.__demo.configure({ zoom: 1.0 });
        d.stopBlink();
        await H.setCaret({ para: 0, offset: 0 });
        await d.present();
        await H.flush();
        const stage = document.querySelector('.heditor-stage');
        const pagesEl = document.querySelector('.heditor-pages');
        const px = d.pagePx();
        const dpr = window.devicePixelRatio || 1;
        const step = px.h + px.gap;
        const rows = [];
        for (const page of [1, 3]) {
          // 把目标页滚到视口里（滚动本身不是判据），再在页内 (140, 160) 处按一下。
          stage.scrollTop = Math.round((page * step) / dpr - 100);
          await new Promise((res) => setTimeout(res, 260));
          await H.flush();
          const pr = pagesEl.getBoundingClientRect();
          const clientX = pr.left + 140;
          const clientY = pr.top + (page * step) / dpr + 160;
          const el = document.elementFromPoint(clientX, clientY);
          const before = stage.scrollTop;
          el.dispatchEvent(new MouseEvent('mousedown', {
            clientX, clientY, bubbles: true, cancelable: true, button: 0,
          }));
          await H.flush();
          await new Promise((res) => setTimeout(res, 140));
          await H.flush();
          const c = H.caret();
          // 落点与点击点的差：光标矩形是行盒顶，只要求落在点上面那一行以内。
          const z = H.config().zoom * (4 / 3) * dpr;
          rows.push({ page, caretPage: c.page, before, after: stage.scrollTop,
                      dy: Math.abs(c.rect.y - (160 * dpr) / z), line: c.rect.h });
        }
        return rows;`);
      const bad = r.filter((x) => x.caretPage !== x.page);
      check('点在第二页与第四页，光标落在那一页', bad.length === 0,
            JSON.stringify(r.map((x) => [x.page, x.caretPage])));
      const jumped = r.filter((x) => Math.abs(x.after - x.before) > 2);
      // 视口被拽走一次，多页文档上就是"点一下页面乱跳、光标看不见"。
      check('点击不动视口（scrollTop 不变）', jumped.length === 0,
            JSON.stringify(r.map((x) => [x.before, x.after])));
      const off = r.filter((x) => x.dy > x.line);
      check('落点在点击的那一行上（不差出一行）', off.length === 0,
            JSON.stringify(r.map((x) => [x.dy, x.line])));
    });

    await test.step('九、光标闪烁：点完一直闪，不是亮一下就灭', async () => {
      const r = await p.ev<{ text: string; flips: number; hasOn: boolean; hasOff: boolean }>(`
        const H = window.__demo;
        await H.setCaret({ para: 0, offset: 0 });
        await H.flush();
        const seen = [];
        for (let i = 0; i < 20; ++i) {
          await new Promise((res) => setTimeout(res, 120));
          seen.push(H.caret() && H.caret().visible ? 1 : 0);
        }
        const text = seen.join('');
        return { text, flips: text.replace(/(.)\\1*/g, (m) => m[0]).length,
                 hasOn: seen.includes(1), hasOff: seen.includes(0) };`);
      // 2.4 s 里至少两轮半；取"既有亮也有灭、且至少翻两次"，不给精确节拍 ——
      //   定时器在负载下会晚，判据不该跟着抖。
      check('2.4 秒内光标既亮过也灭过', r.hasOn && r.hasOff, r.text);
      check('至少翻转两次（不是亮一下就灭）', r.flips >= 3, '翻转 ' + r.flips + ' 次：' + r.text);
    });

    await test.step('十、没有 console 错误', async () => {
      const real = errors.filter((t) => !/favicon/i.test(t));
      check('没有 console 错误', real.length === 0, real.slice(0, 2).join(' | '));
    });
  });
});
