// 诊断面（diag）的运行时判据。
//
// 为什么单独一份：demo_probe 只证明"公开面只有公开面"，diag 在 debug 未开时抛
//   TypeError —— 也就是说它平时一条都不会被调用。类型对上不代表命令接通：
//   diag 的每个成员都是"读一个命令 / 写一个命令 + 补收尾"，接错一个只会让探针读到
//   undefined，而类型系统看不见。
//
// 这一份把 2026-10-22 补进去的 17 个成员逐条跑一遍：装载 7、光标 2、闪烁 3、
//   字体 2、窗口 1、错误 1。两条传输各跑一遍（diag 的转发在 Worker 上是一次往返）。
//
// 用法：node scripts/diag_probe.mjs --port 9333 --transport worker
//       页面默认是构建出来的示例页（web/dist-app/index.html）；换 Vite 开发服务器就传
//       --url http://127.0.0.1:5173/index.html。
import { CDP, Checks, sleep } from './cdp.mjs';

const die = (e) => {
  console.log('PROBE FAILED: ' + ((e && (e.stack || e.message)) || String(e)));
  process.exit(1);
};
process.on('uncaughtException', die);
process.on('unhandledRejection', die);

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const TRANSPORT = opt('transport', 'inline');
const URL_BASE = opt('url', 'http://127.0.0.1:8781/web/dist-app/index.html');
const URL_ = URL_BASE
  + (URL_BASE.includes('?') ? '&' : '?')
  + 'transport=' + TRANSPORT + '&debug=1';

const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
const page = list.find((t) => t.type === 'page');
if (!page) throw new Error('没有可用的页面 target');
const cdp = await CDP.connect(page.webSocketDebuggerUrl);
await cdp.send('Page.enable');
await cdp.send('Runtime.enable');
await cdp.send('Log.enable');
await cdp.disableHttpCache();
await cdp.send('Emulation.setDeviceMetricsOverride',
               { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });

const chk = new Checks('diag');
console.log('→ 打开', URL_);
await cdp.send('Page.navigate', { url: URL_ });
const bootMs = await cdp.waitFor('return !!window.__demoReady', 'window.__demoReady', 40000);
console.log(`→ 页面就绪，耗时 ${(bootMs / 1000).toFixed(1)} s`);

const ev = (body) => cdp.eval('return (async () => {\n' + body + '\n})();');

chk.section('一、diag 只在 debug 下存在');
{
  const r = await ev(`
    const e = window.__demo;
    return { has: !!e.diag, keys: Object.keys(e.diag || {}).length };`);
  chk.check('debug: true 时 diag 可读', r.has === true);
  chk.check('diag 成员数 > 40', r.keys > 40, String(r.keys));
}

chk.section('二、装载');
{
  const r = await ev(`
    const d = window.__demo.diag;
    const s6 = await d.loadScenario(6, 1.5);
    const p6 = window.__demo.paragraphs().length;
    const s7 = await d.loadScenario(7, 1.5);
    const p7 = window.__demo.paragraphs().length;
    const back = await d.reset(6, 1.5);
    const pBack = window.__demo.paragraphs().length;
    return { s6, p6, s7, p7, back, pBack,
             full6: !!(s6 && s6.layoutWasFull), full7: !!(s7 && s7.layoutWasFull) };`);
  chk.check('loadScenario(6) 返回那一拍的账', r.full6 === true, String(r.p6));
  chk.check('场景 6 是短文档', r.p6 > 0 && r.p6 < 10, `${r.p6} 段`);
  chk.check('场景 7 是长文档', r.p7 >= 200, `${r.p7} 段`);
  chk.check('场景 7 首次排版是全量', r.full7 === true);
  chk.check('reset() 回到默认场景', r.pBack === r.p6, `${r.pBack} vs ${r.p6}`);
}
{
  const r = await ev(`
    const d = window.__demo.diag;
    await d.stress(300, 10);
    const a = window.__demo.paragraphs().length;
    await d.stressDiverse(200, 12);
    const b = window.__demo.paragraphs().length;
    return { a, b };`);
  chk.check('stress 造出 300 段', r.a === 300, String(r.a));
  chk.check('stressDiverse 造出 200 段', r.b === 200, String(r.b));
}
{
  const r = await ev(`
    const d = window.__demo.diag;
    await d.loadFormSample(0);
    const a = window.__demo.form().length;
    await d.loadMedicalSample(1);
    const p = window.__demo.paragraphs().length;
    return { a, p };`);
  chk.check('loadFormSample 装出了控件', r.a > 0, String(r.a));
  chk.check('loadMedicalSample 装出了病历（> 20 段）', r.p > 20, `${r.p} 段`);
}

chk.section('三、整批建控件');
{
  const r = await ev(`
    const d = window.__demo.diag;
    await d.setFormControls([
      { start: 0, end: 3, kind: 0 },
      { start: 5, end: 8, kind: 0, ro: true },
    ]);
    const f = window.__demo.form();
    return { n: f.length, ro: f.map((x) => x.focused) };`);
  chk.check('setFormControls 换掉了控件表', r.n === 2, String(r.n));
}

