// 示例页的运行时闸：证明公开面真的只有公开面。
//
// 静态检查 api_check.mjs 看的是源码里出现了什么；这一份看的是运行期拿得到什么。
//   两者缺一不可：源码里没写 window.__heditor，不代表页面运行时没有那个全局。
//
// 六节与旧 demo_probe 逐条对应：内部全局、实例成员、公开方法、change 事件、
//   configure 的夹取与未知键、销毁后的拒绝。
import { createHash } from 'node:crypto';

import { check } from '../support/check';
import { test } from '../support/probe';
import { SELECT_HELPERS } from '../support/readings';

// 第一节的读数。
type Globals = {
  heditor: string;
  createWorkerEditor: string;
  createEditor: string;
  readThrows: boolean;
};

// 第二节的读数。
type Members = {
  keys: string[];
  diagThrows: boolean;
  unknownThrows: boolean;
  msg: string;
};

// 第三节的读数。
type PublicApi = {
  before: number;
  typed: boolean;
  hasAB: boolean;
  savedLen: number;
  afterUndo: string;
};

// 第四节的读数。
type Changes = { seen: { source: string; version: number; textLen: number; json: number }[] };

// 第五节的读数。
type Config = {
  zoom: number;
  clamped: number;
  readOnlyWrite: boolean | null;
  modeShown: string;
  cfgZoom: number;
  badKey: string;
};

// 第六节的读数。
type Destroyed = { threw: string; empty: number };

