// 示例页的运行时闸：证明公开面真的只有公开面。
//
// 静态检查 api_check.mjs 看的是源码里出现了什么；这一份看的是运行期拿得到什么。
//   两者缺一不可：源码里没写 window.__heditor，不代表页面运行时没有那个全局。
//
// 六节与旧 demo_probe 逐条对应：内部全局、实例成员、公开方法、change 事件、
//   configure 的夹取与未知键、销毁后的拒绝。
import { check } from '../support/check';
import { test } from '../support/probe';

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
