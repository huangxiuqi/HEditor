// 编辑面（二）：表单模式下控件是区间、边界是策略。
//
// 控件不是"画上去的括号"，而是一段带侧别的区间：插入点落在区间里还是区间外、
// 只读能不能进、相邻两个字段共享一处边界时归谁，全在这一份里钉。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit, key, ZOOM } from '../support/edit-helpers';

test.describe('编辑面：表单控件', () => {
  test('控件是区间、边界是策略，表单模式下按契约工作', async ({ open }) => {
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

  log.section('[8] 表单：控件是「区间」、边界是「策略」');
  {
    // 这一节要证明四件事，缺一不可：
    //   标记真的被画出来了（不是"数据里有控件、画面上什么都没有"）；
    //   括号常驻：每个字段的左括号一直在，且不随光标变（默认档 All）；
    //   字段被删空之后括号仍然在（内容没了、"位置还占着"、括号却消失了）；
    //   只读模式标记真的消失（文档与表单都画，只读是呈现与打印态，不画也不占位）。
    //
    // 标记色 = 亮蓝 rgb(56,139,253)，与 EngineConfig::control_mark 一致
    // —— 刻意和光标色 (31,111,235) 不同，否则像素断言里分不清"括号"和"光标"。
    //
    // 判据 = 方向性（"偏蓝得够远、够亮、红通道不太低"），不是围绕标记色的矩形容差：
    //   括号是字形、笔画细，大部分像素是"标记色 ↔ 白"的混色，矩形容差（|r-56|<=20）
    //   会把它们判成"不是标记"；而同一份画面里矩形容差只数到 1 px、方向性判据数到
    //   5~20 px。相位或粗细一变就翻脸的断言等于没有断言。
    //   光标色 (31,111,235) 的 r=31 < 45，被方向性判据排除 —— 这是刻意留的口子，
    //   否则"光标旁边的括号"和"光标本身"分不开。
    //   只读字段的括号是灰的（168,168,168 ⇒ b-r=0），两种判据都不认它。
    const MARK = `(r, g, b) => r > 45 && b - r > 60 && b > 140`;
    const inkMark = `
      const mark = ${MARK};   // 光标色 r=31 被排除
      const f = await D.rawFrame();
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        if (mark(f[i], f[i+1], f[i+2])) ++n;
      }
      return n;
    `;
    // 统计某条竖带里的标记像素 —— 括号就画在控件左缘外侧。
    //
    // 竖带取 [cx-10, cx-1] 磅（全部在左缘左边），这个范围是量出来的：
    //   括号 pen_x = 左缘 - adv - gap，而 adv 随字号走（14pt 下约 4.2pt，
    //   加上 1.5pt 的 gap ⇒ 左缘左侧约 6pt 起步，笔画本身还再往里几像素）。
    //   窗口取 [cx-4, cx+1] 时恰好差 1 像素没框住括号，断言会红而画面其实是对的
    //   （诊断显示左括号在 x=183、控件左缘在 x=190）。
    //   结论：像素断言的窗口要按"实际量出来的位置"定，不能按几何直觉估。
    //   上限取 cx-1（而不是 cx+2）是为了不碰前一个字段的右括号 ——
    //   那会让 Focused 档下"非聚焦字段左缘应为空"这条假阳性。
    // 必须带 return：探针的 eval 直接执行这段表达式，没有 return 的话
    //   结果就是 undefined（断言会以 "undefined px" 的形式失败，很好认）。
    const bandAt = (cxPt: any) => `
      return (async function () {
        const near = ${MARK};
        const f = await D.rawFrame();
        const fw = D.frameW();
        const lo = ${Math.round((cxPt - 10) * 1.5)}, hi = ${Math.round((cxPt - 1) * 1.5)};
        let n = 0;
        for (let i = 0; i + 3 < f.length; i += 4) {
          const x = (i >> 2) % fw;
          if (x < lo || x > hi) continue;
          if (near(f[i], f[i+1], f[i+2])) ++n;
        }
        return n;
      })()
    `;

    // 一个括号槽的像素宽 = 括号字形 advance（12pt 下实测 4.055pt）
    //   + 排版留的间隙 kControlBracketSlotGapPt。间隙也算宽度，所以槽才是
    //   "一个字被推开多少"的单位 —— 用括号 advance 会系统性偏小 1.5pt。
    // 提到这一节的最外层：下面好几个子块都要用它（含"空字段"那块）。
    const bracketSlotPx = (4.055 + 1.5) * ZOOM;

    await probe.read('return await LOAD.form(0);');
    const ct = await probe.read('return D.controls();');
    check('载入表单样例：3 个控件、已切到表单模式',
              ct.count === 3 && ct.mode === 1, `count=${ct.count} mode=${ct.mode}`);

    // 像素级：括号必须把画出来的字推开，而不只是把几何数字改大。
    //
    //   这条断言的存在理由（一个真实且不易察觉的缺陷）：
    //     行盒的簇位置来自 cluster_adv_pt（含占位宽度），而字形位置只用
    //     pen_x += sg.x_advance 一个个字形累加 —— 占位簇没有字形（g0 == g1），
    //     那个循环一次都不执行。于是光标/控件矩形按"字让开了"算，画出来的字
    //     却没让开，表象就是方括号压在文字上。
    //     而单测里那条"首行宽变大"只查了 inline_end_pt（= cluster_adv_pt 之和），
    //     与"字形画在哪"是两套数 —— 它结构上永远抓不到这个 bug。
    //
    //   做法：同一段文字（scenario 6）两遍，一遍不带表单、一遍带表单且光标在字段里，
    //   只比最右那个墨迹列簇的左缘。它应当整整移开两个括号宽（[ 与 ]）。
    {
      const lastInkLeft = `
        return (async function () {
          const f = await D.rawFrame();
          const W = D.frameW(), H = D.frameH();
          const at = (x, y) => { const i = (y * W + x) * 4; return [f[i], f[i+1], f[i+2]]; };
          const dark = (r, g, b) => r < 120 && g < 120 && b < 120;
          const rows = [];
          for (let y = 0; y < H; ++y) {
            let n = 0;
            for (let x = 0; x < W; ++x) { const [r,g,b] = at(x,y); if (dark(r,g,b)) ++n; }
            rows.push(n);
          }
          let s = -1; const bands = [];
          for (let y = 0; y < H; ++y) {
            if (rows[y] > 0) { if (s < 0) s = y; }
            else { if (s >= 0) { bands.push([s, y - 1]); s = -1; } }
          }
          if (s >= 0) bands.push([s, H - 1]);
          const band = bands.find(g => g[1] - g[0] > 4) || bands[0] || [0, 0];
          const xs = [];
          for (let x = 0; x < W; ++x) {
            let hit = false;
            for (let y = band[0]; y <= band[1]; ++y) {
              const [r,g,b] = at(x,y); if (dark(r,g,b)) { hit = true; break; }
            }
            if (hit) xs.push(x);
          }
          if (!xs.length) return { left: -1, right: -1 };
          let a = xs[xs.length - 1], p = xs[xs.length - 1];
          for (let i = xs.length - 2; i >= 0; --i) {
            if (p - xs[i] > 3) break;
            a = xs[i]; p = xs[i];
          }
          return { left: a, right: xs[xs.length - 1] };
        })()
      `;
      // 期望位移现在是 6 个槽（不是 2 个）：默认档 All ——
      //   表单样例的 3 个字段（complaint / duration(只读) / aggravate）
      //   每个都常驻一对括号 ⇒ 行尾那个字被推开 3×2 = 6 个槽。
      //   这条数字同时钉死了"默认档 = 全部字段占位"：改回 Focused 档会变成 2 个槽。
      const expectSlots = 6;

      await probe.read(`await LOAD.reset(6, ${ZOOM}); await D.stopBlink(); return true;`);
      await probe.read('await D.present(); return true;');
      const base = await probe.read(lastInkLeft);

      await probe.read('return await LOAD.form(0);');
      await probe.read('return await D.stopBlink();');
      await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
      await probe.read('await D.present(); return true;');
      const form = await probe.read(lastInkLeft);

      // 比的是右缘（最右墨迹列），不是左缘。
      //   量出来过：无表单时最后一组墨迹列是 [402,427]，带表单时是 [441,446] ——
      //   左缘跳了 39px，右缘只跳 19px。原因不是排版错了，是分组变了：
      //   字距一变。 和它前面的字就从"同一组"（间隙 <=3px）变成"两组"。
      //   拿左缘当判据 = 拿一个会重组的东西当尺子。右缘只跟"最后一个字画在哪"
      //   有关，是单调的。
      const dx = form.right - base.right;
      // 为什么给的是"1.5~2.5 个括号槽"这个区间，而不是 ≈ 2 × slot × zoom 的等号：
      //   这里量的是"第一个被判为墨迹的像素列"，它是一次阈值穿越，
      //   受字形边缘抗锯齿影响有 ±2px 抖动。
      //   真正要区分的是三件事：没让开（0）／只让一个槽（≈8）／让两个（≈17）。
      //   区间 (12.5, 20.8) 把前两种都判红，又不会被 AA 抖动误伤。
      // 窗口给的是 ±1 个槽（而不是 ±0.5）：bracketSlotPx 里的 4.055pt 是
      //   native 单测里量出来的括号 advance，而浏览器表单样例用的是同一族字体
      //   的另一个字号/实例，实测槽宽 ≈ 9.33px（= 6.22pt）而不是 8.33px。
      //   真正要区分的是三件事：没让开（0）／只让一对（≈19）／让三对（≈56）——
      //   ±1 槽的窗口把前两种都判红，又不会被这点系统性偏差误伤。
      //   （只写 dx ≈ expectSlots 的话，改回 Focused 档时这里会以"差一点"的形式
      //     含糊地红掉；写成区间 + 打印实测值，一眼就能看出是档位错了还是没画。）
      check(` 行尾字形真被推开了 ${expectSlots} 个括号槽（像素证据，不是只看几何数字）`,
                base.right > 0 && dx > (expectSlots - 1) * bracketSlotPx &&
                  dx < (expectSlots + 1) * bracketSlotPx,
                `行尾墨迹右缘 ${base.right} → ${form.right}（Δ${dx} px，` +
                `期望 ${expectSlots - 1}~${expectSlots + 1} 个括号槽 = ` +
                `${((expectSlots - 1)*bracketSlotPx).toFixed(1)}~${((expectSlots + 1)*bracketSlotPx).toFixed(1)}）`);
    }

    const markForm = await probe.read(inkMark);
    check(' 表单模式下标记真的画出来了（亮蓝像素，像素证据）',
              markForm > 50, `${markForm} px`);

    // 控件是「区间」→ 它投影出的矩形来自行盒几何，短字段应当恰好 1 块
    const r0 = ct.items.length ? ct.items[0].rects : [];
    check('字段 #0 投影出 1 个矩形块，尺寸合法',
              r0.length === 1 && r0[0].w > 20 && r0[0].h > 5,
              r0.length ? `${r0[0].w.toFixed(1)} × ${r0[0].h.toFixed(1)} pt` : '无矩形');

    // 括号常驻：光标在 #0 还是在 #2，两个字段的左括号都应该在，
    //   而且墨迹量完全一样（不随光标变）。
    //   （只统计"总量"是区分不出来的 —— 所以这里逐字段取竖带。）
    //
    // 每一次 setCaret 之后仍然重新读矩形：默认档下它确实不随光标动，
    //   但"不随光标动"本身就是要验的东西 —— 缓存矩形等于把它假设掉。
    if (ct.count >= 3) {
      const rectX = async (i: any) => {
        const cc = await probe.read('return D.controls();');
        return cc.items[i] && cc.items[i].rects[0] ? cc.items[i].rects[0].x : 0;
      };

      await probe.read('return await H.setCaret({ para: 0, offset: 6 });');  // 在 #0 里
      const a0 = await probe.read(bandAt(await rectX(0)));
      const a2 = await probe.read(bandAt(await rectX(2)));

      await probe.read('return await H.setCaret({ para: 0, offset: 13 });');  // 在 #2 里
      const b0 = await probe.read(bandAt(await rectX(0)));
      const b2 = await probe.read(bandAt(await rectX(2)));

      check(' 光标在 #0 时，#0 与 #2 的左括号都在（常驻，不看光标）',
                a0 > 0 && a2 > 0, `#0 左缘 ${a0} px / #2 左缘 ${a2} px`);
      check(' 光标移到 #2：两个左括号仍然都在，且墨迹量不变（常驻的硬判据）',
                b0 > 0 && b2 > 0 && b0 === a0 && b2 === a2,
                `#0 左缘 ${a0}→${b0} px / #2 左缘 ${a2}→${b2} px`);

      // [诊断] 把 #0 那一行的蓝色列打出来 —— 用来核对"竖带窗口是否真的框住了括号"。
      {
        const d = await probe.read(`
          const f = await D.rawFrame();
          const fw = D.frameW();
          const near = ${MARK};
          const it = D.controls().items[0];
          const r0 = it.rects[0];
          const cols = new Set();
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < (r0.y - 2) * 1.5 || y > (r0.y + r0.h + 2) * 1.5) continue;
            if (near(f[i], f[i+1], f[i+2])) cols.add(x);
          }
          return { cxPx: Math.round(r0.x * 1.5), rect: r0,
                   cols: Array.from(cols).sort((a, b) => a - b) };
        `);
        console.log(`     [诊断] #0 内容左缘 ${d.cxPx} px（rect x=${d.rect.x.toFixed(2)} w=${d.rect.w.toFixed(2)}）；`
          + `该行蓝色列 = ${JSON.stringify(d.cols)}`);
      }

      // ⭐ 那个 bug：把字段内容删光，括号不能消失
      //
      //   原话：「文本控件删除最后一个字符后，括号也不渲染了，但是好像括号位置
      //   还占着，输入一个字符后括号出现」。
      //
      //   判据是像素级的，而且要分别验证左右两个括号：
      //     删空后控件矩形必然为空（rects: []），任何"按矩形找括号"的测法都会
      //     失效 —— 所以这里用光标 x 当基准：空字段里光标就停在槽的中间，
      //     左括号在它左边、右括号在它在右边（间隙各 1.5pt）。
      //     光标自己的颜色（r=31）被 mark 判据排除，不会污染计数。
      {
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await D.stopBlink();');
        await probe.read('return await H.setCaret({ para: 0, offset: 9 });');   // complaint [5,9) 的末尾
        for (let i = 0; i < 4; ++i) await probe.read(key('Backspace'));  // 真实键盘删光它
        await probe.read('await D.present(); return true;');

        const cc = await probe.read('return D.controls();');
        const k0 = cc.items[0];
        const emptyRect = k0 && (!k0.rects || k0.rects.length === 0);
        check('⭐ 前置：字段内容被删空后，它投影不出矩形'
                  + '（老实现正是死在这一步：没有矩形 → 没有标记）',
                  emptyRect, `rects=${JSON.stringify(k0 && k0.rects)}`);

        const slotPx = bracketSlotPx;
        const empty = await probe.read(`
          const st = D.editStats();
          const f = await D.rawFrame();
          const fw = D.frameW();
          const winY = (st.caretPage - D.editStats().windowFirstPage) * (D.pagePx().h + D.pagePx().gap) +
                       st.caretY;
          const mark = (r, g, b) => r > 45 && b - r > 60 && b > 140;   // 光标色 r=31 被排除
          const cx = st.caretX;
          let left = 0, right = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < winY || y >= winY + st.caretH) continue;
            if (!mark(f[i], f[i + 1], f[i + 2])) continue;
            if (x < cx - 3 && x > cx - ${Math.ceil(slotPx + 4)}) ++left;
            if (x > cx + 3 && x < cx + ${Math.ceil(slotPx + 4)}) ++right;
          }
          return { caretX: cx, caretOff: st.caretOffset, left, right };
        `);
        check(' 前置：光标停在空字段里（偏移 5）',
                  empty.caretOff === 5, `offset=${empty.caretOff}`);
        check('⭐ 空字段的左括号照样画出来了（在光标左边）',
                  empty.left > 0, `光标左 ${empty.left} px（caretX=${empty.caretX}）`);
        check('⭐ 空字段的右括号也画出来了（在光标右边）',
                  empty.right > 0, `光标右 ${empty.right} px`);

        // 引擎帧 / 屏幕也要一致：的是"屏幕上没有括号"，
        //   所以只查帧还不够 —— 上屏那一步漏了照样是他报的现象。
        const scEmpty = await probe.read('return await D.selfCheck();');
        const auEmpty = await probe.read('return await D.auditFrame();');
        check(' 删空字段后：引擎帧 == 整帧重画', auEmpty && auEmpty.diff === 0,
                  auEmpty ? `差 ${auEmpty.diff} px` : 'null');
        check(' 删空字段后：屏幕 == 引擎帧（括号真的在屏幕上）',
                  scEmpty.screenVsEngineDiffPx === 0,
                  `差 ${scEmpty.screenVsEngineDiffPx} px`);
      }

      // ⭐ 相邻字段的括号顺序：][ 不是 []
      //
      //  （截图里是 心悸[]三天）：两个字段紧挨着时，前一个字段的
      //  右括号画到了后一个字段左括号的右边 —— 顺序反了。
      //  表单样例正好就是这一对：complaint[5,9)（可编辑→亮蓝）+ duration[9,11)
      //  （只读→灰）—— 所以"谁在左"可以用颜色直接读出来，不必猜字形。
      {
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await D.stopBlink();');
        await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
        await probe.read('await D.present(); return true;');

        const ord = await probe.read(`
          const f = await D.rawFrame();
          const fw = D.frameW();
          const cc = D.controls();
          const A = cc.items[0], B = cc.items[1];      // complaint / duration(只读)
          const slotPx = ${bracketSlotPx};
          const x0 = Math.round((A.rects[0].x + A.rects[0].w) * 1.5) - 2;
          const x1 = Math.round((A.rects[0].x + A.rects[0].w) * 1.5 + 2 * slotPx) - 2;
          const isBlue = (r, g, b) => r > 45 && b - r > 60 && b > 140;
          const isGray = (r, g, b) => {
            const drg = Math.abs(r - g), dgb = Math.abs(g - b);
            return r >= 140 && r <= 195 && drg <= 8 && dgb <= 8;
          };
          let blueLo = -1, blueHi = -1, grayLo = -1, grayHi = -1;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw;
            if (x < x0 || x > x1) continue;
            const r = f[i], g = f[i+1], b = f[i+2];
            if (isBlue(r, g, b)) { if (blueLo < 0) blueLo = x; blueHi = x; }
            if (isGray(r, g, b)) { if (grayLo < 0) grayLo = x; grayHi = x; }
          }
          return { x0, x1, blueLo, blueHi, grayLo, grayHi };
        `);
        check('⭐⭐ 相邻字段共享边界：亮蓝 `]`（A 的）整个在灰 `[`（B 的）左边（`][`）',
                  ord.blueLo >= 0 && ord.grayLo >= 0 && ord.blueHi < ord.grayLo,
                  `窗口 x∈[${ord.x0},${ord.x1}]：蓝 ${ord.blueLo}..${ord.blueHi} ｜ ` +
                  `灰 ${ord.grayLo}..${ord.grayHi}` +
                  (ord.blueHi < ord.grayLo ? '' : '  ← 顺序反了，就是截图里的 `[]`'));
      }

      // ⭐ 运行期开关括号：configure({ showBrackets: false|true })
      //
      //   这条测的是"改配置也要重排"：括号进文档流，显示与否一变占位就变，
      //     正文得跟着挪。只重绘不重排的话，括号会按新的开关画、正文却按旧口径排 ——
      //     表象是括号盖在字上/槽里空一条，而且从数字上看不出来。
      //   判据取"开关前后同一个字段的竖带"，加上屏幕/帧一致。
      {
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await D.stopBlink();');
        await probe.read('return await H.setCaret({ para: 0, offset: 6 });');   // 光标在 #0
        await probe.read('await D.present(); return true;');
        const on0 = await probe.read(bandAt(await rectX(0)));
        const on2 = await probe.read(bandAt(await rectX(2)));

        await probe.read('return await SET_BRACKETS(false);');   // 括号全关
        const off0 = await probe.read(bandAt(await rectX(0)));
        const off2 = await probe.read(bandAt(await rectX(2)));
        const scSw = await probe.read('return await D.selfCheck();');
        const auSw = await probe.read('return await D.auditFrame();');

        await probe.read('return await SET_BRACKETS(true);');    // 再打开
        const back0 = await probe.read(bandAt(await rectX(0)));
        const back2 = await probe.read(bandAt(await rectX(2)));

        check(' setShowBrackets：开（两个都在）→ 关（两个都没有）→ 开（又都在）',
                  on0 > 0 && on2 > 0 && off0 === 0 && off2 === 0 && back0 > 0 && back2 > 0,
                  `开 #0=${on0}/#2=${on2} ｜ 关 #0=${off0}/#2=${off2} ｜ ` +
                  `开 #0=${back0}/#2=${back2}`);
        check(' 开关括号之后：引擎帧 == 整帧重画、且屏幕 == 引擎帧（改配置确实重排了）',
                  auSw && auSw.diff === 0 && scSw.screenVsEngineDiffPx === 0,
                  `帧自审差 ${auSw ? auSw.diff : '?'} px，屏幕差 ${scSw.screenVsEngineDiffPx} px`);
      }

      // 小点：独立开关，与括号无关。它不占宽度 → 开关不该重排，只改像素。
      {
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
        await probe.read('await D.present(); return true;');
        await probe.read('return await SET_DOTS(true);');
        const stDot = await probe.read('return D.editStats();');
        const scDot = await probe.read('return await D.selfCheck();');
        await probe.read('return await SET_DOTS(false);');
        const scOff = await probe.read('return await D.selfCheck();');
        check(' 小点开关：不触发重排（它不占宽度、不进文档流）', stDot.layoutRan === false,
                  `layoutRan=${stDot.layoutRan}`);
        check(' 小点开关：开与关之后屏幕都等于引擎帧',
                  scDot.screenVsEngineDiffPx === 0 && scOff.screenVsEngineDiffPx === 0,
                  `开差 ${scDot.screenVsEngineDiffPx} px，关差 ${scOff.screenVsEngineDiffPx} px`);
      }

      // 换字段之后，引擎帧本身必须等于"整帧重画一遍"的结果。
      //
      //   这条与上面几条的分工：上面查的是"标记画在哪"（局部像素）；
      //     这一条查的是"有没有什么东西该消失却没消失"。
      //     "光标走了、旧字段的方括号还在"就是后者 ——
      //     它可能只残留一个标记、只在某个位置，局部判据很容易漏，
      //     而"和整帧重画逐字节比"是全覆盖的。
      //
      //   为什么需要它而不是只靠 selfCheck（画布 vs 引擎帧）：
      //     如果引擎帧自己就是错的（旧标记没擦掉），画布忠实反映了它，
      //     selfCheck 两边一起错、判据照样绿。实测就是这样漏掉的。
      const aud = await probe.read('return await D.auditFrame();');
      check(' 换字段后引擎帧 == 整帧重画（帧自审，逐字节）—— 覆盖"该消失却没消失"',
                aud && aud.diff === 0,
                aud ? `差 ${aud.diff} px` + (aud.box ? ` 盒=${JSON.stringify(aud.box)}` : '')
                    : '返回 null');
    }

    //  复现那个 bug：闪烁正停在"光标隐藏"那半拍时换字段
    //
    //  这是"有时"二字的来源 —— 相位是 50/50，而所有老断言开头都调过
    //  stopBlink()，正好把这一半概率整个屏蔽掉了（所以它一直没被抓住）。
    //
    //  机制（的两组数字把它钉死了）：
    //    引擎在本次 repaint 里整行重排（占位换字段）→ 脏矩形是整帧；
    //    但 editStats.caretVisible 是 false（相位在隐藏半拍）→ 宿主的
    //      afterEdit() 判定"刚才那次把光标画成隐的"，再 present 一次，
    //      并把上屏矩形换成那一轮的（只含光标与标记那几小块）；
    //    于是第一次那次全帧重绘从头到尾没上屏 ——
    //        auditFrame() 差 0（引擎帧是对的）
    //        selfCheck() 差一千多像素（画布停在旧排版上）。
    //  的就是「光标走了、文字没动、那个字段的方括号还留在原地」。
    //
    //  判据就用他自己跑的那两个：auditFrame 必须 0（引擎侧），
    //  selfCheck 也必须 0（上屏侧）。少任何一条这个 bug 都能溜过去。
    {
      // 这一节要"第一次 present 写整帧"：
      //   触发条件是"一次配置变更让整窗重排 → present 写整帧"，
      //   用开关括号来制造（它和换字段是同一类事件：排版输入变了）。
      //   旧的 Focused 档已经删掉，不能再靠"换字段"制造重排。
      await probe.read('return await LOAD.form(0);');
      await probe.read('return await D.stopBlink();');
      await probe.read('return await H.setCaret({ para: 0, offset: 6 });');   // 光标在 #0（反复心悸）

      // 把相位摆到"隐藏"，并且让引擎的账也进入隐藏态
      await probe.read('await D.showCaret(false); await D.present(); return true;');
      const hiddenPre = await probe.read('return await D.selfCheck();');
      check('前置：把闪烁相位摆到「光标隐藏」之后，屏幕仍等于引擎帧',
                hiddenPre.screenVsEngineDiffPx === 0,
                `差 ${hiddenPre.screenVsEngineDiffPx} px`);

      // 在隐藏半拍里关掉括号（＝设置面板上点一下「显示括号」）
      await probe.read('return await SET_BRACKETS(false);');
      await probe.read('return await D.stopBlink();');   // afterEdit 会重启定时器，再停掉
      const hiddenStat = await probe.read('return D.editStats();');
      check(' 前提：关括号那一拍光标确实处于隐藏相位（宿主正是靠它决定走补救路径）',
                hiddenStat.caretVisible === false, `caretVisible=${hiddenStat.caretVisible}`);

      // 宿主补救：看到 caretVisible=false 就把光标点亮再呈现一次（app.js 走的就是这条）。
      //   配置变更不像编辑那样自动点亮光标（setShowBrackets 不算编辑），所以要显式补。
      await probe.read('await D.showCaret(true); await D.present(); return true;');
      await probe.read('return await D.stopBlink();');
      const scHide = await probe.read('return await D.selfCheck();');
      const auHide = await probe.read('return await D.auditFrame();');

      check(' 隐藏半拍关括号：引擎帧必须等于整帧重画（引擎侧无罪）',
                auHide && auHide.diff === 0,
                auHide ? `差 ${auHide.diff} px` + (auHide.box ? ` 盒=${JSON.stringify(auHide.box)}` : '')
                       : '返回 null');
      check(' 隐藏半拍关括号：屏幕像素也必须等于引擎帧 —— 这就是用户那个 bug',
                scHide.screenVsEngineDiffPx === 0,
                scHide.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${scHide.screenVsEngineDiffPx} px，盒=${JSON.stringify(scHide.screenVsEngineDiffBox)}`
                    + `（引擎写了的区域没被贴上屏）`);
      check(' 补救之后光标必须可见（宿主把隐藏半拍收尾了）',
                scHide.caret.visible === true, `visible=${scHide.caret.visible}`);

      // 对照：换字段不再重排，上屏量远小于整帧
      //
      //  这条钉死"括号常显"的收益：占位集合与光标无关 ⇒ 换字段不触发布局，
      //  上屏从"整帧"掉到"一块小矩形"。
      {
        await probe.read('return await SET_BRACKETS(true);');   // 括号开回来
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await D.stopBlink();');
        await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
        await probe.read('await D.present(); return true;');

        await probe.read('return await H.setCaret({ para: 0, offset: 14 });');      // 换字段
        const cst = await probe.read('return D.editStats();');
        const fw = await probe.read('return D.frameW();');
        const fh = await probe.read('return D.frameH();');
        check(' 换字段：不重排（layoutRan=false）',
                  cst.layoutRan === false, `layoutRan=${cst.layoutRan}`);
        check(' 换字段：上屏量远小于整帧（括号常驻的收益）',
                  cst.repaintedW > 0 && cst.repaintedW < fw / 2 &&
                    cst.repaintedH < fh / 2,
                  `上屏 ${cst.repaintedW}×${cst.repaintedH} px（整帧 ${fw}×${fh}）`);
      }

      //  光标停在字段末尾时必须落在 ] 之内（在它左边）。
      //
      //光标停在字段最后一个字上，再按一次右方向键，
      //  光标直接跳到 ] 的右边 —— 中间"在 ] 之内"的位置被跳过了，
      //  看着像光标从字里蹿出了框外。
      //
      //  判据是像素级的：量"光标那一行里最靠右的标记墨迹"（＝ ] 的字形），
      //    要求光标的右边缘在它左边。不去拿控件矩形的几何数字比 ——
      //    这一整类 bug 的教训就是"几何元数据 ≠ 绘制结果"，两边必须分别量。
      {
        await probe.read('return await LOAD.form(0);');
        await probe.read('return await D.stopBlink();');
        // 表单样例：complaint[5,9) / duration[9,11) 只读 / aggravate[12,16)
        // aggravate 是最右一个可编辑字段 → 它行内最右的标记墨迹就是它的 ]。
        await probe.read('return await H.setCaret({ para: 0, offset: 16 });');   // 「天」右边、`]` 左边
        await probe.read('await D.present(); return true;');

        const cmp = await probe.read(`
          const st = D.editStats();
          const f = await D.rawFrame();
          const fw = D.frameW();
          const winY = (st.caretPage - D.editStats().windowFirstPage) * (D.pagePx().h + D.pagePx().gap) +
                       st.caretY;
          const near = ${MARK};   // 标记亮蓝；光标色 r=31 被排除
          let bracketX = -1;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < winY || y >= winY + st.caretH) continue;
            if (!near(f[i], f[i + 1], f[i + 2])) continue;
            if (x > bracketX) bracketX = x;
          }
          return { caretX: st.caretX, caretW: st.caretW, bracketX,
                   caretOff: st.caretOffset };
        `);
        check(' 光标确实停在字段末尾（偏移 16）',
                  cmp.caretOff === 16, `offset=${cmp.caretOff}`);
        check(' 字段末尾的光标落在 `]` 之内（光标右边缘在最右标记墨迹左边）',
                  cmp.bracketX > 0 && cmp.caretX + cmp.caretW <= cmp.bracketX + 1,
                  `光标 x∈[${cmp.caretX}, ${cmp.caretX + cmp.caretW}] ｜ ` +
                  `该行最右标记墨迹 x=${cmp.bracketX}` +
                  (cmp.caretX + cmp.caretW <= cmp.bracketX + 1
                    ? '' : '  ← 光标跑到 `]` 右边去了'));
      }

    //  方向键一次一格，但只在可编辑内容里走：表单模式的插入点不进括号之间的空档、
    //  也不进正文（选区才自由）。走到内容两端就停住，换字段用 Tab。
    //
    //  判据是逐步的：一次只跨一枚簇，到内容末尾就停住 —— 只看终点抓不到"投影式移动"。
    {
      await probe.read('return await LOAD.form(0);');
      await probe.read('return await D.stopBlink();');
      await probe.read('return await H.setCaret({ para: 0, offset: 12 });');   // #2 = aggravate [12,16)

      await probe.read(key('ArrowLeft'));                          //  真实键盘
      const afterLeft = await probe.read('return CARET();');
      check('★ 在 #2 内容起点按左方向键：不退出字段（括号之间那一格不可插入）',
                afterLeft.offset === 12 && afterLeft.side === 1,
                `12/1 → ${afterLeft.offset}/${afterLeft.side}`);

      // 右走 6 步：13 14 15 16（到内容末尾就停住），单步跨度恒为 1
      await probe.read('return await H.setCaret({ para: 0, offset: 12 });');
      const seq = [];
      for (let i = 0; i < 6; ++i) {
        await probe.read(key('ArrowRight'));
        seq.push((await probe.read('return CARET();')).offset);
      }
      let worst = 0;
      let prev = 12;
      for (const o of seq) { worst = Math.max(worst, o - prev); prev = o; }
      check('★ 从 #2 起点右走 6 步：一次一格，到内容末尾就停住',
                seq.join(',') === '13,14,15,16,16,16',
                `12 → ${seq.join(' → ')}`);
      check(' 单步跨度恒为 1（没有任何一步瞬移）',
                worst === 1, `最大单步跨度 ${worst}`);
    }

      // 同一件事用"敲字"再走一遍（这条路径由打字触发）
      await probe.read('return await LOAD.form(0);');
      await probe.read('return await D.stopBlink();');
      await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
      await probe.read('await D.showCaret(false); await D.present(); return true;');
      await probe.read(key('x'));                     //  真实键盘路径
      await probe.read('return await D.stopBlink();');
      const scType = await probe.read('return await D.selfCheck();');
      const auType = await probe.read('return await D.auditFrame();');
      check(' 隐藏半拍敲字：引擎帧自审必须 0',
                auType && auType.diff === 0,
                auType ? `差 ${auType.diff} px` : '返回 null');
      check(' 隐藏半拍敲字：屏幕像素必须等于引擎帧',
                scType.screenVsEngineDiffPx === 0,
                scType.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${scType.screenVsEngineDiffPx} px，盒=${JSON.stringify(scType.screenVsEngineDiffBox)}`);
    }

    // 只读模式：这是"策略生效"最干净的可观测形式
    //   （表单模式下光标被 clamp 在可编辑字段内，反而没法用普通按键走到"区间外"）
    await probe.read('return await SET_MODE(2);');
    const txt0 = await probe.read('return H.paragraph(0);');
    const blk0 = await probe.read('return D.blockedEdits();');
    for (const ch of 'abc') await probe.read(key(ch));
    const txt1 = await probe.read('return H.paragraph(0);');
    const blk1 = await probe.read('return D.blockedEdits();');
    check(' 只读模式：按键全被拒、文本一字不变、被拒次数 +3',
              txt1 === txt0 && blk1 === blk0 + 3,
              `被拒 ${blk1 - blk0} 次，文本${txt1 === txt0 ? '未变' : '变了'}`);

    // 表单模式：可编辑字段内能打进去，且区间跟着变长
    await probe.read('return await LOAD.form(0);');
    await probe.read('return await H.setCaret({ para: 0, offset: 6 });');
    const len0 = await probe.read('return Array.from(H.paragraph(0)).length;');
    await probe.read(key('测'));
    const len1 = await probe.read('return Array.from(H.paragraph(0)).length;');
    const ct2 = await probe.read('return D.controls();');
    check(' 表单模式：字段内按键成功，且字段区间跟着变长（9 → 10）',
              len1 === len0 + 1 && ct2.items[0].end[1] === 10,
              `码点 ${len0} → ${len1}，字段 #0 end=${ct2.items[0].end[1]}`);

    // 只读模式：标记必须一个像素都不剩。
    //   文档模式与表单模式都画括号（编辑态要一眼看出哪块是控件），只有只读不画 ——
    //   它是要印出来的呈现态，画上去就印出来了。三档的这一条口径见 [15]。
    await probe.read('return await SET_MODE(2);');
    const roMark = await probe.read(inkMark);
    check('★ 只读模式：标记一个像素都不剩', roMark === 0, `${roMark} px`);

    // 文档模式：标记照常画出来（不再是"只有表单模式才看得见控件边界"）
    await probe.read('return await SET_MODE(0);');
    const docMark = await probe.read(inkMark);
    check('★ 文档模式：标记照常出现（编辑态看得见哪块是控件）', docMark > 0,
              `${docMark} px`);

    // 切回表单模式后标记仍在（证明不是"切走一次就再也不画"）
    await probe.read('return await SET_MODE(1);');
    const backMark = await probe.read(inkMark);
    check(' 切回表单模式：标记也没有丢', backMark > 0, `${backMark} px`);

    // 穷尽不变式（在这一侧尤其重要：native 用的是自写光栅器，
    //     浏览器是 Skia，两边的字形光栅/度量并不完全相同）。
    //
    //   判据（默认档 All）：光标走遍每一个可停位置，每一步都查每个可编辑
    //     字段的左缘外侧 —— 括号是常驻的，所以每一步都必须有墨迹，而且
    //     每个字段自己的墨迹量必须恒定（不随光标变）。
    //     只查"聚焦字段有、其余为空"是 Focused 档的旧判据，在这里会假红。
    //     为什么值得穷尽：是"有时括号渲染不更新"。单点断言抓不到
    //     "某个位置刚好漏掉"，而漏掉几乎总是"某个矩形和光标重叠、被后画的东西盖掉"
    //     造成的 —— 只有走遍位置才碰得到。
    //
    // 整个循环放在一次 eval 里：否则 18 次 setCaret × N 个字段 = 几十次跨界，
    // 每次还要拷一份 4.5 MB 的帧，慢到没法用。
    const exh = await probe.read(`
      const near = ${MARK};
      const total = Array.from(H.paragraph(0)).length;
      let checked = 0, bad = 0, firstBad = '';
      // 每字段的 min/max 存成对象（{ci: [lo, hi]}），不能存数组：
      //   只读字段会被跳过 → 稀疏数组在跨进程回传时被序列化成 null
      //   （「Cannot read properties of null (reading '0')」，而且栈里
      //    看不到是哪一行 —— 因为整段是 eval 的字符串）。
      const seen = {};
      for (let off = 0; off <= total; ++off) {
        await H.setCaret({ para: 0, offset: off });
        const cc = D.controls();
        const f = await D.rawFrame();
        const fw = D.frameW();
        for (let ci = 0; ci < cc.items.length; ++ci) {
          const it = cc.items[ci];
          if (it.ro || !it.rects.length) continue;
          const cx = it.rects[0].x;
          const lo = Math.round((cx - 10) * 1.5), hi = Math.round((cx - 1) * 1.5);
          let n = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const x = (i >> 2) % fw;
            if (x < lo || x > hi) continue;
            if (near(f[i], f[i+1], f[i+2])) ++n;
          }
          ++checked;
          if (n === 0) { ++bad; if (!firstBad) firstBad = \`off=\${off} 字段#\${ci} 应有括号却为 0\`; }
          const cur = seen[ci] || [n, n];
          seen[ci] = [Math.min(cur[0], n), Math.max(cur[1], n)];
        }
      }
      return { checked, bad, total, firstBad, perField: seen };
    `);
    check(' 穷尽不变式：光标走遍全部可停位置，每个字段的括号始终在（All 档常驻）',
              exh.checked > 20 && exh.bad === 0,
              `${exh.checked} 项检查（走 ${exh.total + 1} 个位置），不符 ${exh.bad} 项` +
                (exh.firstBad ? `；首个：${exh.firstBad}` : ''));
    const pf = Object.entries(exh.perField)
      .map(([k, v]: [string, any]) => ({ ci: Number(k), lo: v[0], hi: v[1] }));
    check(' 穷尽不变式：每个字段括号的墨迹量与光标位置恒定（常驻的硬判据）',
              pf.length > 0 && pf.every((p) => p.lo === p.hi && p.lo > 0),
              pf.map((p) => `#${p.ci} ${p.lo}~${p.hi}`).join(' ｜ '));

    // 给用户用的取证接口也当场跑一遍 —— 它要是抛异常，用户那边就白跑了。
    const mk = await probe.read('return await D.markCheck();');
    check(' 用户自检接口可用且当场判为一致（diag.markCheck()）',
              !!mk && mk.allOk === true && Array.isArray(mk.rows) && mk.rows.length >= 1,
              mk ? `${mk.rows.length} 个可编辑字段，全部一致=${mk.allOk}` : '返回 null');

    // Tab = 缩进（不是跳字段）
    //
    //   语义纠偏：Tab 在编辑器里是缩进。绑成"跳到下一个控件"是表单/网页里
    //     字段之间的约定，放到正文编辑器里会把肌肉记忆打断（用户明确纠正过）。
    //     字段之间用鼠标点、或者方向键（光标本来就会跨字段）。
    //   改完之后"跳字段"只剩程序化入口 focusControl（探针/自动化用），
    //     下面单独验它还在。
    //
    //   这一段的核心判据是"真实按键 → 屏幕像素 == 引擎帧"。它抓的是这一类：
    //     某个入口改了引擎状态却没给自己收尾，宿主于是拿着上一次呈现的
    //     脏矩形去上屏，画布变成"旧内容 + 局部新内容"的混合体
    //     （的"光标走了、旧括号还在"）。
    {
      await probe.read('return await LOAD.form(0);');
      await probe.read('return await H.setCaret({ para: 0, offset: 6 });');   // k1 内
      await probe.read('await D.stopBlink(); await D.present(); return true;');

      const beforeTab = await probe.read('return await D.selfCheck();');
      check('前置：按 Tab 之前屏幕与引擎帧一致（否则下面的判据没意义）',
                beforeTab.screenVsEngineDiffPx === 0, `差 ${beforeTab.screenVsEngineDiffPx} px`);

      const nBefore = await probe.read('return Array.from(H.paragraph(0)).length;');
      await probe.read(key('Tab'));                              //  真实键盘路径
      const nAfter = await probe.read('return Array.from(H.paragraph(0)).length;');
      const postTab = await probe.read('return await D.selfCheck();');
      const liveOff = await probe.read('return D.caretLive();');
      const auditTab = await probe.read('return await D.auditFrame();');

      check(' 真实 Tab 键 = 缩进：文档 +4 个码点，且是 4 个空格',
                nAfter === nBefore + 4 &&
                  (await probe.read('return H.paragraph(0).slice(6, 10) === "    ";')),
                `码点 ${nBefore} → ${nAfter}`);
      check(' Tab 之后光标停在新插入的空格之后（offset=10）',
                postTab.caret.offset === 10, `offset=${postTab.caret.offset}`);
      check(' Tab 之后"编辑统计里的光标"必须等于"引擎实时光标"'
                + ' —— 即这个入口自己把画面收尾了，宿主不会拿到过期脏矩形',
                postTab.caret.offset === liveOff,
                `统计 ${postTab.caret.offset} ／ 实时 ${liveOff}` +
                (postTab.caret.offset === liveOff ? '' : '  ← 有入口改了状态却没重绘'));
      check(' Tab 之后屏幕像素必须等于引擎帧（不许出现"引擎对了、屏幕还是旧的"）',
                postTab.screenVsEngineDiffPx === 0,
                postTab.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${postTab.screenVsEngineDiffPx} px，盒=${JSON.stringify(postTab.screenVsEngineDiffBox)}`);
      check(' Tab 之后引擎帧必须等于"整帧重画一遍"的结果（帧自审，逐字节）',
                auditTab && auditTab.diff === 0,
                auditTab ? `差 ${auditTab.diff} px` + (auditTab.box ? ` 盒=${JSON.stringify(auditTab.box)}` : '')
                         : '返回 null');

      // Shift+Tab 反缩进
      await probe.read(key('Tab', ', shiftKey: true'));
      const nBack = await probe.read('return Array.from(H.paragraph(0)).length;');
      check(' Shift+Tab = 反缩进：把 4 个空格删回来',
                nBack === nBefore, `码点 ${nAfter} → ${nBack}`);
    }

    // 程序化跳字段（探针/自动化入口，不再绑键盘）
    {
      await probe.read('return await LOAD.form(0);');
      await probe.read('return await H.setCaret({ para: 0, offset: 5 });');
      const f1 = await probe.read('await H.focusControl("next"); return D.editStats();');
      const c1 = await probe.read('return CARET();');
      const f2 = await probe.read('await H.focusControl("next"); return D.editStats();');
      const c2 = await probe.read('return CARET();');
      check(' focusControl（程序化跳字段）：12 → 5 绕回第一个字段',
                !!f1 && !!f2 && c1.offset === 12 && c2.offset === 5,
                `${c1.offset} → ${c2.offset}`);
      const post = await probe.read('return await D.selfCheck();');
      const audit = await probe.read('return await D.auditFrame();');
      check(' 跳字段之后：屏幕==引擎帧、且引擎帧==整帧重画',
                post.screenVsEngineDiffPx === 0 && audit && audit.diff === 0,
                `屏幕差 ${post.screenVsEngineDiffPx} px，帧自审差 ${audit ? audit.diff : '?'} px` +
                `，盒=${JSON.stringify(post.screenVsEngineDiffBox)}` +
                `，上屏=${JSON.stringify(await probe.read('return D.blitStats();'))}`);
      const mk2 = await probe.read('return await D.markCheck();');
      check(' 跳字段后每个字段的括号都还在（常显；markCheck 自检）',
                !!mk2 && mk2.allOk === true && mk2.showBrackets === true,
                mk2 ? `${mk2.rows.length} 个可编辑字段，showBrackets=${mk2.showBrackets}，`
                      + `全部一致=${mk2.allOk}` : '返回 null');
    }
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
