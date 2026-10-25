// 编辑面（三）：相邻与嵌套字段的边界归属。
//
// 真实鼠标点选 + 真实按键：共享边界上点的是哪一个字段、打字进哪一个、
// 退格删到哪里、只读的内层点不点得进。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit, key, sleep, ZOOM } from '../support/edit-helpers';

test.describe('编辑面：控件边界归属', () => {
  test('相邻与嵌套字段的边界归属按契约工作', async ({ open }) => {
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

  log.section('[11] 相邻字段共享边界上打字：字进前一个字段，括号仍是 `][`');
  {
    //  报过的现象（截图：患者主诉：[反悸[▏]三天]，[加重一天]。）：两个相邻控件，
    //  在第一个控件末尾打一个字，共享边界上的括号顺序从 ][ 变成 []。
    //
    //  根因不在排版，在区间伸缩：插入点同时是 A 的末尾与 B 的开头，两条端点规则都命中
    //  —— A 的终点 >= at 右移（字进 A），B 的起点要 > at 才右移（于是原地不动）——
    //  结果一个字同时落进两个控件：A=[5,10)、B=[9,12) 重叠，B 的左括号于是被画到
    //  A 右括号的左边。native 侧的结构性判据见单测 [18]，这里补的是"真实按键路径"。
    await probe.read('return await LOAD.form(0);');
    await probe.read('return await D.stopBlink();');
    const b11 = await probe.read('return D.controls();');
    state.boundary = { before: b11.items.map((it: any) => [...it.start, ...it.end]) };

    check('前置：字段 #0 的末尾与 #1 的开头是同一个位置（共享边界）',
              b11.count >= 2 && b11.items[0].end[0] === b11.items[1].start[0] &&
                b11.items[0].end[1] === b11.items[1].start[1],
              `#0 [${b11.items[0].start[1]},${b11.items[0].end[1]}) ｜` +
                ` #1 [${b11.items[1].start[1]},${b11.items[1].end[1]})`);

    // 光标停在这个边界上，然后派一个真实按键（不是 type()，见 [4] 的说明）
    await probe.read(`return await H.setCaret({ para: 0, offset: ${b11.items[0].end[1]} });`);
    const cc11 = await probe.read('return D.controls();');
    check('光标停在边界上时归前一个字段（末尾算在区间内）', cc11.caret === 0,
              `caret-control=${cc11.caret}`);
    await probe.read(key('诊'));

    const a11 = await probe.read('return D.controls();');
    state.boundary.after = a11.items.map((it: any) => [...it.start, ...it.end]);
    const e0 = a11.items[0].end[1], s1 = a11.items[1].start[1];
    check('★ 字进了 #0（#0 变长），#1 整体右移 —— 两段区间不重叠',
              e0 === b11.items[0].end[1] + 1 && s1 === b11.items[1].start[1] + 1,
              `#0 末尾 ${b11.items[0].end[1]} → ${e0} ｜ ` +
                `#1 开头 ${b11.items[1].start[1]} → ${s1}`);
    check('★ 重叠就是那个 `[]`：一个字同时属于两个字段',
              e0 <= s1, `#0 末尾 ${e0} vs #1 开头 ${s1}`);

    // 几何证据：控件的矩形是从行盒几何投影出来的，重叠的区间投影出重叠的矩形。
    //   #0 的右缘越过 #1 的左缘 = 两个字段的框在屏幕上叠在一起。
    const r0b = a11.items[0].rects[0], r1b = a11.items[1].rects[0];
    const right0 = r0b ? r0b.x + r0b.w : -1;
    const left1 = r1b ? r1b.x : -1;
    state.boundary.rects = { right0, left1 };
    check('★ #0 的内容矩形不越过 #1 的左缘（两个字段的框不叠在一起）',
              right0 > 0 && left1 > 0 && right0 <= left1 + 0.01,
              `#0 右缘 ${right0.toFixed(2)} pt ≤ #1 左缘 ${left1.toFixed(2)} pt`);

    const au11 = await probe.read('return await D.auditLayout();');
    check('★★ 边界上打字之后增量排版 == 全量重排（逐字段）',
              !!au11 && au11.diff === 0,
              au11 ? `diff ${au11.diff}（字形 ${au11.glyphDiff}、行 ${au11.lineDiff}、` +
                `段 ${au11.paraDiff}）` : 'null');
    const sc11 = await probe.read('return await D.selfCheck();');
    check('边界上打字之后屏幕 == 引擎帧', sc11.screenVsEngineDiffPx === 0,
              `差 ${sc11.screenVsEngineDiffPx} px`);

    await probe.read('return await H.setCaret({ para: 0, offset: 10 });');
    await probe.read('await D.present(); return true;');
    await sleep(150);
    await probe.shot('shared-boundary');

    // 三连可编辑字段（演示样例 variant 2）：三条边界两侧都可写 —— 前两个样例的边界
    //   一侧是只读的，只能在一个方向试。这里在边界上打字、再把中间那个删空，
    //   看区间和矩形会不会互相穿过。
    await probe.read('return await LOAD.form(2);');
    await probe.read('return await D.stopBlink();');
    const v3 = await probe.read('return D.controls();');
    state.chain3 = { before: v3.items.map((it: any) => [...it.start, ...it.end]) };
    check('三连样例：3 个字段都紧挨着（两条共享边界）',
              v3.count === 3 && v3.items.every((it: any) => !it.ro) &&
                v3.items[0].end[1] === v3.items[1].start[1] &&
                v3.items[1].end[1] === v3.items[2].start[1],
              v3.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));

    await probe.read('return await H.setCaret({ para: 0, offset: 7 });');  // 第一条共享边界
    await probe.read(key('痛'));
    const v3b = await probe.read('return D.controls();');
    state.chain3.afterType = v3b.items.map((it: any) => [...it.start, ...it.end]);
    // 字进前一个字段、后两个整体右移；三段首尾相接就是"不穿过"的最强形式
    const chain = (r: any) =>
      r.items.every((it: any, i: any) => i + 1 >= r.items.length || it.end[1] === r.items[i + 1].start[1]);
    check('★ 三连样例：在第一条边界上打字之后，三段区间仍然首尾相接（不重叠、不穿线）',
              chain(v3b) && v3b.items[0].end[1] === 8 && v3b.items[1].start[1] === 8 &&
                v3b.items[1].end[1] === 10 && v3b.items[2].start[1] === 10,
              v3b.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));

    // 把中间那个删空：它变成零宽的空字段，边界上与"左字段的闭 + 右字段的开"并列。
    //   这里重新载入一次样例，拿回干净的三连区间（[5,7) [7,9) [9,11)）—— 上面那一步
    //   已经往左字段里打过字，它变长成了 [5,8)，量出来的就不是这条边界了。
    await probe.read('return await LOAD.form(2);');
    await probe.read('return await D.stopBlink();');
    await probe.read('return await H.setCaret({ para: 0, offset: 9 });');
    await probe.read(key('Backspace'));
    await probe.read(key('Backspace'));
    const v3c = await probe.read('return D.controls();');
    state.chain3.afterEmpty = v3c.items.map((it: any) => [...it.start, ...it.end]);
    const mid = v3c.items[1];
    const right = v3c.items[2];
    check('★ 中间字段被删成零宽的空字段，右边那个左移并保持紧挨着',
              mid.start[1] === mid.end[1] && right.start[1] === mid.end[1],
              v3c.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));

    // 删空之后那个零宽字段必须还能进、还能打字：
    //   它是自己位置上的最内层（区间树要跟着编辑重算），否则位置被左邻居抢走，
    //   表现就是"中间的控件删掉最后一个字之后光标再也进不去、字也打不进去"。
    //   注意顺序：这一段必须在"往左字段里打字"之前做，否则左字段已经变长（[5,8)），
    //   量出来的边界就不是 [5,7] 那一处了。
    const caretAt = await probe.read('return CARET().offset;');
    check('★ 光标停在那个零宽字段的位置上（位置归属没被左邻居抢走）',
              caretAt === mid.start[1], `光标 ${caretAt}，空字段 ${mid.start[1]}`);
    // 方向键一次一格 —— 括号也是格（占位簇各占一枚），所以共享边界 ][ 要按两下：
    //   一格一格地走过"字与开括号之间""两枚括号之间"那些位置，不许整段跳过去。
    await probe.read('return await H.setCaret({ para: 0, offset: 5 });');
    const walk = [];
    for (let i = 0; i < 5; ++i) {
      await probe.read(key('ArrowRight'));
      const a = await probe.read(`
        const q = CARET();
        return { o: q.offset, s: q.side };`);
      walk.push(`${a.o}/${a.s}`);
    }
    state.chain3.walk = walk;
    check('★ 方向键一次一格，走到字段内容末尾就停住（不跨过 ][）',
              JSON.stringify(walk) === JSON.stringify(['6/0', '7/0', '7/0', '7/0', '7/0']),
              `落点 ${walk.join(' ')}`);
    // 回到那个零宽字段的位置上：打字应该进它自己，左右两个字段不许被撑大
    await probe.read('return await H.setCaret({ para: 0, offset: 7 });');
    await probe.read(key('痛'));
    const v3d = await probe.read('return D.controls();');
    state.chain3.afterFill = v3d.items.map((it: any) => [...it.start, ...it.end]);
    check('★ 在空字段的位置上打字 → 字进它自己（左右两个字段都没被撑大）',
              v3d.items[1].start[1] === 7 && v3d.items[1].end[1] === 8 &&
                v3d.items[0].end[1] === 7 && v3d.items[2].start[1] === 8,
              v3d.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));

    const au3 = await probe.read('return await D.auditLayout();');
    check('★★ 三连样例的每一步增量排版都等于全量重排', !!au3 && au3.diff === 0,
              au3 ? `diff ${au3.diff}` : 'null');
    const sc3 = await probe.read('return await D.selfCheck();');
    check('三连样例：屏幕 == 引擎帧', sc3.screenVsEngineDiffPx === 0,
              `差 ${sc3.screenVsEngineDiffPx} px`);
    await probe.read('return await H.setCaret({ para: 0, offset: 7 });');
    await probe.read('await D.present(); return true;');
    await sleep(150);
    await probe.shot('three-adjacent-fields');
  }

  log.section('[12] 真实鼠标点击的归属：点后一个字段的开头要进后一个字段');
  {
    // 为什么必须用真实鼠标事件（CDP Input.dispatchMouseEvent）：
    //   这一格坏在"点画布 → 命中测试 → 归属"这条链上，而合成 MouseEvent 与真实点击
    //   走的虽然都是同一个监听器，但只有真实事件连坐标换算、命中、dpr 一起过一遍。
    //   之前所有探针都用合成事件，所以这格一直没被钉住。
    await probe.read('return await LOAD.form(2);');
    await probe.read('return await D.stopBlink();');
    const geo = await probe.read(`
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const c0 = D.controls().items[0].rects[0];
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), y: c0.y + c0.h / 2 };`);
    const click = async (xPt: any, yPt: any) => {
      const x = geo.left + xPt * geo.zoom * geo.sx;
      const y = geo.top + yPt * geo.zoom * geo.sy;
      const p = { x, y, button: 'left', clickCount: 1, buttons: 1 };
      await probe.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await probe.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      await sleep(30);
    };

    // 控件自己投影出来的内容框（磅）：点框内任意一点都必须归它自己。
    //   这就是用户眼里的"我点的是这个框"。
    const boxes = await probe.read(`
      const cs = D.controls();
      return cs.items.map((it, i) => (it.rects || []).map(q => ({ i, x: q.x, w: q.w, y: q.y })))
        .flat();`);
    let probed = 0;
    const wrong = [];
    for (const b of boxes) {
      for (let x = b.x + 1; x <= b.x + b.w - 1; x += 2) {
        await click(x, geo.y);
        const st = await probe.read('return D.controls().caret;');
        ++probed;
        if (st !== b.i) wrong.push(`x=${Math.round(x)}→字段${st}(应${b.i})`);
      }
    }
    state.clickOwner = { probed, wrong: wrong.slice(0, 5) };
    check('★★ 真实点击：点任意字段内容框内的点，归属都是它自己',
              probed > 20 && wrong.length === 0,
              `采样 ${probed} 点，错的 ${wrong.length} 个 ${wrong.slice(0, 3).join(' ')}`);

    // 落笔也跟着归属走：点中间字段的开头再打字，字必须进中间那个字段。
    await probe.read('return await LOAD.form(2);');
    await probe.read('return await D.stopBlink();');
    const midBox = await probe.read(`
      const q = D.controls().items[1].rects[0];
      return { x: q.x, y: q.y, w: q.w, h: q.h };`);
    await click(midBox.x + 1, geo.y);
    const ownerAtStart = await probe.read('return D.controls().caret;');
    const caretAtStart = await probe.read('return CARET().offset;');
    await probe.read(key('痛'));
    const v12 = await probe.read('return D.controls();');
    state.clickOwner.afterType = v12.items.map((it: any) => [...it.start, ...it.end]);
    check('★ 真实点击中间字段的开头 → 光标停在那里且归属它',
              ownerAtStart === 1 && caretAtStart === 7,
              `归属 ${ownerAtStart}，光标 ${caretAtStart}`);
    check('★ 接着打字 → 字进中间那个字段（左字段没被撑大）',
              v12.items[0].end[1] === 7 && v12.items[1].end[1] === 10,
              v12.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));

    const au12 = await probe.read('return await D.auditLayout();');
    check('★★ 点击 + 打字之后增量排版 == 全量重排', !!au12 && au12.diff === 0,
              au12 ? `diff ${au12.diff}` : 'null');
    const sc12 = await probe.read('return await D.selfCheck();');
    check('点击之后屏幕 == 引擎帧', sc12.screenVsEngineDiffPx === 0,
              `差 ${sc12.screenVsEngineDiffPx} px`);
  }

  log.section('[13] 三层嵌套可编辑字段（演示样例 variant 3）');
  {
    await probe.read('return await LOAD.form(3);');
    await probe.read('return await D.stopBlink();');
    const n = await probe.read('return D.controls();');
    state.nested = { before: n.items.map((it: any) => [...it.start, ...it.end]) };
    check('三层嵌套：深度 0/1/2，父链 外→中→内',
              n.count === 3 && n.items.every((it: any) => !it.ro) && n.items[0].depth === 0 &&
                n.items[1].depth === 1 && n.items[2].depth === 2 && n.items[1].parent === 0 &&
                n.items[2].parent === 1,
              n.items.map((it: any) => `#${it.depth}p${it.parent}[${it.start[1]},${it.end[1]})`).join(' '));

    // 归属取最内层：在内层中间打字，三层一起变长（父子区间不许互相穿过）。
    await probe.read('return await H.setCaret({ para: 0, offset: 8 });');
    const ownerN = await probe.read('return D.controls().caret;');
    await probe.read(key('痛'));
    const n2 = await probe.read('return D.controls();');
    state.nested.afterType = n2.items.map((it: any) => [...it.start, ...it.end]);
    check('★ 光标在内层里打的字归最内层（不是外层）', ownerN === 2, `归属 ${ownerN}`);
    check('★ 内层变长 → 中/外层一起变长（区间不许互相穿过）',
              n2.items[2].end[1] === 10 && n2.items[1].end[1] === 11 && n2.items[0].end[1] === 12 &&
                n2.items[2].start[1] === 7 && n2.items[1].start[1] === 6 && n2.items[0].start[1] === 5,
              n2.items.map((it: any) => `[${it.start[1]},${it.end[1]})`).join(' '));
    // 三层都可编辑 → 内层也必须能改（嵌套最容易被误伤的一格：内层被外层"吃掉"）
    check('★ 内层可编辑（嵌套没有把内层的编辑权吃掉）',
              n2.items[2].end[1] === n2.items[2].start[1] + 3,
              `内层 = [${n2.items[2].start[1]},${n2.items[2].end[1]})，打进了 1 个字`);

    const au13 = await probe.read('return await D.auditLayout();');
    check('★★ 嵌套样例增量排版 == 全量重排（括号占位嵌套时最容易错位）',
              !!au13 && au13.diff === 0, au13 ? `diff ${au13.diff}` : 'null');
    const sc13 = await probe.read('return await D.selfCheck();');
    check('嵌套样例：屏幕 == 引擎帧', sc13.screenVsEngineDiffPx === 0,
              `差 ${sc13.screenVsEngineDiffPx} px`);

    await probe.read('return await LOAD.form(3);');
    await probe.read('return await H.setCaret({ para: 0, offset: 8 });');
    await probe.read('await D.present(); return true;');
    await sleep(150);
    await probe.shot('nested-three-levels');
  }

  log.section('[15] 文档模式也画控件边界（方括号、与表单一致），只读才不画不占位');
  {
    //  用户的原话："编辑模式下不显示控件边界了，我都不知道哪块是控件"。
    //  口径（引擎的 bracketedControls / bracketPlanId）：文档模式与表单模式都占位、都画；
    //  只读模式是呈现与打印态，一个都不占 —— 占位会把正文凭空推开一截，而它又不画。
    //  判据落在占位槽清单上：占位与不占位的区别是"正文有没有被推开"，画的都是同一对括号，
    //  截图分不出来。槽清单里每枚括号的偏移、开闭、归属、所在行都在，这是唯一看得见它的地方。
    const load15 = async (v: any) => {
      await probe.read(`return await LOAD.form(${v});`);
      await probe.read('return await D.stopBlink();');
    };
    const slots15 = () => probe.read('return (await D.bracketSlots() || []).length;');
    const text15 = () => probe.read('return Array.from(await H.text()).join("");');

    await load15(0);
    await probe.read('return await SET_BRACKETS(true);');  // 显式打开括号
    const cnt15 = await probe.read('return D.controls().count;');
    const form15 = await slots15();
    // 每个字段自己的那一对：一枚开、一枚闭，归属不许串台（串台就是几何投影错位）
    const owned15 = await probe.read(`
      const m = {};
      for (const s of (await D.bracketSlots() || [])) {
        m[s.owner] = m[s.owner] || { open: 0, close: 0 };
        if (s.side === 1) m[s.owner].open++; else m[s.owner].close++;
      }
      return m;`);
    const ownedOk = Object.keys(owned15).length === cnt15 &&
      Object.values(owned15).every((v: any) => v.open === 1 && v.close === 1);
    state.docBrackets = { count: cnt15, form: form15, owned: owned15 };

    check('前置：表单模式下每个字段两枚括号',
              form15 === cnt15 * 2, `${form15} 槽 / ${cnt15} 字段`);
    check('表单模式：每个字段各一枚开括号 + 一枚闭括号（归属不串台）', ownedOk,
              JSON.stringify(owned15));

    const tForm15 = await text15();
    await probe.read('return await SET_MODE(0);');  // 文档模式
    const doc15 = await slots15();
    state.docBrackets.doc = doc15;
    check('★ 文档模式：括号照常占位（不再"编辑模式下什么都看不见"）',
              doc15 === cnt15 * 2, `${doc15} 槽 / ${cnt15} 字段`);
    check('★ 文档模式：正文一个字都没变（括号是占位簇，不是插进去的字符）',
              (await text15()) === tForm15, JSON.stringify(await text15()));

    // All 档下括号常驻：光标跨字段移动不改变占位集合，也不该重排（这是 All 档的意义）
    await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
    check('文档模式：光标跨字段移动不改变占位集合（All 档括号常驻）',
              (await slots15()) === cnt15 * 2, `${await slots15()} 槽`);

    await probe.read('return await SET_MODE(2);');  // 只读
    const ro15 = await slots15();
    check('★ 只读模式：一个括号都不占、也不画（它是要印出来的呈现态）',
              ro15 === 0, `${ro15} 槽`);
    check('只读模式：正文也没变（"不占位"不等于"删了字"）',
              (await text15()) === tForm15, JSON.stringify(await text15()));

    await probe.read('return await SET_MODE(0);');
    const sc15 = await probe.read('return await D.selfCheck();');
    check('★ 三档来回切之后屏幕 == 引擎帧（标记改了、底图没改，靠整帧重贴收口）',
              sc15.screenVsEngineDiffPx === 0, `差 ${sc15.screenVsEngineDiffPx} px`);
    await probe.read('await D.present(); return true;');
    await sleep(150);
    await probe.shot('brackets-in-document-mode');
  }

  log.section('[16] 控件边界上的退格（真实按键）+ 引擎命令');
  {
    //  四条规则（见 engine.cpp 的 bracketBackspace / bracketDeleteForward）。
    //  三连可编辑字段（variant 2）：part1=[5,7) 反复、part2=[7,9) 心悸、part3=[9,11) 三天。
    //  字段末尾那个码点偏移上压着两格 —— 侧别 0 还在内容里（退的是字），侧别 1 已经跨过
    //  闭括号（退的是控件内容里那一项）。任何"按码点偏移猜"的实现都过不了下面 (a)/(b) 这一对。
    //  结构操作要文档模式：表单模式默认被 structureLock 锁住，(c) 那一条正是它的对照。
    const load16 = async (v: any) => {
      await probe.read(`return await LOAD.form(${v});`);
      await probe.read('return await D.stopBlink();');
    };
    const text16 = () => probe.read('return Array.from(await H.text()).join("");');
    const ids16 = () => probe.read('return D.controls().items.map(i => i.id);');
    const n16 = () => probe.read('return D.controls().count;');
    const blocked16 = () => probe.read('return D.blockedEdits();');
    const side16 = () => probe.read('return CARET().side;');

    await load16(2);
    await probe.read('return await SET_MODE(0);');
    const base16 = { t: await text16(), ids: await ids16() };
    state.removeControl = { base: base16 };
    check('前置：三连可编辑字段 part1 / part2 / part3',
              JSON.stringify(base16.ids) === '["part1","part2","part3"]',
              JSON.stringify(base16.ids));

    // (a) 对照组：侧别 0 —— 还在 part2 的内容里，退格删的是「悸」这个字
    await probe.read('return await H.setCaret({ para: 0, offset: 9 }, { side: (0) > 0 ? "after" : "before" });');
    check('前置：侧别 0 是 part2 的内容末尾', (await side16()) === 0, `侧别 ${await side16()}`);
    await probe.read(key('Backspace'));
    check('★ 侧别 0（内容末尾）退格删的是「悸」，控件一个都没少',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复心三天，加重一天。',
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);

    // (b) 侧别 1 —— 已经跨过 part2 的闭括号：按规则先进 part2 删掉它内容里的最后一项，
    //   控件本身要等它空了才删得掉（"删一个字符或空控件"）。
    await load16(2);
    await probe.read('return await SET_MODE(0);');
    await probe.read('return await H.setCaret({ para: 0, offset: 9 }, { side: (1) > 0 ? "after" : "before" });');
    check('前置：侧别 1 是 part2 的闭括号之后', (await side16()) === 1, `侧别 ${await side16()}`);
    await probe.read(key('Backspace'));
    state.removeControl.afterBackspace = { ids: await ids16(), t: await text16() };
    check('★★ 闭括号之后退格：进 part2 删掉「悸」，控件一个都没少',
              JSON.stringify(await ids16()) === '["part1","part2","part3"]' &&
                (await text16()) === '患者主诉：反复心三天，加重一天。',
              `${JSON.stringify(await ids16())} ｜ ${JSON.stringify(await text16())}`);
    check('★ 光标进到了 part2 里（内容末尾，侧别 0）', (await side16()) === 0,
              `侧别 ${await side16()}`);
    const lv16 = await probe.read('return await D.auditLayout();');
    check('★★ 删字之后增量排版 == 全量重排', !!lv16 && lv16.diff === 0,
              lv16 ? `diff ${lv16.diff}` : 'null');
    const sc16 = await probe.read('return await D.selfCheck();');
    check('★ 删字之后屏幕 == 引擎帧（括号与占位都得跟着走）',
              sc16.screenVsEngineDiffPx === 0, `差 ${sc16.screenVsEngineDiffPx} px`);

    // part2 的内容删光（两下）之后光标停在它内部 ⇒ 第三下删掉的是这个空控件本身
    await probe.read(key('Backspace'));
    await probe.read(key('Backspace'));
    state.removeControl.afterBackspace2 = { ids: await ids16(), t: await text16() };
    check('★★ 空控件上再退一格：删掉的是 part2 这个控件（不是字）',
              JSON.stringify(await ids16()) === '["part1","part3"]', JSON.stringify(await ids16()));
    // 区间不许被撑大：part2 整个消失之后 part1 / part3 各守自己的位置。
    //   单列这一条是因为"删掉一个字段、前一个字段把它吞了"也能让上面那条全绿，
    //   视觉上就是区间被撑大，不量区间看不出来。
    const rng16 = await probe.read('return D.controls().items.map(i => [i.start[1], i.end[1]]);');
    state.removeControl.ranges = rng16;
    check('★★ 剩下两个字段的区间仍是 [5,7) 与 [7,9)（前一个字段没把它吞进去）',
              JSON.stringify(rng16) === '[[5,7],[7,9]]', JSON.stringify(rng16));
    check('★★ 文本只剩 part1 与 part3 的内容（控件里本来就空了）',
              (await text16()) === '患者主诉：反复三天，加重一天。', JSON.stringify(await text16()));
    await probe.shot('ctl-remove-by-backspace');

    // 撤销要把控件整棵装回去，而且光标回到按键之前那一格 —— 不是"选中整个控件"
    //   （那样撤销之后光标会落到闭括号右边，屏幕上还留着一圈选区）。
    check('★★ 撤销把 part2 装回来（空的、还在原来那一格），光标归位且不留选区',
              (await probe.read('return await H.undo() ? 1 : 0;')) === 1 &&
                JSON.stringify(await ids16()) === '["part1","part2","part3"]' &&
                (await text16()) === '患者主诉：反复三天，加重一天。' &&
                (await probe.read('return SEL().empty ? 1 : 0;')) === 1 &&
                JSON.stringify(await probe.read(
                  'return D.controls().items.map(i => [i.start[1], i.end[1]]);')) ===
                  '[[5,7],[7,7],[7,9]]',
              JSON.stringify(await ids16()) + ' ｜ 侧别 ' + (await side16()));

    // (c) 表单模式：结构被 structureLock 锁住 —— 删字那一下照样生效，删控件那一下被拒
    //   （光标落在 part2 的内容末尾：表单模式的光标只停在可编辑内容里，括号之间那一格
    //   不可插入，见 docs/文档模型设计.md 第七节。）
    await load16(2);
    const b16 = await blocked16();
    await probe.read('return await H.setCaret({ para: 0, offset: 9 }, { side: (1) > 0 ? "before" : "after" });');
    await probe.read(key('Backspace'));
    check('★ 表单模式：内容末尾退格删掉 part2 的「悸」（删的是内容，不是结构）',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复心三天，加重一天。' &&
                (await blocked16()) === b16,
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);
    await probe.read(key('Backspace'));
    await probe.read(key('Backspace'));
    check('★ 空控件上那一下被拒：控件还在、光标没被带出括号（删除不越过控件边界）',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复三天，加重一天。',
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);
    check('★ 那次被拒的结构操作记了账', (await blocked16()) === b16 + 1,
              `blockedEdits ${b16} → ${await blocked16()}`);

    // (d) 引擎命令：连内容一起删（keep_content = false）
    await load16(2);
    await probe.read('return await SET_MODE(0);');
    const rm16 = await probe.read('return await D.removeControl(1, false) ? 1 : 0;');
    check('★ 引擎命令 removeControl(1,false)：连内容一起删',
              rm16 === 1 && JSON.stringify(await ids16()) === '["part1","part3"]' &&
                (await text16()) === '患者主诉：反复三天，加重一天。',
              `${JSON.stringify(await ids16())} ｜ ${JSON.stringify(await text16())}`);
    check('★ 撤销把它整棵装回来',
              (await probe.read('return await H.undo() ? 1 : 0;')) === 1 &&
                JSON.stringify(await ids16()) === JSON.stringify(base16.ids) &&
                (await text16()) === base16.t,
              JSON.stringify(await ids16()));

    // (e) 锁是逐控件的：表单模式里给 part2 开锁之后它删得掉，没开锁的 part1 仍被拒
    await load16(2);
    const unlock16 = await probe.read('return await H.setControlProp(1, 3, false) ? 1 : 0;');
    const rmLocked16 = await probe.read('return await D.removeControl(0, true) ? 1 : 0;');
    const rmOpen16 = await probe.read('return await D.removeControl(1, true) ? 1 : 0;');
    check('★ 表单模式：开锁的那个删得掉、没开锁的仍被拒（锁挂在控件自己身上）',
              unlock16 === 1 && rmLocked16 === 0 && rmOpen16 === 1 &&
                JSON.stringify(await ids16()) === '["part1","part3"]',
              `开锁 ${unlock16} 删被锁的 ${rmLocked16} 删已开锁的 ${rmOpen16} ｜ ` +
                JSON.stringify(await ids16()));

    // (f) 属性入口幂等：拿同一个值再设一次，撤销栈上不留记录
    await load16(2);
    await probe.read('return await SET_MODE(0);');
    await probe.read('return await H.setControlProp(0, 3, false);');  // 锁 → 开：记一条
    const before16 = await text16();
    await probe.read('return await H.setControlProp(0, 3, false);');  // 再设一次：不记
    check('★ 属性入口幂等：同一个值设两次只留一条撤销记录',
              (await probe.read('return await H.undo() ? 1 : 0;')) === 1 &&
                (await probe.read('return await H.undo() ? 1 : 0;')) === 0 &&
                (await text16()) === before16,
              JSON.stringify(await text16()));

    // (g) 默认键位：Ctrl+Z 撤销、Ctrl+Y（与 Ctrl+Shift+Z）重做。
    //   上面几条走的是宿主门面 undo() —— 那是同一件事的另一条路。键位这条路要真的
    //   派按键才算验过，而且它多担一条："该吞的吞"。页面没有可撤销的 DOM 编辑
    //   （键盘的承接层是个只读 textarea），漏给浏览器只会让 Ctrl+Z 表现成"按了没反应"。
    //   这也是 [2] 段那条"Ctrl/Alt/Meta 一律不吞"的例外，例外必须写进判据里。
    await load16(2);
    await probe.read('return await SET_MODE(0);');
    const L16 = (await text16()).length;
    await probe.read(`return await H.setCaret({ para: 0, offset: ${L16} });`);
    await probe.read(key('X'));
    await probe.read(key('Y'));
    const typed16 = await text16();
    check('前置：真实按键敲进两个字（各是一笔事务）',
              typed16 === base16.t + 'XY', JSON.stringify(typed16));

    const ctrl = (k: any, extra = '') => probe.read(`
      const e = new KeyboardEvent('keydown',
        { key: ${JSON.stringify(k)}, ${extra}bubbles: true, cancelable: true });
      return IME().dispatchEvent(e);   // false = 被 preventDefault
    `);
    // 摆前置状态一律用门面 undo()，不借上一条键位判据的结果 —— 判据要各自承重：
    //   若靠上一条把状态摆好，键位整段没绑时"重做回原样"照样是通过的。
    const undoFacade = () => probe.read('return await H.undo() ? 1 : 0;');

    // 撤销粒度 = 一笔编辑一步，不做合并：敲进去两个字符就要撤两次。
    check('★ Ctrl+Z 被宿主吞掉（不吞 = 这个键位等于没绑）',
              (await ctrl('z', 'ctrlKey: true, ')) === false);
    check('★ Ctrl+Z 一次只退掉最后那个字符（撤销不做合并）',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    await ctrl('z', 'ctrlKey: true, ');
    check('★ 再撤一次，回到载入时的文本（栈空）',
              (await text16()) === base16.t, JSON.stringify(await text16()));

    // 重做同样一步一个字。此时状态是 base、重做栈里有两项，所以这一对判据各自承重。
    await ctrl('y', 'ctrlKey: true, ');
    check('★ Ctrl+Y 重做一步：退掉的字回来一个',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    await ctrl('y', 'ctrlKey: true, ');
    check('★ 再重做一次：两个字都回来了', (await text16()) === typed16,
              JSON.stringify(await text16()));

    // Ctrl+Shift+Z 也是重做。真键盘在 shift 按下时给的是大写 'Z' ——
    //   判据必须走这一格，否则"只在 key === 'z' 上判"的实现会漏掉它。
    check('前置：门面撤销一次（文本退到 base + 一个字）',
              (await undoFacade()) === 1 && (await text16()) === base16.t + 'X',
              JSON.stringify(await text16()));
    await ctrl('Z', 'ctrlKey: true, shiftKey: true, ');
    check('★ Ctrl+Shift+Z 也当重做（大写 Z 同样要认）',
              (await text16()) === typed16, JSON.stringify(await text16()));

    // 撤到底：栈空之后再一次 Ctrl+Z 不许改文档，也不许被当成"没处理"漏给浏览器
    //   —— 那条路落到 resetBlink()，吞掉动作在调引擎之前就做完了。
    check('前置：门面撤销两次，栈空、文本回到载入时',
              (await undoFacade()) === 1 && (await undoFacade()) === 1 &&
                (await text16()) === base16.t,
              JSON.stringify(await text16()));
    check('★ 栈空时 Ctrl+Z 仍然被吞、且文档一个字不改',
              (await ctrl('z', 'ctrlKey: true, ')) === false && (await text16()) === base16.t,
              JSON.stringify(await text16()));

    // (h) 撤销上限：宿主的门面把它一路传到引擎（默认 100，与 Office 系和 Quill 的
    //   maxStack 同值）。这条走的是 setUndoLimit → wasm → EngineConfig → UndoStack 一整条线，
    //   任何一环断了这里都会露出来。
    check('默认撤销上限 100（Office 系与 Quill 的 maxStack 同值）',
              (await probe.read('return await H.config().undoLimit;')) === 100,
              String(await probe.read('return await H.config().undoLimit;')));

    await load16(2);
    await probe.read('return await SET_MODE(0);');
    await probe.read('return await UNDO_LIMIT(2);');
    await probe.read(`return await H.setCaret({ para: 0, offset: ${L16} });`);
    for (const ch of ['X', 'Y', 'Z']) await probe.read(key(ch));
    check('前置：敲进三个字符', (await text16()) === base16.t + 'XYZ',
              JSON.stringify(await text16()));

    let undoneH = 0;
    while (await probe.read('return await H.undo() ? 1 : 0;')) ++undoneH;
    check('★ 上限 2：三笔只撤得动两次', undoneH === 2, String(undoneH));
    check('★ 丢的是最旧的 —— 第一个字符撤不回去了',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    // 还原成默认，免得影响后面的判据。
    check('还原成默认上限', (await probe.read('return await UNDO_LIMIT(100);')) === 100);
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
