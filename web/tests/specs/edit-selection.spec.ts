// 编辑面（七）：选区与移动族、编辑入口吃选区、拖选、光标与选区样式、
// 光标与反白的纵向范围（取行槽，不取字面盒）、选区粒度（词与段）与拖拽搬运。
//
// 选区是引擎里的一等状态，不是宿主自己记的两对数：扩展只动焦点端、别的定位
// 一律折叠、上下键跨短行保住列位、矩形与反白都从同一个真源导出。
// 粒度决定一次连击选中多少，搬运是选区上的第二条命令路径 —— 两者都要真指针走一遍。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit, key, sleep, S6, ZOOM } from '../support/edit-helpers';

test.describe('编辑面：选区与样式', () => {
  test('选区的真源、编辑入口吃选区、拖选与两处样式都按契约工作', async ({ open }) => {
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

  log.section('[18] 选区与移动族：Shift 扩展、上下键、Ctrl+A、选区上屏');
  {
    // 这一节判的是"用户按键这一路"：全部派真实 keydown，不走门面调用 ——
    //   门面调用绕过的正是要判的那一段（宿主把 key + 修饰键翻译成引擎命令）。
    // 文档换成长段：上下键与"跨行拆矩形"必须真的跨行，一行装得下的文档测不出来。
    await probe.read(
      'await LOAD.text(' +
      JSON.stringify('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥') +
      ', 3); return true;');

    // (a) 装载之后没有选区 —— 反向一半：没有选区就没有要画的东西
    const s0 = await probe.read('return SEL();');
    check('装载之后没有选区（两端重合）', !!s0 && s0.empty === true, JSON.stringify(s0));
    check('★ 空选区零矩形（反向：没选区就没东西可画）',
              !!s0 && s0.rects.length === 0, JSON.stringify(s0 && s0.rects));

    // (b) Shift+右：选区从锚点那一端长
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const anchor0 = await probe.read('return SEL().anchor;');
    await probe.read(key('ArrowRight', ', shiftKey: true'));
    await probe.read(key('ArrowRight', ', shiftKey: true'));
    const s1 = await probe.read('return SEL();');
    check('★ Shift+右 之后有选区', s1.empty === false, JSON.stringify(s1));
    check('★ 锚点留在起点没动', s1.anchor === anchor0, `anchor=${s1.anchor} 原=${anchor0}`);
    check('焦点端前进两格', s1.focus === anchor0 + 2, `focus=${s1.focus}`);

    // (c) 连续扩展：锚点还是那一个（不是"每次重新起头"）
    await probe.read(key('ArrowRight', ', shiftKey: true'));
    const s2 = await probe.read('return SEL();');
    check('★ 连续 Shift+右 锚点仍不动', s2.anchor === anchor0, `anchor=${s2.anchor}`);
    check('焦点端再前进一格', s2.focus === s1.focus + 1, `focus=${s2.focus}`);

    // (d) Shift+左：反向扩展，锚点仍不动
    await probe.read(key('ArrowLeft', ', shiftKey: true'));
    const s3 = await probe.read('return SEL();');
    check('★ 反向扩展锚点也不动', s3.anchor === anchor0, `anchor=${s3.anchor}`);
    check('焦点端回退一格', s3.focus === s2.focus - 1, `focus=${s3.focus}`);

    // (e) 选区上屏：矩形真的落到了页面上
    check('★ 选区落到了矩形上', s3.rects.length >= 1, `rects=${s3.rects.length}`);
    if (s3.rects.length) {
      const r = s3.rects[0];
      check('矩形有正的宽高', r.w > 0 && r.h > 0, JSON.stringify(r));
    }
    // 像素级证据：同一份文档、同一相位，只看"有选区 / 无选区"两张帧。
    //
    //   尺子不能用"非白像素数（任一通道 < 200）"：选区填充是留给文字透出来的浅蓝
    //   （alpha 90），混到纸面上是 228/241/255 —— 三个通道都够亮，那把尺子按定义
    //   量不出它（实测差 +2，那只是字形抗锯齿边缘被压过阈值的那几个像素）。
    //   填充的指纹是"偏蓝"：b 明显高于 r。空选区时这个计数只来自次像素抗锯齿的边缘。
    //
    //   两条一起才算数，而且它们各自只钉一件事，能独立地翻红：
    //     偏蓝的像素变多 —— 有东西被涂上去了（没画 ⇒ 这条红）；
    //     变暗的像素基本不涨 —— 涂上去的是"洗"不是"块"，纸面与文字都还亮着
    //       （改成实心填充 ⇒ 这条红，而上一条照样绿）。
    //   所以门限取 r >= 170：实心填充的 r 是 178，仍然算"偏蓝"，
    //   两条才能各自只钉一件事，而不是一起红。
    const scanExpr = `
      const F = await PIX();
      const f = F.data;
      let ink = 0, blue = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const r = f[i], g = f[i + 1], b = f[i + 2];
        if (r < 200 || g < 200 || b < 200) ++ink;
        if (b - r >= 8 && r >= 170) ++blue;
      }
      return { ink, blue };`;
    await probe.read(
      'await H.setCaret({ para: 0, offset: 0 }); await D.stopBlink(); return true;');
    const pxNo = await probe.read(scanExpr);
    await probe.read(key('ArrowRight', ', shiftKey: true'));
    await probe.read(key('ArrowRight', ', shiftKey: true'));
    const pxSel = await probe.read(scanExpr);
    const dBlue = Number(pxSel.blue) - Number(pxNo.blue);
    const dInk = Number(pxSel.ink) - Number(pxNo.ink);
    check('★ 选区真的画进了帧（偏蓝的像素数变多）', dBlue >= 200,
              `有选区 ${pxSel.blue} vs 无选区 ${pxNo.blue}（差 ${dBlue}）`);
    check('★ 涂上去的是混合不是实心（纸面没被压暗）', dInk <= 50,
              `有选区 ${pxSel.ink} vs 无选区 ${pxNo.ink}（差 ${dInk}）`);
    const stSel = await probe.read('return D.editStats();');
    check('这一拍确实重画过（不是"只算了矩形没画"）', Number(stSel.repaintedPixels) > 0,
              `${stSel.repaintedPixels}`);

    // (f) 裸方向键把选区折叠
    await probe.read(key('ArrowRight'));
    const s4 = await probe.read('return SEL();');
    check('★ 裸方向键把选区折叠（不留看不见的选区）', s4.empty === true, JSON.stringify(s4));

    // (g) Ctrl+A 全选
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await probe.read(key('a', ', ctrlKey: true'));
    const sa = await probe.read('return SEL();');
    check('★ Ctrl+A 全选（走真事件）', sa.empty === false, JSON.stringify(sa));
    check('★ 全选跨行 ⇒ 多个矩形（整篇确实被选中）', sa.rects.length >= 2,
              `rects=${sa.rects.length}`);

    // (h) 上下键：↓↑ 回原位
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const cb = await probe.read('return CARET();');
    await probe.read(key('ArrowDown'));
    const cd = await probe.read('return CARET();');
    check('★ ↓ 走到了别的行（位置变了）', !(cd.para === cb.para && cd.offset === cb.offset),
              `${JSON.stringify(cb)} → ${JSON.stringify(cd)}`);
    await probe.read(key('ArrowUp'));
    const cu = await probe.read('return CARET();');
    check('★ ↓↑ 回到原位', cu.para === cb.para && cu.offset === cb.offset,
              `${JSON.stringify(cb)} → ${JSON.stringify(cu)}`);

    // (i) Shift+↓ 扩展（与 Shift+左右 共用同一个 collapse 口径）
    await probe.read(key('ArrowDown', ', shiftKey: true'));
    const sd = await probe.read('return SEL();');
    check('★ Shift+↓ 扩展出选区', sd.empty === false, JSON.stringify(sd));

    // (j) 文档首尾：Ctrl+Home / Ctrl+End（裸 Home/End 是行首行尾，见 [3]）
    const tp = await probe.read('return D.editStats().totalParagraphs;');
    await probe.read(key('End', ', ctrlKey: true'));
    const ce = await probe.read('return CARET();');
    check('Ctrl+End 落在最后一段', ce.para === Number(tp) - 1, `para=${ce.para} 共 ${tp} 段`);
    await probe.read(key('Home', ', ctrlKey: true'));
    const chh = await probe.read('return CARET();');
    check('Ctrl+Home 回到第一段段首', chh.para === 0 && chh.offset === 0,
              JSON.stringify(chh));
  }

  log.section('[19] 编辑入口吃掉选区：替换、退格只删选区、跨段删除、撤销回两端');
  {
    // 与 [18] 同一条纪律：全走真 keydown（含 Ctrl+A / Ctrl+Z / Ctrl+Y），
    //   不走门面调用 —— 门面调用绕过的正是"宿主把按键翻译成引擎命令"那一段。
    const DOC19 = '零一二三四五六七八九十';
    const load19 = () => probe.read(
      'await LOAD.text(' + JSON.stringify(DOC19) + ', 3); return true;');
    // 从 (0, 2) 起 Shift+右 两格 = 选中第 2、3 两个字。
    const selectTwo19 = async () => {
      await probe.read('await H.setCaret({ para: 0, offset: 2 }); return true;');
      await probe.read(key('ArrowRight', ', shiftKey: true'));
      await probe.read(key('ArrowRight', ', shiftKey: true'));
      return probe.read('return SEL();');
    };

    // (a) 选中再打字 = 原文 − 选区 + 新字
    await load19();
    const s19 = await selectTwo19();
    check('★ Shift+右 选中两个字（走真事件）',
              s19.empty === false && s19.focus === s19.anchor + 2, JSON.stringify(s19));
    await probe.read(key('测'));
    const t19 = await probe.read('return H.paragraph(0);');
    check('★ 选中再打字 = 原文 − 选区 + 新字', t19 === '零一测四五六七八九十', t19);
    const r19 = await probe.read('return SEL();');
    check('★ 替换之后没有残选区（反向：不许留一条看不见的选区）', r19.empty === true,
              JSON.stringify(r19));

    // (b) 撤销一次：文字与选区的两端一起回来
    await probe.read(key('z', ', ctrlKey: true'));
    const u19 = await probe.read(
      'return { t: await H.paragraph(0), s: SEL() };');
    check('★ 撤销把文字还回来了', u19.t === DOC19, u19.t);
    check('★ 撤销回到编辑前两端（不是只剩一个光标）',
              u19.s.empty === false && u19.s.anchor === s19.anchor && u19.s.focus === s19.focus,
              `期望 ${s19.anchor}..${s19.focus}，实际 ${u19.s.anchor}..${u19.s.focus}`);
    check('★ 回来的选区画得出来（有矩形）', u19.s.rects.length >= 1,
              `rects=${u19.s.rects.length}`);

    // (c) 重做：替换那一下整套回来
    await probe.read(key('y', ', ctrlKey: true'));
    const re19 = await probe.read(
      'return { t: await H.paragraph(0), s: SEL() };');
    check('★ 重做回到替换之后', re19.t === '零一测四五六七八九十', re19.t);
    check('重做之后没有选区', re19.s.empty === true, JSON.stringify(re19.s));

    // (d) 退格在选区非空时只删选区，不再往前多删一个字
    await load19();
    await selectTwo19();
    await probe.read(key('Backspace'));
    const b19 = await probe.read('return H.paragraph(0);');
    check('★ 退格只删掉选中的两个字（不多吃一个字）', b19 === '零一四五六七八九十', b19);

    // (e) 反向一半：没有选区时退格照旧删一个字。
    //     没有这一条，"退格什么都不做"也能让上面那条绿。
    await load19();
    await probe.read('await H.setCaret({ para: 0, offset: 3 }); return true;');
    await probe.read(key('Backspace'));
    const b19b = await probe.read('return H.paragraph(0);');
    check('★ 空选区时退格照旧删一个字', b19b === '零一三四五六七八九十', b19b);

    // (f) 前删同理
    await load19();
    await selectTwo19();
    await probe.read(key('Delete'));
    const d19 = await probe.read('return H.paragraph(0);');
    check('★ 前删也只删选区', d19 === '零一四五六七八九十', d19);

    // (g) 跨段：选区跨过段落标记，退格把两段并起来
    await load19();
    await probe.read('await H.setCaret({ para: 0, offset: 8 }); return true;');
    for (let i = 0; i < 5; ++i) await probe.read(key('ArrowRight', ', shiftKey: true'));
    const x19 = await probe.read('return SEL();');
    check('★ 选区跨过了段落标记（焦点进了下一段）', x19.empty === false, JSON.stringify(x19));
    await probe.read(key('Backspace'));
    const xa = await probe.read(
      'return { t: await H.text(),' +
      '         n: D.editStats().totalParagraphs };');
    check('★ 跨段退格把两段并成一段、内容接得上',
              xa.t === '零一二三四五六七一二三四五六七八九十\n零一二三四五六七八九十', xa.t);
    check('★ 段数少了一段（3 → 2）', Number(xa.n) === 2, `${xa.n}`);

    // (h) Ctrl+A 再退格：整篇删空，只剩一个空段
    await load19();
    await probe.read(key('a', ', ctrlKey: true'));
    await probe.read(key('Backspace'));
    const e19 = await probe.read(
      'return { t: await H.text(),' +
      '         n: D.editStats().totalParagraphs,' +
      '         s: SEL() };');
    check('★ Ctrl+A 再退格把整篇删空（只剩一个空段）',
              e19.t === '' && Number(e19.n) === 1, `${JSON.stringify(e19.t)} / ${e19.n} 段`);
    check('删空之后没有残选区', e19.s.empty === true, JSON.stringify(e19.s));

    // (i) 全选再打字 = 整篇只剩那个字
    await load19();
    await probe.read(key('a', ', ctrlKey: true'));
    await probe.read(key('测'));
    const f19 = await probe.read(
      'return { t: await H.text(),' +
      '         n: D.editStats().totalParagraphs };');
    check('★ 全选再打字 = 整篇只剩那个字', f19.t === '测' && Number(f19.n) === 1,
              `${JSON.stringify(f19.t)} / ${f19.n} 段`);

    // (j) 回车（带选区）：先删掉选区，再在落点分段
    await load19();
    await selectTwo19();
    await probe.read(key('Enter'));
    const g19 = await probe.read(
      'return { p0: await H.paragraph(0),' +
      '         p1: await H.paragraph(1),' +
      '         n: D.editStats().totalParagraphs };');
    check('★ 回车（带选区）：先删选区再分段（3 → 4 段）', Number(g19.n) === 4, `${g19.n}`);
    check('前半段是选区之前的内容', g19.p0 === '零一', g19.p0);
    check('后半段是选区之后的内容', g19.p1 === '四五六七八九十', g19.p1);

    // 收尾：把文档换回去，别把这一节的痕迹留给后面的节
    await probe.read(
      'await LOAD.text(' + JSON.stringify(S6) + ', 1); return true;');
  }

  log.section('[20] 拖选：按下、移动、抬起（真鼠标事件）');
  {
    // 为什么必须走 CDP 的真鼠标事件（与 [12] 同一条理由）：拖选这一格坏在
    //   "按下 → 命中 → 定位 → 移动 → 焦点端跟着走"这条链上，合成 MouseEvent 虽然
    //   也能打到监听器上，但只有真事件连坐标换算、命中、目标元素（画布）一起过一遍。
    //   另外两套宿主对"按下目标"的判断不同（单线程看坐标，Worker 看 event.target），
    //   合成事件很容易只在一侧成立。
    //
    // 本节自己钉住缩放：坐标换算读的是实时的实际缩放（自洽），但 (i) 那一节还依赖
    //   "文档有多高"（自动滚动要跑得够久），那是缩放决定的。不钉住的话，前一节把
    //   缩放留在别的值上，这一节就在另一种高度下跑，红项看起来像拖选坏了。
    await probe.read(`return await LOAD.reset(6, ${ZOOM});`);
    await probe.read('return await D.stopBlink();');

    const DOC20 = '零一二三四五六七八九十';
    const load20 = () => probe.read(
      'await LOAD.text(' + JSON.stringify(DOC20) + ', 3); return true;');

    // 光标几何 -> 客户端坐标。geo 每次现取：换文档、换窗口都会动。
    const geo20 = async () => probe.read(`
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const s = EL('.heditor-stage').getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               stageTop: s.top, stageBottom: s.bottom, stageRight: s.right, stageMidX: (s.left + s.right) / 2 };`);

    // caret.x / caret.y / caret.h 是帧像素（引擎那一侧乘过 zoom，见 [14] 的
    //   innerCaretPt），所以这里只差"帧像素转 CSS 像素"这一道换算，不能再乘 zoom；
    //   多乘一次只在 zoom 为 1 时是恒等。对照组：[14] 用的是 controls.items[].rects，
    //   那是帧磅，所以那边要乘 zoom。两种单位混用会让点击整体偏若干行，
    //   症状像命中测试点错了字。
    const toClient = (g: any, pt: any, dx: any, dy: any) => ({
      x: g.left + (pt.x + (dx || 0)) * g.sx,
      y: g.top + (pt.y + (dy || 0)) * g.sy,
    });
    const mouse = async (type: any, at: any, over?: any) => {
      const p = { x: Math.round(at.x), y: Math.round(at.y), button: 'left',
                  buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1, ...(over || {}) };
      await probe.send('Input.dispatchMouseEvent', { type, ...p });
    };
    // 走完一次完整手势：按下 → 若干次移动 → 抬起。中间那次 sleep 给异步宿主留往返。
    const dragGesture = async (from: any, to: any, steps = 4) => {
      await mouse('mousePressed', from);
      for (let i = 1; i <= steps; ++i) {
        await mouse('mouseMoved', {
          x: from.x + (to.x - from.x) * (i / steps),
          y: from.y + (to.y - from.y) * (i / steps),
        });
        await sleep(20);
      }
      await mouse('mouseReleased', to);
      await probe.read('return await D.dragSettled();');
      await sleep(30);
    };
    const sel20 = () => probe.read('return SEL();');
    const caret20 = () => probe.read('return CARET();');

    // (a) 量两个端点：文档末尾一个点、文档开头一个点。端点用键盘把光标摆过去再量，
    //     量完把选区收回来（setCaret 会清掉 anchor），免得量测本身影响了待测的手势。
    await load20();
    const g20 = await geo20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await probe.read(key('End', ', ctrlKey: true, shiftKey: true'));
    const ref20 = await sel20();                       // 键盘扩到文档末尾的答案
    const caretEnd = await caret20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const caretStart = await caret20();
    check('★ 参考值：Ctrl+Shift+End 扩到文档末尾（真 keydown）',
              ref20.empty === false && Number(ref20.focus) > Number(ref20.anchor),
              JSON.stringify(ref20));

    const ptStart = toClient(g20, caretStart, 1, caretStart.h / 2);
    const ptEnd = toClient(g20, caretEnd, 2, caretEnd.h / 2);

    // (b) 按下不拖：空选区（单击不该选出任何东西）
    await mouse('mousePressed', ptStart);
    await mouse('mouseReleased', ptStart);
    const s20a = await sel20();
    const c20a = await caret20();
    check('★ 按下不拖：没有选区（单击不等于选一个字）', s20a.empty === true,
              JSON.stringify(s20a));
    check('按下点就是光标位置', Number(c20a.para) === 0 && Number(c20a.offset) === 0,
              `${c20a.para},${c20a.offset}`);

    // (c) 按下 → 拖到文档末尾 → 抬起：两端与键盘扩到同一处逐值相同
    await dragGesture(ptStart, ptEnd);
    const s20b = await sel20();
    const c20b = await caret20();
    check('★ 拖出来的选区有内容', s20b.empty === false, JSON.stringify(s20b));
    check('★ 拖到文档末尾与键盘扩到文档末尾逐值相同（两端与方向）',
              Number(s20b.anchor) === Number(ref20.anchor) &&
              Number(s20b.focus) === Number(ref20.focus),
              `拖 ${s20b.anchor}..${s20b.focus} vs 键盘 ${ref20.anchor}..${ref20.focus}`);
    check('★ 焦点端落在文档末尾（光标跟着焦点端走）',
              Number(c20b.para) === 2 && Number(c20b.offset) === DOC20.length,
              `${c20b.para},${c20b.offset}（末段长 ${DOC20.length}）`);

    // (d) 反向拖：从末尾按下、拖回开头 —— 两端不交换（选区带方向）
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g20d = await geo20();
    await dragGesture(toClient(g20d, caretEnd, 2, caretEnd.h / 2),
                      toClient(g20d, caretStart, 1, caretStart.h / 2));
    const s20c = await sel20();
    check('★ 反向拖：anchor 在末尾、focus 在开头（方向没被交换）',
              Number(s20c.anchor) === Number(ref20.focus) &&
              Number(s20c.focus) === Number(ref20.anchor),
              JSON.stringify(s20c));

    // (e) 拖出去再拖回来：抬手时落在按下点，选区收成空（反向的一半：不许留下看不见的选区）
    const back = toClient(g20d, caretStart, 1, caretStart.h / 2);
    await mouse('mousePressed', back);
    await mouse('mouseMoved', toClient(g20d, caretEnd, 2, caretEnd.h / 2));
    await sleep(20);
    await mouse('mouseMoved', back);
    await mouse('mouseReleased', back);
    await probe.read('return await D.dragSettled();');
    const s20d = await sel20();
    check('★ 拖出去再拖回来收成空选区', s20d.empty === true, JSON.stringify(s20d));

    // (f) 抬起之后再移动鼠标不再改选区（反向的一半：拖动只在按着的时候有效）
    await mouse('mouseMoved', { x: ptEnd.x, y: ptEnd.y }, { buttons: 0 });
    await sleep(40);
    const s20e = await sel20();
    check('★ 抬起之后再移动不再改选区', s20e.empty === true, JSON.stringify(s20e));

    // (g) 拖选之后打字 = 选区被吃掉（与第 5 刀接上：拖出来的与键盘扩出来的是同一件东西）
    await load20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g20g = await geo20();
    const cs20 = await caret20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await probe.read(key('End', ', ctrlKey: true, shiftKey: true'));
    const ce20 = await caret20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await dragGesture(toClient(g20g, cs20, 1, cs20.h / 2),
                      toClient(g20g, ce20, 2, ce20.h / 2));
    await probe.read(key('测'));
    const g20h = await probe.read(
      'return { t: await H.text(),' +
      '         n: D.editStats().totalParagraphs };');
    check('★ 拖选之后打字：整篇被替换成一个字', g20h.t === '测' && Number(g20h.n) === 1,
              `${JSON.stringify(g20h.t)} / ${g20h.n} 段`);

    // (h) 页外的灰底起手不选中：按下点没落在正文上，拖动不该选中任何东西
    await load20();
    const g20i = await geo20();
    const outside = { x: g20i.stageRight - 8, y: g20i.stageTop + 30 };
    await dragGesture(outside, { x: outside.x - 4, y: outside.y + 60 });
    const s20f = await sel20();
    check('★ 页外灰底起手不选中（按下点必须在正文上）', s20f.empty === true,
              JSON.stringify(s20f));

    // (i) 拖出可视区：指针停在容器下缘之外，容器自己滚起来、选区继续长
    await probe.read('await LOAD.stress(200, 20); return true;');
    // 显式回到文档顶部：这一节要的是"往下还有得滚"，视口停在哪一层不该由前面的节决定。
    await probe.read('EL(".heditor-stage").scrollTop = 0; return true;');
    await sleep(250);
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g20j = await geo20();
    const cj = await caret20();
    const fromJ = toClient(g20j, cj, 1, cj.h / 2);
    const outJ = { x: g20j.stageMidX, y: g20j.stageBottom + 30 };
    // 前置：按下点要落在容器内（否则"拖出可视区"这一节无从发生），而且视口要真的
    //   在文档顶部、下面还有可滚的余量。这三条不成立时后面的断言会量到空值，
    //   那时再去看红项只会往命中测试上想。
    //
    // 第一条必须钉"接近 0"，不能只写"离底部还远"：top 接近上限时"离底部还远"照样
    //   成立，整节就会在已经滚到底的状态下跑，两次读数自然一模一样。
    const room = await probe.read(`
      const s = EL('.heditor-stage');
      return { top: s.scrollTop, max: s.scrollHeight - s.clientHeight };`);
    check('前置：视口钉在文档顶部、下面还有可滚余量，按下点在容器内',
              Number(room.top) <= 50 && Number(room.max) > 1000 &&
              fromJ.y > g20j.stageTop && fromJ.y < g20j.stageBottom,
              `scrollTop ${room.top}/${room.max}，按下点 y=${Math.round(fromJ.y)}，` +
              `容器 ${Math.round(g20j.stageTop)}..${Math.round(g20j.stageBottom)}`);

    // 量测必须落在自动滚动还没停的时候，不能先 await 收尾再量。
    //   dragSettled 等的是自动滚动停下来（滚到两端、或指针回到框内），而自动滚动
    //   一直把 raf 挂满，所以 await 它等于等到滚到底；那时量到的 j0 已经是终值，
    //   j1 自然一模一样，"还在继续滚"就量不出来了。
    const readStage = () => probe.read(`
      const s = EL('.heditor-stage');
      return { top: s.scrollTop, max: s.scrollHeight - s.clientHeight,
               scrollH: s.scrollHeight, clientH: s.clientHeight,
               zoom: ZOOM(),
               sel: SEL(),
               drag: D.dragState() };`);

    await mouse('mousePressed', fromJ);
    await mouse('mouseMoved', outJ);
    await sleep(220);                                   // 让它先跑几帧，但不到底
    const j0 = await readStage();
    await sleep(900);                                   // 继续滚、继续长
    const j1 = await readStage();
    await mouse('mouseReleased', outJ);
    await probe.read('return await D.dragSettled();');

    // 诊断面本身也要有内容：拖动中它必须说"在拖、已经移动过、发过若干次焦点端搬运"。
    check('★ 拖动中的诊断读数：在拖、移动过、搬过焦点端',
              j0.drag && j0.drag.active === true && j0.drag.moved === true &&
              Number(j0.drag.extends) > 0,
              JSON.stringify(j0.drag));
    check('★ 拖出可视区：容器自己滚起来了（指针没动）',
              Number(j0.top) > 0, `scrollTop ${j0.top}（上限 ${j0.max}）`);
    check('★ 拖出可视区：还在继续滚（不是一次到位）',
              Number(j1.top) > Number(j0.top),
              `${j0.top} -> ${j1.top}（上限 ${j1.max}，滚动高 ${j1.scrollH}，` +
              `视口高 ${j1.clientH}，zoom ${j1.zoom}，指针 y=${Math.round(outJ.y)}）`);
    check('★ 拖出可视区仍继续扩展：焦点端继续往后走',
              Number(j1.sel.focus) > Number(j0.sel.focus),
              `${j0.sel.focus} -> ${j1.sel.focus}`);
    check('★ 这一节没滚到底（滚到底是靠自动滚动、不是被别的东西拽过去）',
              Number(j1.top) < Number(j1.max),
              `${j1.top} / ${j1.max}`);
    const d20 = await probe.read('return D.dragState();');
    check('★ 抬手之后拖选收尾（诊断面回到不在拖）', d20 && d20.active === false,
              JSON.stringify(d20));

    // (j) 抬起事件丢了也要收尾，否则拖选状态一直留着。
    //   真实成因都是"页面收不到 mouseup"（在浏览器窗口之外松手、被别的窗口抢走、
    //   被浏览器接管成原生拖放），那些场景 CDP 造不出来。能造的是它的后果：
    //   移动事件里已经带着"按钮抬起了"。
    //
    //   这一条的刺激必须是页内合成的：给 CDP 的 mouseMoved 传 buttons=0 没用 ——
    //   Chrome 用它自己的按钮状态覆盖那个字段（实测读到的还是 1，于是那条移动把选区
    //   又扩了一格）。被测的只是"onMove 读到按钮已抬起就收尾"这一条，
    //   坐标换算与命中都不在它里面，所以合成事件在这里是恰当的刺激而不是绕过接线。
    await probe.read('await LOAD.stress(200, 20); return true;');
    await probe.read('EL(".heditor-stage").scrollTop = 0; return true;');
    await sleep(250);
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g20k = await geo20();
    const ck20 = await caret20();
    const pk20 = toClient(g20k, ck20, 1, ck20.h / 2);
    const outK = { x: g20k.stageMidX, y: g20k.stageBottom + 30 };
    // 页内派一条移动事件（按钮位可指定）—— 见上面为什么必须合成。
    const synthMove = (x: any, y: any, buttons: any) => probe.read(
      'window.dispatchEvent(new MouseEvent("mousemove", { clientX: ' + Math.round(x) +
      ', clientY: ' + Math.round(y) + ', buttons: ' + buttons +
      ', bubbles: true, cancelable: true })); return true;');
    const readK = () => probe.read(
      'const s = EL(".heditor-stage");' +
      'return { drag: D.dragState(), top: s.scrollTop };');
    await mouse('mousePressed', pk20);
    await mouse('mouseMoved', outK);                      // 拖到容器外：自动滚动起来
    await sleep(300);
    const k0 = await readK();
    await synthMove(outK.x, outK.y, 0);                   // 抬起丢了，但人还在动鼠标
    await sleep(80);
    const k1 = await readK();
    await sleep(300);
    const k2 = await readK();
    check('★ 前置：拖到容器外确实在自动滚动（否则这一节测了个空）',
              k0.drag && k0.drag.active === true && k0.drag.autoScroll === true,
              JSON.stringify(k0.drag));
    check('★ 抬起事件丢失时，下一次移动（按钮已抬起）也收尾',
              k1.drag && k1.drag.active === false,
              `收尾前 ${JSON.stringify(k0.drag)} → 之后 ${JSON.stringify(k1.drag)}`);
    check('★ 收尾之后自动滚动也停了（松手后画面不再自己滚）',
              k2.top === k1.top, `${k1.top} → ${k2.top}`);
    await mouse('mouseReleased', outK);                   // 收尾：把按下状态也放掉

    // (k) pointercancel 也要收尾：触摸被识别成滚动、或笔被系统抢走时只剩这一个信号。
    await load20();
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g20m = await geo20();
    const cm20 = await caret20();
    await mouse('mousePressed', toClient(g20m, cm20, 1, cm20.h / 2));
    await mouse('mouseMoved', { x: g20m.stageMidX, y: g20m.stageTop + 120 });
    await sleep(60);
    const m0 = await probe.read('return D.dragState();');
    await probe.read('window.dispatchEvent(new PointerEvent("pointercancel",' +
                 ' { bubbles: true, cancelable: true })); return true;');
    await sleep(60);
    const m1 = await probe.read('return D.dragState();');
    check('★ pointercancel 也收尾（触摸/笔被系统接管）',
              m0 && m0.active === true && m1 && m1.active === false,
              `${JSON.stringify(m0)} → ${JSON.stringify(m1)}`);
    await mouse('mouseReleased', { x: g20m.stageMidX, y: g20m.stageTop + 120 });

    // 收尾：把文档换回去，别把这一节的痕迹留给后面的节
    await probe.read(
      'await LOAD.text(' + JSON.stringify(S6) + ', 1); return true;');
  }

  log.section('[21] 光标与选区的样式：可配置的参数');
  {
    await probe.read(
      'await LOAD.text(' + JSON.stringify(S6) + ', 1); return true;');
    // 顺序有讲究：stopBlink 只把相位写成"可见"，不重绘（它要留着上一次全量的账）；
    //   所以要在它之后再落一次光标 —— 那一拍 present 才会把光标画进帧里。反过来读到
    //   的是"相位正好关着"的那一帧，光标像素是白的，与机器负载无关。
    await probe.read('return await D.stopBlink();');
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');

    // 采样点取光标矩形中心偏下 3 px：一定在光标内，又躲开上缘的取整边界。
    //   读的是页画布（PIX 逐页取再按窗口拼），不是引擎导出的帧 —— 后者是被测对象的自述。
    const FRAME_PIX = `
      const F = await PIX();
      const f = F.data;
      const w = F.w, h = F.h;
      const cr = CARET();
      const x = cr.x + Math.max(0, Math.floor(cr.w / 2));
      const y = cr.y + Math.min(3, Math.max(0, cr.h - 1));
      if (!(x >= 0 && y >= 0 && x < w && y < h)) return null;
      const i = (y * w + x) * 4;
      return [f[i], f[i + 1], f[i + 2], f[i + 3]];`;
    const frameArea = await probe.read(`const g = H.geometry(); const p = D.pagePx();
      return p.w * Math.max(1, g.window.count) * (p.h + p.gap);`);

    const defCaret = await probe.read('return H.config().caretStyle;');
    const defSel = await probe.read('return H.config().selectionStyle;');
    const p0 = await probe.read(FRAME_PIX);
    check('默认光标色就是配置里那一份（0,0,0）',
              !!p0 && p0[0] === 0 && p0[1] === 0 && p0[2] === 0,
              `采样 ${JSON.stringify(p0)}，配置 ${JSON.stringify(defCaret.color)}`);

    // 光标宽度与 Word 同一档：0.5 磅。排版一磅在这个倍率下是一个设备像素，
    //   一磅以上会在高分屏上糊成两像素宽的一条。下限 0.4 之上才留得住这个取值。
    const caretW = await probe.read('return { w: CARET().w, z: D.stats().density };');
    check('默认光标线宽 0.5 磅，画出来一个设备像素',
              defCaret.widthPt === 0.5 && caretW.w === 1,
              `配置 ${defCaret.widthPt} 磅，画出来 ${caretW.w} px（密度 ${caretW.z}）`);

    const st1 = await probe.read(
      'return await CARET_STYLE({ color: [220, 20, 60, 255] });');
    const p1 = await probe.read(FRAME_PIX);
    const back1 = await probe.read('return H.config().caretStyle;');
    check('改光标色：帧上那个像素变成新颜色（不只是改了参数）',
              !!p1 && p1[0] === 220 && p1[1] === 20 && p1[2] === 60 &&
              !!p0 && (p0[0] !== p1[0] || p0[1] !== p1[1] || p0[2] !== p1[2]),
              `${JSON.stringify(p0)} → ${JSON.stringify(p1)}`);
    check(' 读回来的光标样式与设进去的一致',
              back1.color[0] === 220 && back1.color[1] === 20 && back1.color[2] === 60 &&
              back1.color[3] === 255 && back1.widthPt === defCaret.widthPt,
              JSON.stringify(back1));

    // 参考臂：整篇重排重绘那一拍写多少像素。没有它，"改样式那拍写得少"这句无从比较
    //   —— 一个恒报 0 的计数器也会让它成立。
    //   先切到另一个场景再切回来：run(同 id) 那一步在"这个场景已经装着"时是空操作，
    //   读回来的会变成上一拍（改样式那一拍）的 186 px —— 那样参考臂就恒等于被测对象，
    //   判据恒红。换文档是保证这一拍真的是整篇重排重绘的唯一稳当办法。
    const fullPix = await probe.read(`
      await CARET_STYLE({ color: [31, 111, 235, 255] });
      await LOAD.scenario(7, ${ZOOM});
      const s = await LOAD.scenario(6, ${ZOOM});
      return s.repaintedPixels;`);
    // 分母是窗口内几页的像素，不是整个帧：帧按页号分槽之后帧高 = 槽数 x 步距，
    //   槽数可以多于窗口页数（多出来的是空槽，永远不写），拿 frameArea 当分母会让这条
    //   判据随槽数一起松掉（空槽越多越松，最后恒真）。
    const winArea = await probe.read(`
      const st = D.editStats() || {};
      return (st.windowPages || 1) * D.pagePx().w * D.pagePx().h;`);
    check('参考臂：整篇重排重绘确实写了大半个窗口（否则下面那条判据是恒真的）',
              fullPix > winArea * 0.5, `写 ${fullPix} / 窗口 ${winArea} px（帧 ${frameArea}）`);
    const st2 = await probe.read(
      'return await CARET_STYLE({ color: [0, 128, 0, 255] });');
    check('★ 改样式那一拍的写入量比整篇重绘少一个量级以上（没有整窗重光栅）',
              st2.repaintedPixels * 10 < fullPix,
              `改样式写 ${st2.repaintedPixels} px，整篇重绘写 ${fullPix} px，` +
              `帧 ${frameArea} px`);
    check(' 改样式不脏页（底图一个字都没变）', st2.dirtyPageCount === 0,
              `脏页 ${st2.dirtyPageCount}`);
    check(' 改样式不重排', st2.layoutRan === false && st1.layoutRan === false,
              `layoutRan ${st1.layoutRan} / ${st2.layoutRan}`);

    // 宽度下限：引擎把它夹到 0.4 —— 读回来必须是夹过的那一份，不是递进去的那一份。
    await probe.read('return await CARET_STYLE({ widthPt: 0.1 });');
    const wClamp = await probe.read('return H.config().caretStyle;');
    check('光标宽度下限被夹到 0.4 磅（递 0.1 进去）', wClamp.widthPt === 0.4,
              `读回 ${wClamp.widthPt}`);
    await probe.read('return await CARET_STYLE({ widthPt: 3 });');
    const wSet = await probe.read('return H.config().caretStyle;');
    check(' 宽度在范围内的取值原样保留（不是恒被夹到下限）', wSet.widthPt === 3,
              `读回 ${wSet.widthPt}`);

    // 宽度参数确实改变了画出来的东西：数整帧上等于光标色的像素，那个面积就是光标的
    //   可见大小。只读回 widthPt 只能证明引擎存下了这个数，证明不了它被画出来 ——
    //   而"存下来但没画"正是这类参数最常见的失败方式。
    const caretArea = async (w: any) => probe.read(`
      await CARET_STYLE({ color: [0, 128, 0, 255], widthPt: ${w} });
      const F = await PIX();
      const f = F.data;
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        if (f[i] === 0 && f[i + 1] === 128 && f[i + 2] === 0) ++n;
      }
      return n;`);
    const areaThin = await caretArea(0.4);
    const areaThick = await caretArea(6);
    check('光标宽度参数改变了画出来的粗细（整帧上光标色的像素面积）',
              areaThin > 0 && areaThick > areaThin * 5,
              `0.4 磅 ${areaThin} px，6 磅 ${areaThick} px`);

    // 通道越界：递进去 300 / -5，必须夹到 255 / 0 而不是回绕。
    await probe.read('return await SEL_STYLE({ color: [300, -5, 0, 0] });');
    const cClamp = await probe.read('return H.config().selectionStyle;');
    check('色通道越界被夹到 0~255（不回绕）',
              JSON.stringify(cClamp.color) === JSON.stringify([255, 0, 0, 0]),
              JSON.stringify(cClamp.color));

    // 选区：alpha 255 + 纯红 ⇒ 选区矩形内任一点都是那个红。用实心色是为了可判 ——
    //   半透明混出来的值依赖 blend 的取整口径，那不该由这条判据来钉。
    //
    // 单位：selection.rects 的 x/y/w/h 是页面磅（另带 page），而帧是帧像素 ——
    //   两套单位混用只在 zoom=1 时是恒等，取中心点会取到行外去（读回一片白）。见名词对照。
    const selPix = await probe.read(`
      await SEL_STYLE({ color: [255, 0, 0, 255] });
      await H.selectAll();
      const F = await PIX();
      const f = F.data;
      const w = F.w, h = F.h;
      const z = D.stats().density;
      const step = D.pagePx().h + D.pagePx().gap;
      const first = D.editStats().windowFirstPage;
      const r = SEL().rects[0];
      const x = Math.floor((r.x + r.w / 2) * z);
      const y = Math.floor((r.page - first) * step + (r.y + r.h / 2) * z);
      const i = (y * w + x) * 4;
      return { px: [f[i], f[i + 1], f[i + 2], f[i + 3]], n: SEL().rects.length,
               at: [x, y], inFrame: x >= 0 && y >= 0 && x < w && y < h, z, step, first };`);
    check('改选区色：选区矩形内那个像素就是新颜色',
              selPix.inFrame && selPix.n > 0 &&
              JSON.stringify(selPix.px) === JSON.stringify([255, 0, 0, 255]),
              `${selPix.n} 条矩形，取 ${JSON.stringify(selPix.at)}（zoom ${selPix.z}）` +
              ` 得到 ${JSON.stringify(selPix.px)}`);
    const selBack = await probe.read(`
      await SEL_STYLE({ color: ${JSON.stringify(defSel.color)} });
      const F = await PIX();
      const f = F.data;
      const i = (${selPix.at[1]} * F.w + ${selPix.at[0]}) * 4;
      return [f[i], f[i + 1], f[i + 2], f[i + 3]];`);
    check(' 换回默认的半透明色之后同一点不再是纯红（说明上一条读的是真像素）',
              !(selBack[0] === 255 && selBack[1] === 0 && selBack[2] === 0),
              `读回 ${JSON.stringify(selBack)}`);

    // 收尾：把两处样式恢复成引擎的默认值，别把这一节的痕迹留给后面的节和截图。
    await probe.read(
      'await CARET_STYLE({ color: ' + JSON.stringify(defCaret.color) +
      ', widthPt: ' + defCaret.widthPt + ' });' +
      'await SEL_STYLE({ color: ' + JSON.stringify(defSel.color) +
      ' });' +
      'await H.clearSelection(); return true;');
    const restored = await probe.read(
      'return { c: H.config().caretStyle, s: H.config().selectionStyle };');
    check('收尾：两处样式已恢复成默认',
              JSON.stringify(restored.c.color) === JSON.stringify(defCaret.color) &&
              JSON.stringify(restored.s.color) === JSON.stringify(defSel.color),
              JSON.stringify(restored));
  }

  log.section('[22] 光标与反白的纵向范围：取行槽，不取字面盒');
  {
    // 一条行盒有两个纵向范围。字面盒（ascent..descent）管字形与命中，行槽（含行距）管光标
    //   与反白。Word 与浏览器的插入点、反白都跟着行槽走；按字面盒画时，行距一加大，
    //   同一段的两行反白之间就露出一道白缝，光标也比一行短一截。
    //
    // 三个读数各钉一件事，任一条都能独立翻红：反白高度等于行距、相邻两行的矩形首尾相接
    //   （行距没有落成缝）、光标高度与反白同高（两者出自同一个来源）。
    const long = '甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥'.repeat(6);
    await probe.read('await LOAD.text(' + JSON.stringify(long) + ', 1); return true;');
    await probe.read('return await D.stopBlink();');

    const readBands = (setFactor: string) => probe.read(`
      ${setFactor}
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      const c = CARET();
      await H.selectAll();
      await H.flush();
      return { z: ZOOM(), caret: { h: c.h, w: c.w },
               rects: SEL().rects.map((r) => ({ page: r.page, x: r.x, y: r.y, w: r.w, h: r.h })) };`);
    const factor = (f: number) => `await H.selectAll();
      await H.paragraphFormat({ lineSpacing: { rule: 'auto', factor: ${f} } });
      await H.flush();`;
    // 段与段之间的缝：上一行矩形底边到下一行矩形顶边的距离，逐对都该是 0。
    const seams = (b: any): number[] =>
      b.rects.slice(1).map((r: any, i: number) => r.y - (b.rects[i].y + b.rects[i].h));
    const worst = (xs: number[]) => xs.reduce((m, v) => Math.max(m, Math.abs(v)), 0);

    const one = await readBands('');
    check('前提：这一段折成三行以上',
              one.rects.length >= 3, `实际 ${one.rects.length} 条反白`);
    const pitch1 = one.rects.length > 1 ? one.rects[1].y - one.rects[0].y : 0;
    check('单倍行距下行槽等于字面盒（这些字体没有行间隙）',
              Math.abs(pitch1 - one.rects[0].h) < 0.05,
              `行距 ${pitch1}，反白高 ${one.rects[0].h}`);

    const tall = await readBands(factor(1.5));
    check('★ 1.5 倍：相邻两行反白首尾相接，行距没有落成缝',
              tall.rects.length >= 3 && worst(seams(tall)) < 0.05,
              JSON.stringify(seams(tall)));
    check('★ 1.5 倍：反白高度等于行距（取的是行槽，不是字面盒）',
              Math.abs(tall.rects[0].h - pitch1 * 1.5) < 0.05,
              `反白高 ${tall.rects[0].h}，行距应为 ${pitch1 * 1.5}`);
    check('★ 光标高度与反白同高（两者都取行槽）',
              Math.abs(tall.caret.h - tall.rects[0].h * tall.z) <= 1.5,
              `光标 ${tall.caret.h} px，反白 ${tall.rects[0].h} 磅 x 密度 ${tall.z}`);
    check('★ 光标跟着行距一起变高（1.5 倍）',
              Math.abs(tall.caret.h / one.caret.h - 1.5) < 0.1,
              `单倍 ${one.caret.h} px，1.5 倍 ${tall.caret.h} px`);

    // 压缩方向也要跟行距走：倍数小于 1 时反白比单倍矮。只钉"变大"的话，
    //   把高度写成 max(行距, 字面盒) 也能全绿。
    const tight = await readBands(factor(0.8));
    check('★ 0.8 倍：反白压到单倍的八成（方向没搞反）',
              Math.abs(tight.rects[0].h - one.rects[0].h * 0.8) < 0.05,
              `单倍 ${one.rects[0].h}，0.8 倍 ${tight.rects[0].h}`);
    check('反向：压行时相邻两行照样首尾相接',
              tight.rects.length >= 3 && worst(seams(tight)) < 0.05,
              JSON.stringify(seams(tight)));

    // 像素级证据：从第一行的顶扫到第二行的底，逐行都该有反白的颜色。按字面盒画时，
    //   中间那 8.7 磅的行距是一条纯白的横带，逐行计数当场掉到 0。
    //   反白是浅蓝（半透明填充混在纸上）：蓝明显高过红，而纸面与字形三通道相等，不会误计。
    const SCAN = `
      await H.selectAll();
      await H.flush();
      const z = ZOOM();
      const r = SEL().rects;
      const a = r[0], b = r[1];
      const step = D.pagePx().h + D.pagePx().gap;
      const first = D.editStats().windowFirstPage;
      const F = await PIX();
      const f = F.data;
      const w = F.w;
      // 两条反白的公共 x 区间：短的那一行不会让另一行的涂色越界。
      const x0 = Math.max(0, Math.floor(Math.max(a.x, b.x) * z));
      const x1 = Math.min(w, Math.ceil(Math.min(a.x + a.w, b.x + b.w) * z));
      const y0 = Math.floor(a.y * z);
      const y1 = Math.ceil((b.y + b.h) * z);
      const top = Math.floor((a.page - first) * step);
      let inkRows = 0, least = 1e9;
      for (let y = y0; y < y1; ++y) {
        let n = 0;
        for (let x = x0; x < x1; ++x) {
          const i = ((top + y) * w + x) * 4;
          if (f[i + 2] > f[i] + 20) ++n;
        }
        if (n > 0) ++inkRows;
        if (n < least) least = n;
      }
      return { inkRows, rows: y1 - y0, least, x0, x1, y0, y1, z, n: r.length };`;
    await probe.read(factor(1.5) + 'return true;');
    const seam = await probe.read(SCAN);
    check('★ 1.5 倍行距下两条反白之间没有一道白缝（逐行都涂上了）',
              seam.inkRows === seam.rows && seam.least > 10, JSON.stringify(seam));

    // 反向一半：同一把尺子必须量得出"该有的缝"。段与段之间只有段后距，那里本来就该是白的
    //   —— 否则上面那条只能证明"整片都是蓝的"，证明不了它认得出缝隙。
    const across = await probe.read(`
      await LOAD.text('甲\\n乙', 1);
      await D.stopBlink();
      ${SCAN}`);
    check('反向：段与段之间那道空白照旧是白的（尺子认得出缝）',
              across.rows - across.inkRows >= 4, JSON.stringify(across));

    await probe.read('return await LOAD.text(' + JSON.stringify(long) + ', 1);');
  }

    await finishEdit(probe, consoleErrors, state);
  });
});

