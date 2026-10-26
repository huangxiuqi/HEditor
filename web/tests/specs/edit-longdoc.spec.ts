// 编辑面（五）：长文档的增量排版与跨段编辑。
//
// 这一份只钉结构量：重排了几段、复用了哪几段、脏页是哪几页。耗时比值判据在
// tests/perf 里单独跑 —— 它们会被同机上的其它 test 顶翻。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit, key, ZOOM } from '../support/edit-helpers';

test.describe('编辑面：长文档与跨段编辑', () => {
  test('长文档只重排脏段之后的后缀，跨段编辑与撤销都按契约工作', async ({ open }) => {
    const { probe, consoleErrors } = await openEdit(open);

    // 取证账：字段逐个沿用旧探针的名字，便于对着旧日志复核。
    const state: Record<string, any> = {};
    const log = {
      section(title: string) {
        console.log('\n' + title);
      },
      note(text: string) {
        console.log('     ' + text);
      },
    };

  log.section('[9] 长文档：增量排版只重排脏段之后的后缀');
  {
    // 单段文档没有可复用的前缀，增量与全量必然相等（见 [1] / [2]）。
    //   多段文档才谈得上"只重排脏段"—— 这一节用演示的长文档量它。
    //   段数不写死：那张文书是演示数据，随时可能被调大调小，而这一节要钉的是
    //   "重排了几段、复用了几段"这个结构，不是那个数本身。
    const DIRTY = 150;
    // 前面几节装过表单样例，模式停在表单：这一份长文档没有控件，表单模式下打不进字。
    //   这一节量的是增量排版的账，先回编辑模式。
    await probe.read('return await SET_MODE(0);');
    const base = await probe.read('return await LOAD.scenario(7, 1.0);');
    check('长文档载入（段数 ≥ 200）', base.totalParagraphs >= 200,
              `${base.totalParagraphs} 段 / ${base.totalLines} 行`);
    check('首次排版是全量（整篇都脏）', base.layoutWasFull === true);
    state.longDoc = { paragraphs: base.totalParagraphs, lines: base.totalLines,
                       fullMs: base.layoutMs };

    // 先停闪烁：它每半拍 present 一次，而编辑账里的脏页数、以及帧与整帧重画的对账
    //   都是"这一拍"的账 —— 读完再闪一下，脏页数就归零、帧里也换了一次光标相位。
    await probe.read('return await D.stopBlink();');

    // 在脏段中间敲一个字（走真实键盘路径）
    await probe.read(`return await H.setCaret({ para: ${DIRTY}, offset: 5 });`);
    const cps_before = await probe.read<number>('const s = D.editStats(); return s.docCodepoints;');
    await probe.read(key('测'));
    // 等这一下真的落地再读账。两件事叠在一起让"只 flush 一次"不够：
    //   一是键事件由浏览器派发，evaluate 返回时按键可能还没进输入层；
    //   二是 flush 只排空"已经入队"的消息，队列空着它当场就回。
    //   于是按"文档码点数真的 +1"来等（最多四轮），读到的才是这一拍的账。
    let st: any = null;
    for (let i = 0; i < 4; i++) {
      st = await probe.read('return D.editStats();');
      if (st && st.docCodepoints > cps_before) break;
      await probe.read('await H.flush();');
      await probe.page.waitForTimeout(30);
    }
    check('★ 敲字后是增量排版（不是全量）', st.layoutWasFull === false,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段 / 共 ${st.totalParagraphs} 段`);
    // 行结构没变 → 快路：只重排脏段自己，后缀整块搬回（一个字节都不动）。
    check(`★ 只重排第 ${DIRTY} 段，其余原样复用`,
              st.relaidParagraphs === 1 &&
                st.reusedParagraphs === st.totalParagraphs - DIRTY - 1,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段` +
                `（应为 1 / ${st.totalParagraphs - DIRTY - 1}）`);
    check('★ 重排行数真小于总行数', st.relaidLines < st.totalLines,
              `${st.relaidLines} / ${st.totalLines} 行`);
    // 分段账：前缀 [0,DIRTY) 一个字都没动、只重排第 DIRTY 段、其余整块搬回。
    // 这条比"重排段数对不对"更强：它把分界点也钉住了（前缀/脏段/复用各占哪一段）。
    check('★ 分段账：前缀未动 + 脏段 1 段重排 + 后缀全部复用',
              st.relaidParagraphs + st.reusedParagraphs === st.totalParagraphs - DIRTY,
              `1 + ${st.reusedParagraphs} = ${st.totalParagraphs} - ${DIRTY}`);
    // 耗时比值（增量对全量、页指纹对整篇）不在这里判：它们会被同机上的其它 test 顶翻，
    //   单独放在 tests/perf/edit-perf.spec.ts，跑的时候独占一个 worker。

    // 排版自审：增量结果与"不用缓存的全量重排"逐字段比 —— 必须完全相同
    const au = await probe.read('return await D.auditLayout();');
    check('★★ 增量结果与全量重排逐字段一致（auditLayout diff 0）',
              !!au && au.diff === 0,
              au ? `diff ${au.diff}（字形 ${au.glyphDiff}、行 ${au.lineDiff}、段 ${au.paraDiff}、` +
                `簇表 ${au.startsDiff}、统计 ${au.statsSame ? '同' : '不同'}）` : '返回 null');

    // 上屏与帧自审：长文档里编辑之后，屏幕 == 引擎帧，且引擎帧 == 整帧重画
    const sc = await probe.read('return await D.selfCheck();');
    check('长文档编辑后：屏幕 == 引擎帧', sc.screenVsEngineDiffPx === 0,
              `差 ${sc.screenVsEngineDiffPx} px`);
    const af = await probe.read('return await D.auditFrame();');
    check('长文档编辑后：引擎帧 == 整帧重画', af && af.diff === 0,
              af ? `差 ${af.diff} px` : 'null');

    // 页数跟着长：演示长文档不可能只有一页 —— 否则上面那些"重排行数"没有意义
    check('长文档确实排成了多页', st.totalPages > 1, `${st.totalPages} 页`);

    // 后缀平移复用：撑出一行之后，后缀的断行结果照样复用（只挪坐标）。
    //
    // 40 个汉字足以撑出至少一行。走 type()（直连引擎）而不是键盘逐字派发：
    //   键盘路径上面已经验过，这里要的是"一次插入 40 个字"把行结构撑变。
    const widened = await probe.read(
      `return await TYPE(${JSON.stringify('测'.repeat(40))});`);
    check('★ 撑出一行 → 后缀整体平移复用（重排 1 段）',
              !!widened && widened.suffixShifted === true && widened.relaidParagraphs === 1,
              widened ? `重排 ${widened.relaidParagraphs} 段 / 复用 ${widened.reusedParagraphs} 段 / ` +
                `平移=${widened.suffixShifted} / 脏页 ${widened.dirtyPageCount}` : 'null');
    check('★ 平移之后后缀段数照旧全部复用',
              !!widened && widened.reusedParagraphs + widened.relaidParagraphs ===
                widened.totalParagraphs - 150,
              widened ? `${widened.relaidParagraphs} + ${widened.reusedParagraphs} = ` +
                `${widened.totalParagraphs} - 150` : 'null');
    const auShift = await probe.read('return await D.auditLayout();');
    check('★★ 平移复用的排版与全量逐字段一致（auditLayout diff 0）',
              !!auShift && auShift.diff === 0,
              auShift ? `diff ${auShift.diff}（字形 ${auShift.glyphDiff}、行 ${auShift.lineDiff}、` +
                `段 ${auShift.paraDiff}）` : 'null');
    const scShift = await probe.read('return await D.selfCheck();');
    check('平移之后屏幕 == 引擎帧', scShift.screenVsEngineDiffPx === 0,
              `差 ${scShift.screenVsEngineDiffPx} px`);

    // 页指纹的耗时比值同样在 tests/perf 里判；这里只留结构量：脏页数与页数。
    check('★ 页指纹的脏页范围跟着脏段走（不是整篇）',
              st.dirtyPageCount > 0 && st.dirtyPageCount < st.totalPages,
              `脏页 ${st.dirtyPageCount} / 共 ${st.totalPages} 页`);

    // 点第二页：不许乱滚。
    //   这一格坏在"caretY 是窗口帧内的 y"被当成了页内 y —— 页偏移算了两遍，
    //   光标坐标落到下一页去，屏幕跟着往下冲，随后窗口重算又滚一次。
    //   取点选在"光标当前所在的那一行"：它一定看得见，所以根本不该把页面挪走。
    const clickP2 = await probe.read(`
      const stage = EL('.heditor-stage');
      const dpr = window.devicePixelRatio || 1;
      await H.setCaret({ para: 20, offset: 5 });   // 第 20 段在 16 页的那一份里是第二页
      const cr0 = CARET();                      // 光标要页号与页内 y，公开面一次给全
      const before = stage.scrollTop;
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const sx = r.width / cv.width, sy = r.height / cv.height;
      // 画布代理的 rect 是"窗口帧"的坐标，而 cr0.y 是页内 y —— 加上页号偏移才是窗口内 y。
      const step = D.pagePx().h + D.pagePx().gap;
      const wy0 = (cr0.page - D.editStats().windowFirstPage) * step + cr0.y;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left + (cr0.x + 2) * sx, clientY: r.top + (wy0 + 2) * sy,
          bubbles: true, cancelable: true }));
      await H.flush();  // 异步宿主：命中+落光标要走往返
      const sc2 = await D.selfCheck();
      return { pageBefore: cr0.page, pageAfter: CARET().page,
               before, after: stage.scrollTop,
               stepCss: (D.pagePx().h + D.pagePx().gap) / dpr,
               wf: D.editStats().windowFirstPage, caretY: cr0.y,
               frameDiff: sc2.screenVsEngineDiffPx, box: sc2.screenVsEngineDiffBox,
               caretNow: CARET(), blit: D.blitStats() };
    `);
    check('★ 点第二页光标所在那一行：页不变、滚动量小于半页（不乱滚）',
              clickP2.pageBefore >= 1 && clickP2.pageAfter === clickP2.pageBefore &&
                Math.abs(clickP2.after - clickP2.before) <= clickP2.stepCss / 2,
              `光标页 ${clickP2.pageBefore} → ${clickP2.pageAfter} ｜ 滚动 ` +
                `${clickP2.before.toFixed(0)} → ${clickP2.after.toFixed(0)} px` +
                `（半页 ${(clickP2.stepCss / 2).toFixed(0)}）` +
                ` ｜ 窗口首页 ${clickP2.wf}，光标帧内 y=${clickP2.caretY}`);
    check(' 点第二页之后屏幕 == 引擎帧', clickP2.frameDiff === 0,
              `差 ${clickP2.frameDiff} px，盒=${JSON.stringify(clickP2.box)}` +
              `，上屏=${JSON.stringify(clickP2.blit)}`);

    // 滚到下面几页：窗口必须跟着可见页走，可见页必须有画布。
    //   这一格坏在"窗口被光标页钉住"——把光标页无条件并进窗口、再从 min 那一端起算，
    //   用户把屏幕滚到别处之后窗口仍锁在光标那几页：可见页没进窗口 ⇒ 一片空白，
    //   而且算出的锚点与上一轮相同 ⇒ applyWindow 返回 false、连 present 都不做。
    //   症状正是"滚动时下面的画布不生成，点一下才出来"（点击把光标带到那一页）。
    const scrollWin = JSON.parse(await probe.read(`
      return (async function () {
        const stage = EL('.heditor-stage');
        const dpr = window.devicePixelRatio || 1;
        const step = (D.pagePx().h + D.pagePx().gap) / dpr;
        const wait = () => new Promise((r) => requestAnimationFrame(
          () => requestAnimationFrame(r)));
        // 跳页之后要给窗口一个对齐的机会：滚动快过"渲染能力"时宿主会主动放弃追窗口
        //   （app.js 的 tooFastToFollow），等安静 140 ms 再对齐（alignAfterScroll）。
        //   这里每次跳 3 页、三跳连着做，正是会被判成"追不上"的形态 —— 不等就会读到
        //   还没对齐的那一帧。等的是"滚动停下来"，不是"等到断言成立"：安静 180 ms 之后
        //   仍旧读一次状态，对齐没发生的话下面照红。
        const settle = () => new Promise((r) => setTimeout(r, 180));
        const rows = [];
        for (const p of [3, 6, 9]) {
          stage.scrollTop = p * step;
          await settle();
          await wait();
          const vp = VIEWPORT();
          rows.push({ want: p, first: vp.engineFirst, count: vp.engineCount,
                      canvases: vp.canvases, visFirst: vp.visible.first,
                      visLast: vp.visible.last });
        }
        // 逐页下滚：这一步才是"平移复用"那一支（上面每次跳 3 页，重叠为 0，
        //   走的是整窗重建）。逐拍记下重画页数 —— 平移复用时它应当小于窗口页数。
        //   滚 8 页而不是 4 页：窗口带 3 页余量、且余量偏向滚动方向，所以往下滚 3 页
        //   才换一次窗（见 app.js 的 kWindowLookaheadPages）—— 滚 4 页只赶上一次换窗，
        //   而判据要求至少两次。页数取多少是"被测策略的余量"决定的，不是随手写的：
        //   余量再调大时这里要跟着调，否则判据会变成"恒红"而不是"恒绿"。
        stage.scrollTop = 0;
        await wait();
        const shiftSteps = [];
        for (let k = 1; k <= 8; ++k) {
          stage.scrollTop = k * step;
          // 与上面的跳页同一个口径：等的是"滚动停下来"，不是等断言成立。
          //   Worker 传输下这一拍要跨线程往返，只等两帧在机器有负载时会读到换窗前的账。
          await settle();
          await wait();
          const st = D.editStats();
          shiftSteps.push({ page: k, first: st.windowFirstPage, pages: st.windowPages,
                            repainted: st.repaintedPages, bands: st.bandY1 >= 0 });
        }
        return JSON.stringify({ rows, shiftSteps });
      })();
    `));
    for (const r of scrollWin.rows) {
      const covered = r.first <= r.visFirst && r.visLast <= r.first + r.count - 1;
      check(` 滚到第 ${r.want} 页：窗口跟着可见页走（${r.first}+${r.count}）`,
                covered, `可见 ${r.visFirst}~${r.visLast}｜窗口 ${r.first}+${r.count}` +
                         `｜画布 ${r.canvases} 张`);
    }

    // 逐页下滚时确实在"平移复用"：重画页数严格小于窗口页数。
    //   这一条是下面那两条自审的前提检查 —— 没有它，一次"跳 3 页"的滚动
    //   会让窗口重叠为 0、退化成整窗重建，于是底图自审恒绿，什么也没测到
    //   （反证做过：把平移量故意少搬 3 px，只有自审的那一版照样全绿）。
    //   判"至少有 2 步在复用"而不是"每一步都是"：回滚到文档顶部那一步位移可能
    //   超过一整个窗口，重叠为 0 时本来就该整窗重建。
    // 判"没有整窗重建"，不判"一定重画了某几页"：页位图有内容指纹缓存，滚进窗口的那几页
    //   可能整页命中缓存，此时重画页数为 0 —— 那比"只重画一部分"更省，同样是平移复用。
    //   整窗重建的形态是重画页数等于窗口页数（第一拍就是，跳页跳过头时必然如此）。
    const reusedSteps = scrollWin.shiftSteps.filter((s: any) => s.repainted < s.pages);
    check('逐页下滚走的是平移复用（重画页数 < 窗口页数）',
              reusedSteps.length >= 2,
              scrollWin.shiftSteps.map((s: any) =>
                `第${s.page}页 窗${s.first}+${s.pages} 重画${s.repainted}`).join(' ｜ '));

    // 换窗之后底图必须与"从零全建"逐字节相同。
    //   滚动换窗走的是平移复用（把重叠页的像素 memmove 过去、只重画新露出的页），
    //   最容易错的三种：搬错行、上一帧的光标/标记/选区跟着搬走变成幽灵、指纹没重新对齐。
    //   为什么不能用 auditFrame 顶替：那一条比的是帧，而帧是从底图搬的 ——
    //   底图整个错了，帧与"整帧重贴"照样一致，两条都绿而画面是错的。
    const abScroll = await probe.read('return await D.auditBase();');
    check('★★ 滚动换窗之后底图 == 从零全建（逐字节）',
              !!abScroll && abScroll.diff === 0,
              abScroll ? `差 ${abScroll.diff} px` +
                (abScroll.diff > 0 ? `（框 ${JSON.stringify(abScroll.box)}）` : '') : 'null');
    const afScroll = await probe.read('return await D.auditFrame();');
    check('★★ 滚动换窗之后引擎帧 == 整帧重画（逐字节）',
              !!afScroll && afScroll.diff === 0,
              afScroll ? `差 ${afScroll.diff} px` : 'null');
    const scScroll = await probe.read('return await D.selfCheck();');
    check('滚动换窗之后屏幕 == 引擎帧', scScroll.screenVsEngineDiffPx === 0,
              `差 ${scScroll.screenVsEngineDiffPx} px`);
  }

  log.section('[10] 跨段编辑：真实回车分段 / 段首退格合并');
  {
    // 用真实的 Enter 按键走一遍（不是直接调引擎）—— 键盘路径与引擎路径是两处接线，
    //   只测引擎会漏掉"宿主没把 Enter 接上"这类问题。
    await probe.read('return await LOAD.scenario(6, 1.0);');
    await probe.read('return await D.stopBlink();');
    const before = await probe.read('return D.editStats();');
    const textBefore = await probe.read('return H.text();');
    await probe.read('return await H.setCaret({ para: 0, offset: 5 });');
    await probe.read(key('Enter'));

    const after = await probe.read('return D.editStats();');
    const textAfter = await probe.read('return H.text();');
    state.splits = { parasBefore: before.totalParagraphs, parasAfter: after.totalParagraphs };
    check('★ 真实回车让段数 +1', after.totalParagraphs === before.totalParagraphs + 1,
              `${before.totalParagraphs} → ${after.totalParagraphs} 段`);
    check('★ 分段的字符账守恒（不丢不重）',
              textAfter.replace(/\n/g, '').length === textBefore.length,
              `前 ${textBefore.length} 字符 / 后 ${textAfter.replace(/\n/g, '').length} 字符（去换行）`);
    check('★ 分段之后光标在新段段首', after.caretPara === 1 && after.caretOffset === 0,
              `段 ${after.caretPara} 偏移 ${after.caretOffset}`);
    check('段数变了不让后缀复用（后缀段号已经错位）', after.reusedParagraphs === 0,
              `重排 ${after.relaidParagraphs} 段 / 复用 ${after.reusedParagraphs} 段`);

    // 排版与上屏两个自审都要过：分段走的是"前缀复用 + 从脏段续排"，最容易错的是段号错位
    const auSplit = await probe.read('return await D.auditLayout();');
    check('★★ 分段之后增量排版 == 全量重排（逐字段）',
              !!auSplit && auSplit.diff === 0,
              auSplit ? `diff ${auSplit.diff}（字形 ${auSplit.glyphDiff}、行 ${auSplit.lineDiff}、` +
                `段 ${auSplit.paraDiff}）` : 'null');
    const scSplit = await probe.read('return await D.selfCheck();');
    check('分段之后屏幕 == 引擎帧', scSplit.screenVsEngineDiffPx === 0,
              `差 ${scSplit.screenVsEngineDiffPx} px`);
    const afSplit = await probe.read('return await D.auditFrame();');
    check('分段之后引擎帧 == 整帧重画', afSplit && afSplit.diff === 0,
              afSplit ? `差 ${afSplit.diff} px` : 'null');

    // 段首退格并回去（同一个往返：文档要么回到原样，要么至少段数 -1）
    await probe.read('return await H.setCaret({ para: 1, offset: 0 });');
    await probe.read(key('Backspace'));
    const merged = await probe.read('return D.editStats();');
    check('★ 段首退格把两段并回去（段数 -1）',
              merged.totalParagraphs === after.totalParagraphs - 1,
              `${after.totalParagraphs} → ${merged.totalParagraphs} 段`);
    check('★ 合并之后光标落在合并点上', merged.caretPara === 0 && merged.caretOffset === 5,
              `段 ${merged.caretPara} 偏移 ${merged.caretOffset}`);
    const auMerge = await probe.read('return await D.auditLayout();');
    check('★★ 合并之后增量排版 == 全量重排（逐字段）', !!auMerge && auMerge.diff === 0,
              auMerge ? `diff ${auMerge.diff}` : 'null');
    check('合并之后文档回到分段之前的字符数',
              (await probe.read('return H.text();')).length === textBefore.length,
              `回到 ${textBefore.length} 字符`);

    // 把缩放还回去：[9] 与本节的 run(7/6, 1.0) 只是为了让长文档的窗口页数可控，
    //   而缩放是页面级状态。不还原的话，从 [11] 往后的每一节都在 1.0 下跑，
    //   state.zoom 里记的却是 1.5。后面各节都读实时的实际缩放来算坐标（自洽），
    //   只有依赖"文档有多高"的小节会失真：拖选出可视区那一节就是这样，文档矮了
    //   三分之一，自动滚动很快滚到底，"还在继续滚"于是永远量不出来。
    //   判据必须自己钉住前提，不靠前一节留下的状态。
    await probe.read(`return await LOAD.scenario(6, ${ZOOM});`);
  }

  log.section('[11] 连页：中段插一个空段，整条后缀跟着走');
  {
    // 连页的块轴是整篇累加的绝对坐标，页号由它现算；分页的块轴是页内坐标、放不下就
    //   翻页并把块轴挪回页顶。"后缀整体平移"那一趟只认分页那一套时，连页文档从第二
    //   页起的后缀每一行都判成放不下，整条被拽回条带顶部：中间空出一块、下面的接不上；
    //   后缀里带表时表从错误的块轴位置续排，整篇塌回第一页。
    //   判据用"增量 vs 整篇重排"对拍 + 后缀让出了多少：塌下去的那一版自己看着是自洽的，
    //   只有与全量比才露馅。
    const prep = await probe.read<any>(`
      await H.configure({ pageMode: 'continuous' });
      await H.flush();
      const lines = [];
      for (let i = 0; i < 220; ++i) lines.push('第' + i + '行');
      await H.setText(lines.join('\\n'));
      await H.flush();
      const g = H.geometry();
      const st = await D.editStats();
      return { pages: st.totalPages, step: g.page.h + g.gap,
               paras: (await H.paragraphs()).length };`);
    // 段首的全局块轴位置 = 页号 × 步距 + 页内 y（页内 y 是设备像素、已取整，所以下面
    //   比"让出多少"时留 2 px 的余量）。
    const axis = (para: number) => probe.read<number>(`
      await H.setCaret({ para: ${para}, offset: 0 });
      await H.flush();
      const q = CARET();
      return q.page * ${prep.step} + q.y;`);
    check('连页长文档确实跨了好几页', prep.pages >= 3, `${prep.pages} 页`);
    const a150 = await axis(150);
    const tail0 = await axis(151);
    const line = tail0 - a150;
    // 真实回车（不是往内容里塞一个换行符）：用户按的就是这个键。
    await probe.read('await H.setCaret({ para: 150, offset: 3 }); await H.flush(); return true;');
    await probe.read(key('Enter'));
    const st1 = await probe.read<any>('return D.editStats();');
    const au = await probe.read<any>('return await D.auditLayout();');
    const paras1 = await probe.read<number>('return (await H.paragraphs()).length;');
    // 回车插进第 150 段之后：该段内容整体后移一位，后缀从 152 段起。
    const tail1 = await axis(152);
    const back = await probe.read<number>(`
      let bad = 0;
      let prev = -1;
      for (let i = 0; i < 220; i += 7) {
        await H.setCaret({ para: i, offset: 0 });
        await H.flush();
        const q = CARET();
        const a = q.page * ${prep.step} + q.y;
        if (prev >= 0 && a < prev - 0.5) ++bad;
        prev = a;
      }
      return bad;`);
    check('★ 真实回车让段数 +1', paras1 === prep.paras + 1, `${prep.paras} → ${paras1} 段`);
    check('★★ 连页增量 == 整篇重排（逐字段）', !!au && au.diff === 0,
          au ? `diff ${au.diff}（字形 ${au.glyphDiff}、行 ${au.lineDiff}、段 ${au.paraDiff}）`
             : 'null');
    check('★ 页数没有塌回一页', st1.totalPages >= prep.pages,
          `${prep.pages} → ${st1.totalPages} 页`);
    check('★ 后缀只让出一行（让出一条带就是块轴被抹回条带顶）',
          Math.abs((tail1 - tail0) - line) < 2.0,
          `让出 ${(tail1 - tail0).toFixed(1)} px / 行距 ${line.toFixed(1)} px`);
    check('★ 整篇块轴一路递增（没有哪一段被拽到前面去）', back === 0, `${back} 处回头`);
    await probe.read(`await H.configure({ pageMode: 'paged' });
      await H.flush();
      return true;`);
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