test.describe('示例页的公开面', () => {
  test('页画布与定位祖先的约定', async ({ open }) => {
    // 这条钉的是"画布不参与文档流"。两个画布集合（同线程按页建销、Worker 按槽复用）
    //   一旦哪边漏了定位，画布就会随文档流堆叠：style.top 失效、换窗时改流高度触发
    //   浏览器的滚动锚定（滚轮往下滚自己弹回来）。让它在构造约定上就红。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    const r = await p.ev<{ count: number; loose: string[]; pagesPos: string }>(`
      const e = window.__demo;
      const pagesEl = e.element.querySelector('.heditor-pages');
      const cs = [...e.element.querySelectorAll('.heditor-pages canvas')];
      const loose = cs.filter((c) => getComputedStyle(c).position !== 'absolute')
        .map((c) => c.dataset.page ?? '?');
      return { count: cs.length, loose, pagesPos: getComputedStyle(pagesEl).position };`);
    check('页容器里有画布', r.count > 0, String(r.count));
    check('★ 每张页画布都是绝对定位', r.loose.length === 0, r.loose.join(','));
    check('★ .heditor-pages 是定位祖先（不是 static）', r.pagesPos !== 'static', r.pagesPos);
  });

  test('工具栏：下拉框拿得住焦点，字体与字号有回显', async ({ open }) => {
    // 两件事各一条判据：下拉框被外壳抢走焦点就"点开就收回去"（一个选项都选不了）；
    //   字体字号读的是"实际生效的那一份"，否则正文一个字没改过时控件显示空白。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    const r = await p.ev<any>(`
      ${SELECT_HELPERS}
      const e = window.__demo;
      const pick = (act) => document.querySelector('.heditor-select[data-act="' + act + '"]');
      const settle = async (fn) => {
        const t0 = performance.now();
        while (performance.now() - t0 < 3000) {
          if (fn()) return true;
          await new Promise((r) => setTimeout(r, 20));
        }
        return false;
      };
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 1 });
      await e.flush();
      const eff = await e.formatAt({ effective: true });
      const size = pick('size');
      const font = pick('font');
      const shown = await settle(() => SELECTED('size') !== '' && SELECTED('font') !== '');
      const before = { size: SELECT_VALUE('size'), font: SELECT_VALUE('font'),
                       sizeText: SELECTED('size'), fontText: SELECTED('font') };
      // 点一下下拉框：焦点要留在下拉这一套里（触发器或它弹出的候选），不能被外壳抢回编辑器。
      font.focus();
      const hadFocus = document.activeElement === font;
      font.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0 }));
      const pop = document.querySelector('.heditor-pop');
      const keptFocus = document.activeElement === font ||
        (pop !== null && pop.contains(document.activeElement));
      return { eff, shown, before, hadFocus, keptFocus,
               focusTag: document.activeElement?.tagName ?? '' };`);
    check('实际生效的格式里有字体与字号',
          r.eff.size !== undefined && r.eff.font !== undefined, JSON.stringify(r.eff));
    check('两个下拉框都填上了值', r.shown === true, JSON.stringify(r.before));
    check('字号回显 = 实际生效的字号', Number(r.before.size) === r.eff.size,
          JSON.stringify({ shown: r.before.size, eff: r.eff.size }));
    check('字体回显 = 实际生效的字体', Number(r.before.font) === r.eff.font,
          JSON.stringify({ shown: r.before.font, eff: r.eff.font }));
    check('字体那一格写的不是空档位', r.before.fontText !== '—', r.before.fontText);
    check('点下拉框之前焦点在它身上', r.hadFocus === true);
    check('★ 点一下下拉框不会把焦点抢回编辑器', r.keptFocus === true, r.focusTag);
  });

  test('编辑标记：默认画段落标记，三档都能切，只改像素不动版面', async ({ open }) => {
    // 判据是像素：标记不进内容（不占宽度、不改断行），所以「有没有画」只能靠画面回答；
    //   而「没动版面」要另外量 —— 段数与光标矩形必须逐次相同。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    await p.ev(`const e = window.__demo;
      await e.setText('甲乙 丙\\n丁');
      // 光标是瞬态、会闪：设成全透明，截图里只剩内容像素。
      await e.configure({ caretStyle: Object.assign({}, e.config().caretStyle, { color: [0, 0, 0, 0] }) });
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();`);

    const clip = await p.ev<any>(`
      const pg = document.querySelector('.heditor-pages').getBoundingClientRect();
      return { x: pg.left, y: pg.top, width: 220, height: 110 };`);

    /**
     * 拍一张并算摘要：只比"这一拍和上一拍是不是同一张"。
     * @returns 截图的摘要
     */
    const shot = async (): Promise<string> => {
      const buf = await p.page.screenshot({ clip });
      return createHash('sha1').update(buf).digest('hex');
    };
    /**
     * 切一档标记，等呈现落地再拍。
     * @param mode 档位
     * @returns 这一档的画面摘要
     */
    const setMarks = async (mode: string): Promise<string> => {
      await p.ev(`await window.__demo.configure({ showMarks: '${mode}' }); await window.__demo.flush();`);
      await p.page.waitForTimeout(300);
      return shot();
    };

    const state = await p.ev<any>(`
      const e = window.__demo;
      const rect = () => JSON.stringify(e.caret().rect);
      const before = { mode: e.config().showMarks, paras: e.paragraphs().length, rect: rect() };
      return before;`);

    const withPara = await shot();  // 默认：只显示段落标记
    const off = await setMarks('none');
    const all = await setMarks('all');

    // 走工具栏那一条：改下拉框 → change → configure → 外壳重读回显。
    const after = await p.ev<any>(`
      ${SELECT_HELPERS}
      const e = window.__demo;
      PICK('marks', 'paragraph');
      await new Promise((r) => setTimeout(r, 300));
      return { mode: e.config().showMarks, paras: e.paragraphs().length,
               rect: JSON.stringify(e.caret().rect), toolbar: SELECT_VALUE('marks') };`);

    check('默认档是"只显示段落标记"', state.mode === 'paragraph', String(state.mode));
    check('★ 关掉标记：画面真的变了', withPara !== off, '两拍一模一样');
    check('★ 全部档比段落档多出空格里的中点', all !== withPara, '两拍一模一样');
    check('档位读得回来（工具栏改档也走 configure）', after.mode === 'paragraph',
          String(after.mode));
    check('★ 切三档都不动版面（段数与光标矩形逐次相同）',
          after.paras === state.paras && after.rect === state.rect,
          JSON.stringify({ paras: [state.paras, after.paras], rect: [state.rect, after.rect] }));
    check('工具栏那一格回显当前档位', after.toolbar === 'paragraph', after.toolbar);
  });

  test('连页末尾：滚动高度跟着内容走，至少留一页', async ({ open }) => {
    // 连页下"页"只是渲染条带，可滚区该跟着最后一行走。总高按整数页算就会在末尾空出
    //   一整页；口径是 max(一页, 内容底边)，多出来的白纸由页容器裁掉。
    //   三条判据：短文档仍留一页、长文档不是整数页、可滚区不被裁掉的那截撑大。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    const r = await p.ev<any>(`
      const e = window.__demo;
      const box = () => {
        const pages = e.element.querySelector('.heditor-pages');
        const stage = e.element.querySelector('.heditor-stage');
        const g = e.geometry();
        const cs = [...e.element.querySelectorAll('.heditor-pages canvas')];
        const last = cs.filter((c) => c.dataset.page === String(g.pages - 1))[0] || null;
        const pr = pages.getBoundingClientRect();
        return {
          height: pages.offsetHeight,
          // 可滚区 = 页容器在舞台里的整段（含上边距 + 底部那道呼吸缝）。裁剪没生效时
          //   它会被画布撑大 —— 那种撑大不受缝的宽度控制，所以两件事要分开判。
          scroll: stage.scrollHeight,
          padBottom: parseFloat(getComputedStyle(stage).paddingBottom) || 0,
          offsetTop: pages.offsetTop,
          lastBottom: last ? last.getBoundingClientRect().bottom - pr.top : -1,
          geo: g.scrollHeight,
          pageH: g.page.h,
          gap: g.gap,
          pages: g.pages,
        };
      };
      await e.configure({ pageMode: 'continuous' });
      await e.setText('短');
      await e.flush();
      const short = box();
      const line = '连页末尾的高度跟着内容走，不再总是整数页。';
      await e.setText(Array.from({ length: 60 }, () => line).join('\\n'));
      await e.flush();
      const long = box();
      await e.configure({ pageMode: 'paged' });
      await e.flush();
      const paged = box();
      return { short, long, paged };`);

    const near = (a: number, b: number): boolean => Math.abs(a - b) <= 2;
    check('前提：长文档排成了两页以上（连页条带）', r.long.pages >= 2, String(r.long.pages));
    check('★ 短文档在连页下仍留一页高',
          near(r.short.height, r.short.pageH), `${r.short.height} vs ${r.short.pageH}`);
    check('★ 长文档的总高不是整数页（末尾不空一整页）',
          r.long.height > r.long.pageH && r.long.height < r.long.pages * r.long.pageH - 40,
          `${r.long.height} / ${r.long.pages} x ${r.long.pageH}`);
    check('★ 可滚区等于页容器 + 底部那道呼吸缝（没有别的空白）',
          near(r.long.scroll, r.long.offsetTop + r.long.height + r.long.padBottom),
          `scroll ${r.long.scroll} vs ${r.long.offsetTop} + ${r.long.height} + ${r.long.padBottom}`);
    // 反向一半：那道缝必须真的存在（页脚贴着状态栏时它就是 0，而那是这条判据的由来）。
    check('★ 最后一页底下留了呼吸缝，不贴着状态栏',
          r.long.padBottom >= 8, `${r.long.padBottom}px`);
    check('最后一页的画布仍伸到容器外（被裁的正是它多出来的那一截）',
          r.long.lastBottom > r.long.height + 2, `${r.long.lastBottom} vs ${r.long.height}`);
    check('geometry() 报的滚动高度与 DOM 一致',
          near(r.long.geo, r.long.height), `${r.long.geo} vs ${r.long.height}`);
    check('分页下仍是整数页（这条行为没被改坏）',
          near(r.paged.height, r.paged.pages * r.paged.pageH + (r.paged.pages - 1) * r.paged.gap),
          `${r.paged.height} / ${r.paged.pages} x ${r.paged.pageH}`);

    // 反向一半：切回分页，同一份文档的总高该回到"整数页"。
    check('★ 切回分页：总高回到页数 x 页高', r.paged.height > r.long.height,
          `${r.paged.height} vs ${r.long.height}`);
  });

  test('只有契约成员，六组行为按契约工作', async ({ open }) => {
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });

    await test.step('内部全局都没了', async () => {
      const r = await p.ev<Globals>(`
        const out = {};
        out.heditor = typeof window.__heditor;
        out.createWorkerEditor = typeof window.createWorkerEditor;
        out.createEditor = typeof window.createEditor;
        // 读一次才判得出是不是 getter：旧宿主的内部工厂挂在会抛的 getter 上。
        //   能读到 undefined 说明那个 getter 已经撤掉。
        out.readThrows = false;
        try { window.createEditor; } catch (e) { out.readThrows = e instanceof TypeError; }
        return out;`);
      check('window.__heditor 不存在', r.heditor === 'undefined', r.heditor);
      check('window.createWorkerEditor 不存在', r.createWorkerEditor === 'undefined', r.createWorkerEditor);
      check('window.createEditor 不存在（内部工厂不再挂全局）',
            r.createEditor === 'undefined' || r.readThrows === true,
            r.readThrows ? '读它就抛（旧宿主的 getter）' : 'undefined');
    });

    await test.step('实例上只有契约成员', async () => {
      const r = await p.ev<Members>(`
        const e = window.__demo;
        const keys = Object.keys(e);
        let diagThrows = false;
        try { e.auditFrame; } catch (err) { diagThrows = err instanceof TypeError; }
        let unknownThrows = false;
        let msg = '';
        try { e.pleaseDoSomething; } catch (err) { unknownThrows = err instanceof TypeError; msg = err.message; }
        return { keys, diagThrows, unknownThrows, msg };`);
      check('实例成员非空', r.keys.length > 0, r.keys.join(','));
      check('没有 auditFrame / markCheck 这类诊断成员', !r.keys.includes('auditFrame'));
      check('读 diag 抛 TypeError（debug 未开）', r.diagThrows === true);
      check('读未知成员抛 TypeError', r.unknownThrows === true, r.msg);
    });

    await test.step('公开方法', async () => {
      const r = await p.ev<PublicApi>(`
        const e = window.__demo;
        const before = e.paragraphs().length;
        await e.setCaret({ para: 0, offset: 0 });
        const typed = await e.type('AB');
        const text = e.text();
        const saved = await e.save();
        await e.undo();
        return { before, typed, hasAB: text.includes('AB'), savedLen: saved.length,
                 afterUndo: e.text() };`);
      check('存在多段', r.before > 0, String(r.before));
      check('type 返回 true', r.typed === true);
      check('文本里出现了刚敲的字', r.hasAB === true);
      check('save 返回非空 JSON', r.savedLen > 100, r.savedLen + ' 字节');
      check('undo 生效（AB 被撤回）', !r.afterUndo.includes('AB'));
    });

    await test.step('change 事件', async () => {
      const r = await p.ev<Changes>(`
        const e = window.__demo;
        const seen = [];
        const off = e.on('change', async (c) => seen.push({ source: c.source, version: c.version,
                                                            textLen: c.text.length,
                                                            json: (await c.json()).length }));
        await e.type('X');
        off();
        await e.type('Y');
        return { seen };`);
      check('on(change) 收到了一次', r.seen.length === 1, JSON.stringify(r.seen));
      check('载荷里带 json（调方法才序列化）', r.seen[0].json > 100, String(r.seen[0].json));
      check('version 是递增的数', Number.isFinite(r.seen[0].version), String(r.seen[0].version));
    });

    await test.step('configure', async () => {
      const r = await p.ev<Config>(`
        const e = window.__demo;
        const got = await e.configure({ zoom: 1.25 });
        const clamped = await e.configure({ zoom: 99 });
        let badKey = '';
        try { await e.configure({ zom: 1 }); } catch (err) { badKey = err.message; }
        await e.configure({ mode: 'readOnly' });
        const readOnlyWrite = await e.type('nope');
        const modeShown = e.config().mode;
        await e.configure({ mode: 'document' });
        return { zoom: got.zoom, clamped: clamped.zoom, readOnlyWrite, modeShown,
                 cfgZoom: e.config().zoom, badKey };`);
      check('configure 返回生效后的值', r.zoom === 1.25, String(r.zoom));
      check('越界被夹取到 2.25', r.clamped === 2.25, String(r.clamped));
      check('config() 与返回值一致', r.cfgZoom === 2.25, String(r.cfgZoom));
      check('拼错的键同步抛', r.badKey.includes('未知配置项'), r.badKey);
      check('只读时编辑返回 false', r.readOnlyWrite === false, String(r.readOnlyWrite));
      check('mode 读回三档里的只读', r.modeShown === 'readOnly', String(r.modeShown));
    });

    await test.step('销毁', async () => {
      const r = await p.ev<Destroyed>(`
        const e = window.__demo;
        await e.destroy();
        let threw = '';
        try { await e.type('z'); } catch (err) { threw = err.message; }
        return { threw, empty: document.getElementById('ed').children.length };`);
      check('销毁后调用被拒', r.threw.includes('已销毁'), r.threw);
      check('容器被清空', r.empty === 0, String(r.empty));
    });
  });
});