test.describe('编辑面：选区粒度', () => {
  test('真鼠标：单击不选、双击选词、三击选段', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 两段：第一段纯汉字（整类连着走），第二段拉丁文（按空格与字母分词的边界更清楚）。
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁\\none two three');
      await H.flush();
      await H.setCaret({ para: 1, offset: 5 });
      await H.flush();
      await D.stopBlink();
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const q = CARET();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               x: q.x, y: q.y, h: q.h };`);
    const at = { x: geo.left + geo.x * geo.sx, y: geo.top + (geo.y + geo.h / 2) * geo.sy };

    // 单击：没有选区（连击计数从这里起算，下面两下才是第二、第三击）。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const one = await p.read<any>(`return SEL();`);
    check('单击不产生选区', one.empty === true, JSON.stringify(one));

    // 第二击：选词。'two' 是三个字母。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const two = await p.read<any>(`return SEL();`);
    check('★ 双击选词：选中的就是这个词（3 个字母）',
          two.empty === false && Math.abs(Number(two.focus) - Number(two.anchor)) === 3,
          JSON.stringify(two));

    // 第三击：选整段。'one two three' 整段 13 个字符，明显多于一个词。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const three = await p.read<any>(`return { sel: SEL(), text: await H.text() };`);
    const len = Math.abs(Number(three.sel.focus) - Number(three.sel.anchor));
    check('★ 三击选整段：选区远长于一个词', three.sel.empty === false && len >= 13,
          `长度 ${len} / ${JSON.stringify(three.text)}`);
    check('三击不越出这一段（还是 2 段）', String(three.text).split('\n').length === 2,
          JSON.stringify(three.text));
  });
});

test.describe('编辑面：拖拽搬运', () => {
  test('真鼠标：按住选中的文字拖到别处（一步撤销）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 用两处光标坐标算出"按住哪儿、放到哪儿"：起点在选区里，终点在段末。
    const geo = await p.read<any>(`
      await H.setText('abcdef');
      await H.flush();
      const at = async (off) => {
        await H.setCaret({ para: 0, offset: off });
        await H.flush();
        const q = CARET();
        return { x: q.x, y: q.y, h: q.h };
      };
      const cv = FRAME(), r = cv.getBoundingClientRect();
      await D.stopBlink();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               o2: await at(2), o3: await at(3), o4: await at(4), o6: await at(6) };`);
    const P = (q: any) => ({
      x: geo.left + q.x * geo.sx,
      y: geo.top + (q.y + q.h / 2) * geo.sy,
    });

    // 先用真鼠标拖出 "cd"（off 2 → 4）。
    const a = P(geo.o2);
    const b = P(geo.o4);
    await p.page.mouse.move(a.x, a.y);
    await p.page.mouse.down();
    await p.page.mouse.move(b.x, b.y, { steps: 4 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(120);
    const sel = await p.read<any>(`return SEL();`);
    check('前置：拖出了两个字', sel.empty === false && Math.abs(sel.focus - sel.anchor) === 2,
          JSON.stringify(sel));

    // 按住选区中间（off 3，离上一次按下够远，不会被当成双击）拖到段末。
    const from = P(geo.o3);
    const to = P(geo.o6);
    await p.page.mouse.move(from.x, from.y);
    await p.page.mouse.down();
    await p.page.mouse.move(to.x + 20, to.y, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(200);
    const moved = await p.read<any>(`return await H.text();`);
    check('★ 选中的两个字搬到了段末（abefcd）', moved === 'abefcd', JSON.stringify(moved));

    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return await H.text();`);
    check('★ 一步撤销把搬走的字放回原处', undone === 'abcdef', JSON.stringify(undone));
  });

  test('拖动搬运：只留一个落点指示，跟着指针走，松手就收起', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('一二三四五六七八九十\\n甲乙丙丁戊己庚辛壬癸');
      await H.flush();
      await D.stopBlink();
      const at = async (para, offset) => {
        await H.setCaret({ para, offset });
        await H.flush();
        const c = CARET();
        return { x: c.x, y: c.y, h: c.h };
      };
      const a = await at(0, 1);
      const b = await at(0, 5);
      const d = await at(1, 3);
      const fr = FRAME().getBoundingClientRect();
      return { a, b, d, left: fr.left, top: fr.top };`);
    const P = (q: any) => ({ x: geo.left + q.x + 1, y: geo.top + q.y + q.h / 2 });
    const a = P(geo.a);
    const b = P(geo.b);
    // 先拖选一段（a → b），松手。
    await p.page.mouse.move(a.x, a.y);
    await p.page.mouse.down();
    await p.page.mouse.move(b.x, b.y, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(250);
    const sel = await p.read<any>(`return SEL();`);
    check('先拖出一段选区', sel.empty === false, JSON.stringify(sel));

    // 再按住选区中间往外拖：这一下才算"搬"。
    await p.page.mouse.move((a.x + b.x) / 2, a.y);
    await p.page.mouse.down();
    await p.page.mouse.move((a.x + b.x) / 2 + 26, a.y, { steps: 4 });
    await p.page.waitForTimeout(200);
    const mid = await p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      const st = CARET();
      return { bars: divs.map((e) => ({ d: e.style.display, l: e.style.left, t: e.style.top })),
               caretVisible: CARET().visible, stats: D.editStats().caretVisible };`);
    const shown = mid.bars.filter((x: any) => x.d === 'block');
    check('★ 拖动中有一个落点竖条', shown.length === 1, JSON.stringify(mid.bars));
    check('★ 拖动中闪的那颗光标收起来了（页面上只有一个光标）',
          mid.caretVisible === false && mid.stats === false,
          mid.caretVisible + ' / ' + mid.stats);
    // 与编辑光标一样宽：宽一档就会读成"另一个东西"。
    const width = await p.read<any>(`
      const bar = [...EL('.heditor-pages').children].find(
        (e) => e.tagName === 'DIV' && e.style.display === 'block');
      const caretW = CARET().w / (H.geometry().dpr || 1);
      return { bar: bar ? parseFloat(getComputedStyle(bar).width) : -1, caret: caretW };`);
    check('★ 落点竖条与编辑光标一样宽', Math.abs(width.bar - width.caret) <= 1,
          width.bar + ' vs ' + width.caret);

    // 落点跟着指针走：拖到第二段上，竖条也要往下。
    const firstTop = shown.length ? Number(String(shown[0].t).replace('px', '')) : -1;
    await p.page.mouse.move(P(geo.d).x, P(geo.d).y, { steps: 6 });
    await p.page.waitForTimeout(200);
    const mid2 = await p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      return divs.filter((e) => e.style.display === 'block').map((e) => e.style.top);`);
    check('★ 落点竖条跟着指针走（换到第二段之后往下移）',
          mid2.length === 1 && Number(String(mid2[0]).replace('px', '')) > firstTop,
          firstTop + ' → ' + JSON.stringify(mid2));

    await p.page.mouse.up();
    await p.page.waitForTimeout(300);
    const after = await p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      return { bars: divs.map((e) => e.style.display), caret: CARET().visible,
               text: await H.selectAllText() };`).catch(async () => p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      return { bars: divs.map((e) => e.style.display), caret: CARET().visible };`));
    check('★ 松手之后落点竖条收起', after.bars.every((d: string) => d === 'none'),
          JSON.stringify(after.bars));
    check('松手之后光标回到可见', after.caret === true, String(after.caret));
  });

  // 选区从软换行处起（或到某行行首止）时，本行一个字都没选中：上一行末尾不能凭空多出
  //   一个小方块。零宽矩形只该在"本行的段落标记真被选中"时补 —— 那时选的是一枚不占字符
  //   的位置，不补就看不见。软换行处不是段落标记，补了就是在上一行末尾画一块莫名其妙的
  //   反白（Word 那里是空的）。
  test('[23] 从软换行处起选：上一行末尾不多出一个小方块', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      await H.configure({ zoom: 2 });
      await H.setText('甲乙丙丁戊己庚辛壬癸'.repeat(6));
      await H.flush();
      // 换行点：光标 y 第一次跳上去的那一处就是下一行行首，也就是上一行的末尾。
      let wrap = -1;
      let y0 = null;
      for (let i = 0; i <= 55 && wrap < 0; ++i) {
        await H.setCaret({ para: 0, offset: i });
        await H.flush();
        const y = CARET().y;
        if (y0 === null) y0 = y;
        else if (y > y0 + 1) wrap = i;
      }
      if (wrap < 0) return { wrap, at: -1, over: -1 };
      await H.select({ para: 0, offset: wrap }, { para: 0, offset: wrap + 2 });
      await H.flush();
      const at = SEL().rects.length;
      await H.select({ para: 0, offset: wrap - 2 }, { para: 0, offset: wrap + 2 });
      await H.flush();
      const over = SEL().rects.length;
      await H.clearSelection();
      return { wrap, at, over };`);
    check('前提：这一段确实折了行（找到换行点）', r.wrap > 0, '换行点在偏移 ' + r.wrap);
    check('\u2605 从换行处起选：只画下一行那一条，上一行末尾不多小方块',
          r.at === 1, '反白 ' + r.at + ' 条');
    check('反向：真跨过换行处选，两条反白都在', r.over === 2, '反白 ' + r.over + ' 条');
  });

  // 落点身份是五项：段、偏移、侧别、行别、页。前三项在软换行处会重合 —— 同一偏移上压着
  //   上一行行尾与下一行行首两个插入点，只有行别分得开。少判一项就等于"换了落点而宿主
  //   不知道"，输入法与滚动都按上一次的位置算。
  test('[24] 软换行处的两个插入点：前三项相同也算换了落点', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      await H.configure({ zoom: 2 });
      await H.setText('甲乙丙丁戊己庚辛壬癸'.repeat(6));
      await H.flush();
      let wrap = -1;
      let y0 = null;
      for (let i = 0; i <= 55 && wrap < 0; ++i) {
        await H.setCaret({ para: 0, offset: i });
        await H.flush();
        const y = CARET().y;
        if (y0 === null) y0 = y;
        else if (y > y0 + 1) wrap = i;
      }
      if (wrap < 0) return { wrap, same: false, events: -1, dy: 0 };
      // 行别只在给了侧别时才下发（setCaret 的无侧别分支是 plain 落点，不看行别）。
      await H.setCaret({ para: 0, offset: wrap }, { side: 'after', line: 'last' });
      await H.flush();
      const a = CARET();
      const la = D.editStats().caretLineAff;
      let events = 0;
      const off = H.on('caret', () => { ++events; });
      await H.setCaret({ para: 0, offset: wrap }, { side: 'before', line: 'first' });
      await H.flush();
      const b = CARET();
      const lb = D.editStats().caretLineAff;
      off();
      const same = a.para === b.para && a.offset === b.offset && a.page === b.page;
      return { wrap, same, events, la, lb };`);
    check('前提：这一段确实折了行（找到换行点）', r.wrap > 0, '换行点在偏移 ' + r.wrap);
    check('前提：两个插入点的段号、偏移、页完全相同', r.same === true, '同一处偏移 ' + r.wrap);
    check('前提：行别确实分在两个上（其余几项都不动）', r.la !== r.lb,
          `行别 ${r.la} / ${r.lb}`);
    check('\u2605 只有行别不同也算换了落点：caret 事件照发', r.events > 0, '事件 ' + r.events);
  });

  // 相位翻转与位移是两件事：翻转要通知宿主（画不画那颗光标变了），但视口不该跟着回拽。
  //   这两件事共用一个"变没变"会分叉 —— 按相位滚视口，光标每闪一下就把视口拉回原处。
  test('[25] 闪烁相位翻转：发 caret 事件，但不拽视口', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const rows = [];
      for (let i = 0; i < 300; ++i) rows.push('第' + (i + 1) + '段的文字内容，用来把文档撑长。');
      await H.setText(rows.join('\\n'));
      await H.flush();
      // 光标放到末段，视口滚回顶部：此时光标在视口外，任何"按光标回拽"都会把 scrollTop 拉大。
      await H.setCaret({ para: 299, offset: 3 });
      await H.flush();
      const st = document.querySelector('.heditor-stage');
      st.scrollTop = 0;
      await new Promise((r) => requestAnimationFrame(r));
      const before = Math.round(st.scrollTop);
      const wasVisible = H.caret().visible;
      let events = 0;
      const off = H.on('caret', () => { ++events; });
      // 宿主 530 ms 翻一次相位：等一次翻转再留余量。
      await new Promise((r) => setTimeout(r, 800));
      const nowVisible = H.caret().visible;
      off();
      return {
        before, after: Math.round(st.scrollTop), events,
        flipped: wasVisible !== undefined && wasVisible !== nowVisible,
        pages: H.geometry().pages,
      };`);
    check('前提：文档够长（多页）', r.pages >= 4, String(r.pages));
    check('前提：等到了相位翻转', r.flipped === true, '相位未翻转');
    check('\u2605 相位翻转算换了落点：caret 事件照发', r.events > 0, '事件 ' + r.events);
    check('\u2605 相位翻转不拽视口', r.after === r.before, `${r.before} → ${r.after}`);
  });
});