chk.section('四、光标与相位');
{
  const r = await ev(`
    const d = window.__demo.diag;
    await d.reset(6, 1.5);
    await d.setCaretAt(0, 2, 0);
    const at = window.__demo.caret().pos;
    // 只改位置不呈现：真源在引擎里，读 caretLive（直读），不读 editStats（上一拍的账）。
    d.setCaretOnly(0, 5);
    await new Promise((r) => setTimeout(r, 30));
    const live = d.caretLive();
    // caret() 的读数时机两条传输不同：同线程是"上一次呈现的账"，Worker 是随每次写
    //   推回来的 state。所以这里只比引擎里的真值，不拿读数当"有没有呈现"的判据。
    const shown = window.__demo.caret().pos.offset;
    return { at, live, shown };`);
  chk.check('setCaretAt 落到段 0 第 2 格', r.at.para === 0 && r.at.offset === 2,
            JSON.stringify(r.at));
  chk.check('setCaretOnly 改了引擎里的位置', r.live === 5, String(r.live));
}
{
  const r = await ev(`
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
  chk.check('showCaret(false) 后呈现，光标不可见', r.off === false, String(r.off));
  chk.check('showCaret(true) 后呈现，光标又出现', r.on === true, String(r.on));
  chk.check('startBlink / stopBlink 不抛', true);
}

chk.section('五、字体与窗口');
{
  const r = await ev(`
    const d = window.__demo.diag;
    const lazy = await d.reloadFonts('lazy');
    const full = await d.reloadFonts('full');
    d.setFontCacheConfig(65536, 4);
    const anchor = await d.setViewportAnchor(0, 2);
    return { lazy, full, anchor };`);
  chk.check('reloadFonts(lazy) 报告 lazy 模式', r.lazy && r.lazy.mode === 'lazy',
            JSON.stringify(r.lazy));
  chk.check('reloadFonts(full) 报告 full 模式', r.full && r.full.mode === 'full',
            JSON.stringify(r.full));
  chk.check('setViewportAnchor 返回那一拍的账', !!r.anchor && typeof r.anchor === 'object',
            JSON.stringify(r.anchor).slice(0, 40));
}

chk.section('六、错误历史与既有诊断');
{
  const r = await ev(`
    const d = window.__demo.diag;
    const errs = d.errors();
    const au = await d.auditFrame();
    // 必须 await：Worker 传输下它是一次往返，不 await 拿到的是 Promise ——
    //   而 Promise 也是 object，于是"返回取证对象"这条会以空值恒真。
    const sc = await d.selfCheck();
    const px = d.pagePx();
    const bk = d.bracketSlots();
    return { isArr: Array.isArray(errs), n: errs.length, diff: au && au.diff,
             sel: !!sc && sc.compared > 0, compared: sc && sc.compared,
             diffPx: sc && sc.screenVsEngineDiffPx, px, hasBrackets: bk !== undefined };`);
  chk.check('errors() 是数组', r.isArr === true, String(r.n));
  // auditFrame 的载荷是 { diff, box }：diff = 0 表示"帧与从零重画逐字节相同"。
  chk.check('auditFrame 自审通过（diff = 0）', r.diff === 0, String(r.diff));
  // compared > 0 是这一条的反向一半：没有它，缺画布时 diff 恒 0 也算过。
  chk.check('selfCheck 真的比了像素且屏幕与引擎一致',
            r.sel === true && r.diffPx === 0, `compared=${r.compared}，差 ${r.diffPx} px`);
  chk.check('pagePx 是页度量', r.px && r.px.w > 0 && r.px.h > 0,
            JSON.stringify(r.px));
  chk.check('bracketSlots 可读', r.hasBrackets === true);
}

chk.section('七、页画布：一页一张、都在窗口里、都真的可见');
{
  const r = await ev(`
    const d = window.__demo.diag;
    await d.reset(6, 1.5);
    const g = window.__demo.geometry();
    // Worker 传输按槽复用（槽数固定，窗口之外的槽挂 display:none），所以这里只看
    //   此刻真的在显示画布 —— "一页一张"在两种传输下都成立，槽数是实现细节。
    const els = d.pageCanvasList().filter((el) => el.style.display !== 'none');
    const pages = els.map((el) => Number(el.dataset.page)).sort((a, b) => a - b);
    // 每张画布的中心点命中的必须还是它自己：容器被裁掉、被别的层盖住时这一条会红。
    const hidden = [];
    for (const el of els) {
      const q = el.getBoundingClientRect();
      const hit = document.elementFromPoint(q.left + q.width / 2, q.top + q.height / 2);
      if (hit !== el) hidden.push(Number(el.dataset.page));
    }
    return { n: els.length, want: g.window.count, pages, hidden,
             first: g.window.first };`);
  chk.check('页画布张数 == 窗口页数', r.n === r.want, `${r.n} / ${r.want}`);
  chk.check('每张画布都标着页号，且从窗口首页开始',
            r.pages.every((p, i) => p === r.first + i), JSON.stringify(r.pages));
  chk.check('每张画布在屏幕上都真的可见（没被裁掉、没被盖住）',
            r.hidden.length === 0, `看不见的页：${JSON.stringify(r.hidden)}`);
}

chk.section('八、场景装载：页数与字形数随内容变');
{
  const r = await ev(`
    const d = window.__demo.diag;
    const rows = [];
    for (const id of [6, 7]) {
      const st = await d.loadScenario(id, 1.0);
      const lay = d.stats();
      rows.push({ id, pages: st.totalPages, glyphs: lay.glyphCount, paras: st.totalParagraphs });
    }
    return rows;`);
  // 单段演示与多段长文：页数与字形数都必须跟着变（恒等说明参数没接上）。
  chk.check('场景 6 是单页', r[0].pages >= 1 && r[0].paras <= 4,
            `${r[0].pages} 页 / ${r[0].paras} 段`);
  chk.check('场景 7 是多页长文', r[1].pages > 1 && r[1].paras > 200,
            `${r[1].pages} 页 / ${r[1].paras} 段`);
  chk.check('字形数是正数且随场景变', r[0].glyphs > 0 && r[1].glyphs > r[0].glyphs,
            `${r[0].glyphs} → ${r[1].glyphs}`);
}

chk.section('九、能力探测：WebAssembly SIMD');
{
  const r = await ev(`
    const d = window.__demo.diag;
    const s = d.simd();
    const bytes = new Uint8Array(s.probe);
    // 判据自己跑一次 validate，并确认里面真的有一条 SIMD 指令（0xFD 前缀）——
    //   只信 supported 的话，"探针被常量折叠成空壳"与"环境真支持"分不开。
    let valid = false;
    try { valid = WebAssembly.validate(bytes) === true; } catch (e) { valid = false; }
    return { supported: s.supported, valid, len: bytes.length,
             hasSimdOp: bytes.includes(0xFD) };`);
  chk.check('探测结论是"支持"，且探针字节自己 validate 也是 true',
            r.supported === true && r.valid === true,
            `supported=${r.supported} valid=${r.valid}`);
  chk.check('探针字节里真的有一条 SIMD 指令（不是被折叠掉的空壳）',
            r.len > 100 && r.hasSimdOp === true, `${r.len} 字节，含 0xFD=${r.hasSimdOp}`);
}

chk.section('十、多页点击：落点跟着点走，视口不跟着跑');
{
  const r = await ev(`
    const H = window.__demo, d = H.diag;
    await d.loadScenario(7, 1.0);
    d.stopBlink();
    await H.setCaret({ para: 0, offset: 0 });
    await d.present();
    await H.flush();
    const stage = document.querySelector('#stage');
    const pagesEl = document.querySelector('#pages');
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
  chk.check('点在第二页/第四页，光标落在那一页', bad.length === 0,
            JSON.stringify(r.map((x) => [x.page, x.caretPage])));
  const jumped = r.filter((x) => Math.abs(x.after - x.before) > 2);
  // 视口被拽走一次，多页文档上就是"点一下页面乱跳、光标看不见"。
  chk.check('点击不动视口（scrollTop 不变）', jumped.length === 0,
            JSON.stringify(r.map((x) => [x.before, x.after])));
  const off = r.filter((x) => x.dy > x.line);
  chk.check('落点在点击的那一行上（不差出一行）', off.length === 0,
            JSON.stringify(r.map((x) => [x.dy, x.line])));
}

chk.section('十一、光标闪烁：点完一直闪，不是亮一下就灭');
{
  const r = await ev(`
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
  // 530 ms 一拍、2.4 s 里至少两轮半；取"既有亮也有灭、且至少翻两次"这一点，
  //   不给精确节拍 —— 定时器在负载下会晚，判据不该跟着抖。
  chk.check('2.4 秒内光标既亮过也灭过', r.hasOn && r.hasOff, r.text);
  chk.check('至少翻转两次（不是亮一下就灭）', r.flips >= 3, `翻转 ${r.flips} 次：${r.text}`);
}

chk.section('十二、没有 console 错误');
{
  const errs = cdp.consoleErrors.filter((t) => !/favicon/i.test(t));
  chk.check('没有 console 错误', errs.length === 0, errs.slice(0, 2).join(' | '));
}

const fails = chk.rows.filter((x) => !x.ok).length;
console.log(`\n共 ${chk.total} 项断言，失败 ${fails} 项`);
await sleep(50);
if (!process.env.HEDITOR_KEEP_BROWSER) await cdp.send('Browser.close').catch(() => {});
process.exit(fails ? 1 : 0);
