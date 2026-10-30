// 性能判据：增量排版与页场景记账的耗时与结构量。
//
// 排版那两条判的是比值，分子分母来自同一次测量，所以与机器的绝对快慢无关；但它们仍然
// 会被同机上的其它 test 顶翻 —— 所以单独成一份 spec、单独一条命令（npm run test:perf），
// 跑的时候独占一个 worker，机器上不该有别的活。
//
// 页场景这一侧不判耗时比值：全量与增量都要为窗口内每一页构建场景，构建耗时本来就接近；
// 真正说明“增量发生了”的是脏页数（结构量，不受调度影响）。
import { check } from '../support/check';
import { test } from '../support/probe';
import { key, openEdit, finishEdit } from '../support/edit-helpers';

// 脏段的位置：靠后敲字，后缀最长，增量与全量的差距最大。
const DIRTY = 150;

test.describe('编辑面：耗时比值', () => {
  test('增量排版与页场景都真的省（比值与结构量）', async ({ open }) => {
    const { probe, consoleErrors } = await openEdit(open);
    const state: Record<string, any> = {};

    // 前面几节装过表单样例时模式会停在表单，长文档没有控件，表单模式下打不进字。
    await probe.read('return await SET_MODE(0);');
    const base = await probe.read('return await LOAD.long(1.0);');
    check('前提：长文档载入（段数 ≥ 200）', base.totalParagraphs >= 200,
          `${base.totalParagraphs} 段 / ${base.totalLines} 行`);
    check('前提：首次排版是全量（整篇都脏）', base.layoutWasFull === true);
    state.base = base;

    // 在脏段中间敲一个字（走真实键盘路径），量这一整轮。
    await probe.read(`return await H.setCaret({ para: ${DIRTY}, offset: 5 });`);
    await probe.read(key('测'));
    const st = await probe.read('return D.editStats();');
    state.stats = st;
    // 绝对值随机器变，报出来只为对照；判据只看比值。
    console.log(`     敲一个字：增量 ${st.layoutMs.toFixed(3)} ms ｜ 首次全量 ` +
                `${base.layoutMs.toFixed(3)} ms ｜ 脏页 ${st.dirtyPageCount} / 共 ${st.totalPages} 页`);
    check('★ 增量耗时小于全量耗时（同一次测量里的两个数）',
          st.layoutMs < base.layoutMs,
          `增量 ${st.layoutMs.toFixed(3)} ms vs 首次全量 ${base.layoutMs.toFixed(3)} ms`);

    // 相对耗时：分子分母都来自同一次 auditLayout 调用（curMs 是刚才那次增量续排，
    //   fullMs 是本次审计顺便做的全量重排），不掺历史数字。
    const au = await probe.read('return await D.auditLayout();');
    state.audit = au;
    check('前提：排版自审可用（diff 0）', !!au && au.diff === 0,
          au ? `diff ${au.diff}` : '返回 null');
    if (au) {
      // 浏览器的 steady_clock 分辨率是 0.1~0.2 ms（emscripten_get_now），
      //   全量太小时比值会失真（0.2 / 0.4 恰好卡在边界）。所以：全量 ≥ 1 ms 时
      //   才判"不到 20%"，否则退成"增量更快"这条不依赖分辨率的判据 —— 宁可少判，
      //   也要避免"机器越快越假红"。
      const ratioOk = au.fullMs >= 1.0 ? au.curMs * 5 < au.fullMs : au.curMs < au.fullMs;
      check('★ 靠后敲字：增量耗时远小于全量（全量 ≥ 1ms 时判 <20%）', ratioOk,
            `增量 ${au.curMs.toFixed(3)} ms ｜ 全量 ${au.fullMs.toFixed(3)} ms ｜ ` +
            `${(100 * au.curMs / Math.max(au.fullMs, 1e-9)).toFixed(1)}%` +
            (au.fullMs < 1.0 ? '（全量太小，只判"更快"）' : ''));
    }

    // 页场景：耗时只报出来对照；判据落在脏页数上。
    //   全量与增量都要构建窗口内每一页的场景，所以两者耗时接近是正常的；
    //   真正说明“增量发生了”的是只有受影响的页被记为脏。
    const fpFull = base.pageFpMs;
    const fpIncr = st.pageFpMs;
    console.log(`     页场景：整篇 ${fpFull.toFixed(3)} ms ｜ 增量 ${fpIncr.toFixed(3)} ms ｜ ` +
                `脏页 ${st.dirtyPageCount} / 共 ${st.totalPages} 页`);
    check('★ 页场景增量化：增量编辑之后只弄脏少数页（≤ 整篇的 1/4）',
          st.dirtyPageCount > 0 && st.dirtyPageCount * 4 <= st.totalPages,
          `脏页 ${st.dirtyPageCount} / 共 ${st.totalPages} 页`);

    await finishEdit(probe, consoleErrors, state);
  });
});