// 公共面的两把尺子：selection().rects 是磅，caret().rect 是设备像素，比值就是密度。
//
// 这条必须在 DPR=2 下跑才有判别力：把磅再按 dpr 除一次之后，DPR=1 下那一步是恒等，
//   全量判据看不出来，只有 2x 屏上选区矩形会缩成一半。
//   探针读数走 diag.selectionRaw()（引擎原样），绕开了公共面的折算，
//   所以这条只能用 H.selection()，不能用 SEL()。
test.describe('DPR=2：选区矩形的单位不随屏幕变', () => {
  test.use({ deviceScaleFactor: 2 });

  test('rects 是磅、caret 是设备像素，两者之比等于密度', async ({ open }) => {
    const { probe, consoleErrors } = await openEdit(open);
    const r = await probe.read<any>(`
      await H.setText('第一段文字足够长，用来量行高');
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      await H.select({ para: 0, offset: 0 }, { para: 0, offset: 3 });
      await H.flush();
      const sel = H.selection();
      const caret = H.caret();
      return { rects: sel.rects, caret: caret.rect, density: H.diag.stats().density,
               dpr: H.geometry().dpr };`);
    check('前提：DPR 真的是 2', r.dpr === 2, String(r.dpr));
    check('前提：有选区矩形', Array.isArray(r.rects) && r.rects.length >= 1,
          JSON.stringify(r.rects));
    // caret.h 是 ceil(行高磅 × 密度) 且保底 1 像素，所以比的是"乘完差不超过一个像素"，
    //   不是精确相等。被 /dpr 污染时 rects.h 只剩一半，这个差会到二十几个像素，判据抓得住。
    const expected = r.rects[0].h * r.density;
    check('★ rects 是磅、caret 是设备像素（caret.h 等于磅乘密度，差不超过一像素）',
          Math.abs(r.caret.h - expected) <= 1,
          `caret.h=${r.caret.h}，rects.h×密度=${expected.toFixed(2)}，` +
              `rects=${JSON.stringify(r.rects[0])}`);
    check('取矩形不产生 uncaught', consoleErrors.length === 0, consoleErrors.join('; '));
  });

  // 覆盖物走同一套换算：磅乘密度得设备像素，再除 DPR 得 CSS 像素。DPR=1 下这两步合成
  //   恒等，漏一步或重一步都看不出来；2x 屏上少除 DPR 就是整体偏一倍 —— 光标边上那颗
  //   输入框与拖格线时的蓝线会跑到别处，而判据全绿。
  test('覆盖物也按磅落位：IME 框与格线引导线在 2x 屏上不漂', async ({ open }) => {
    const { probe, consoleErrors } = await openEdit(open);
    const ime = await probe.read<any>(`
      await H.setText('第一段文字足够长，用来量行高');
      await H.setCaret({ para: 0, offset: 2 });
      await H.flush();
      const c = H.caret();
      const ta = document.querySelector('.heditor-ime');
      const st = document.querySelector('.heditor-stage');
      const pgs = document.querySelector('.heditor-pages');
      const sr = st.getBoundingClientRect(), pr = pgs.getBoundingClientRect();
      return {
        dpr: H.geometry().dpr,
        caret: c.rect,
        left: parseFloat(ta.style.left), top: parseFloat(ta.style.top),
        baseL: pr.left - sr.left, baseT: pr.top - sr.top + st.scrollTop,
        w: parseFloat(ta.style.width),
      };`);
    check('前提：DPR 真的是 2', ime.dpr === 2, String(ime.dpr));
    check('前提：IME 框按光标摆过（不是收起的 1 像素）', ime.w > 2, '宽 ' + ime.w);
    const wantL = ime.baseL + ime.caret.x / ime.dpr;
    const wantT = ime.baseT + ime.caret.y / ime.dpr;
    check('\u2605 IME 框的左边 = 页偏移 + 光标设备像素 / DPR',
          Math.abs(ime.left - wantL) <= 1, `left=${ime.left}，期望 ${wantL.toFixed(1)}`);
    check('\u2605 IME 框的上边 = 页偏移 + 光标设备像素 / DPR',
          Math.abs(ime.top - wantT) <= 1, `top=${ime.top}，期望 ${wantT.toFixed(1)}`);

    // 格线引导线：压在列边界上按下并拖到一半（不松手），读它落位到哪一列。
    const grid = await probe.read<any>(`
      await H.setText('表前');
      await H.flush();
      await H.insertTable({ rows: 2, cols: 2 });
      await H.flush();
      await H.setCaret({ para: 1, offset: 0 });
      await H.flush();
      const q = CARET();
      const g = await H.tableGrid(q.x / ZOOM() + 4, q.y / ZOOM() + 4);
      const fr = FRAME().getBoundingClientRect();
      await D.stopBlink();
      return { g, z: CSSPT(), left: fr.left, top: fr.top };`);
    check('前提：两列三条边界', grid.g.cols.length === 3, JSON.stringify(grid.g.cols));
    const midY = grid.top + (grid.g.rows[0].y + grid.g.rows[0].h / 2) * grid.z;
    // cols 是相对表左缘的，命中格线要加上表自己的 x。
    const guidePt = grid.g.x + grid.g.cols[1];
    const colX = grid.left + guidePt * grid.z;
    const dx = 30;
    await probe.page.mouse.move(colX, midY);
    await probe.page.mouse.down();
    await probe.page.mouse.move(colX + dx, midY, { steps: 4 });
    const guide = await probe.read<any>(`
      const el = document.querySelector('[data-role="grid-guide"]');
      return el ? { left: parseFloat(el.style.left), display: el.style.display } : null;`);
    await probe.page.mouse.up();
    check('前提：拖动时引导线是显示着的', !!guide && guide.display !== 'none',
          JSON.stringify(guide));
    // 引导线跟指针走：起点是格线的磅坐标，再加指针挪过的 CSS 像素。
    const wantG = guidePt * grid.z + dx;
    check('\u2605 格线引导线落在磅坐标上（磅 x 密度 / DPR）',
          !!guide && Math.abs(guide.left - wantG) <= 1,
          guide ? `left=${guide.left}，期望 ${wantG.toFixed(1)}` : '没有引导线');
    check('覆盖物判据不产生 uncaught', consoleErrors.length === 0, consoleErrors.join('; '));
  });
});
