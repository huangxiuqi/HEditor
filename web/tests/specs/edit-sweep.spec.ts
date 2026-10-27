// 编辑面（四）：控件边界总覆盖，真实鼠标横扫 + 真实按键。
//
// 相邻单共享、三连双共享、嵌套三层、换行、删空、括号外侧：每一种形状都
// 既正查（命中测试说落哪）又反查（点下去真落哪），两半互为反向。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit, key, sleep, ZOOM } from '../support/edit-helpers';

test.describe('编辑面：控件边界总覆盖', () => {
  test('相邻、嵌套、换行、删除、添加、光标六种形状都不留死区', async ({ open }) => {
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

  log.section('[14] 控件边界总覆盖：真实鼠标点选 + 真实按键（相邻 / 嵌套 / 换行 / 删除 / 添加 / 光标）');
  {
    //  这一节是"控件边界"在浏览器里的主场。判据与 [12] 同源：凡是用户用手指和
    //  键盘做出来的动作，都必须走真实事件（CDP 的 mousePressed/mouseReleased、
    //  派到真实 textarea 上的 keydown），不能调引擎 API 代替 —— 坏点往往就在
    //  "坐标换算 → 命中 → 归属 → 落笔"这条链的接缝上，而引擎 API 把接缝绕过去了。
    //
    //  括号的排布只存在于排版结果里（区间数据看不出来：][][] 与 [[ ]] 的区间
    //  一模一样），所以用 diag.bracketSlots() 把槽清单读出来核对：每一枚括号的
    //  偏移、开闭、归属、所在行、行内 x 都在里面。
    // 换算因子读门面的 zoomLevel（密度：设备像素 / 磅）。不要读 #zoom 输入框 ——
    //   那一格是倍数（1 = 100% = 96 dpi），两者差 4 / 3，读错就是整片点不准。
    const zoomNow = async () => probe.read('return ZOOM();');
    const geo = async () => probe.read(`
      const cv = FRAME(), r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height };`);
    // 帧磅 → CSS 像素：与页面 onCanvasMouseDown 里那套换算同源（先乘 zoom 再乘缩放比）
    const clickPt = async (g: any, xPt: any, yPt: any, z: any) => {
      const p = { x: g.left + xPt * z * g.sx, y: g.top + yPt * z * g.sy,
                  button: 'left', clickCount: 1, buttons: 1 };
      await probe.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await probe.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      // 8 ms 只够同步宿主。Worker 宿主的点击是一条链（命中测试一次往返 → 落光标 →
      //   推状态），读早了拿到的是上一个点的结果 —— 而横扫是 x 逐 1 磅前进的，
      //   "上一个点的结果"恰好差一个簇，看起来就像引擎在边界上算错了一格。
      //   排队排空之后再读，两边才可比。
      await sleep(8);
      await probe.read('await H.flush(); return true;');
    };
    const load = async (v: any) => {
      await probe.read(`return await LOAD.form(${v});`);
      await probe.read('return await D.stopBlink();');
    };
    const caretOf = () => probe.read(`
      const q = CARET();
      return { o: q.offset, s: q.side, f: D.controls().caret };`);
    const items = () => probe.read('return D.controls().items.map(i => [i.start[1], i.end[1]]);');
    const slotsOf = (cp: any) => probe.read(`
      return (await D.bracketSlots() || [])
        .filter(s => s.para === 0 && s.cp === ${cp})
        .map(s => ({ side: s.side, owner: s.owner, ro: s.ro, line: s.line, x: s.x }));`);
    // BracketSide：1 = 开，2 = 闭（枚举在 layout.h）
    const sidesOf = (arr: any) => arr.map((s: any) => (s.side === 1 ? '[' : ']')).join('');
    // 某个控件内容框左缘内侧 2 磅处（帧磅）。那里只被这个字段自己（以及它的外层）
    //   覆盖，所以归属必须是最内层 —— 也就是这个字段自己。
    //   不用"第一个字的中点"：相邻字段共享边界时，槽带把两个字的几何挤在一起，
    //   量出来的中点可能落到里层字段的框里（判据本身就不可靠，见 [19]）。
    const fieldSpot = (ci: any) => probe.read(`
      const q = D.controls().items[${ci}].rects[0];
      return { x: q.x + 2, y: q.y + q.h / 2 };`);

    // 某个控件在某偏移上的"内侧"插入点（帧磅）。
    //   caret.x / caret.w 是帧像素（乘过 zoom），除以 zoom 换成帧磅；x 是光标矩形的左缘，
    //   插入点还要再往右半个光标宽 —— 少算这半个，点选位置就会落在括号格的正中偏左。
    const innerCaretPt = async (cp: any, ci: any) => {
      const r = JSON.parse(await probe.read(`
        await D.setCaretAt(0, ${cp}, ${ci});
        const q = CARET();
        return JSON.stringify({ x: q.x, w: q.w,
                                z: ZOOM() });`));
      return r.x / r.z + (r.w / r.z) * 0.5;
    };
    // 点"某字段开括号的内侧"那一格：落在括号与它后面那个字之间、靠右的一小段。
    //   括号占位簇的势力范围按左右半边分 —— 左半边是"括号之前"（控件之外）、右半边是
    //   "括号之后"（控件之内）。要"点进这个字段"，落点得在右半边，所以取内侧插入点左侧
    //   半磅：那里既在右半边之内，又紧贴着字段的第一个字。
    const clickOpenInner = async (g: any, z: any, y: any, cp: any, ci: any) => {
      const inner = await innerCaretPt(cp, ci);
      await clickPt(g, inner - 0.5, y, z);
    };
    log.section('[14.1] 相邻单共享边界：点 ] 归前一个、点 [ 归后一个（variant 0）');
    {
      await load(0);
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s9 = await slotsOf(9);
      check('边界 9 上是 ][，两枚括号各归各的字段',
                s9.length === 2 && sidesOf(s9) === '][' && s9[0].owner === 0 && s9[1].owner === 1,
                `${sidesOf(s9)} ｜ 归属 ${s9.map((s: any) => s.owner).join(' ')}`);
      check('只读字段的括号是灰的（颜色跟着字段走，不跟着位置走）',
                s9.length === 2 && !s9[0].ro && s9[1].ro,
                `ro ${s9.map((s: any) => (s.ro ? 1 : 0)).join(' ')}`);
      const pitch = s9.length === 2 ? Math.max(1, s9[1].x - s9[0].x) : 6;

      await clickPt(g, s9[0].x + pitch * 0.5, y, z);
      const a = await caretOf();
      check('★ 真实鼠标点 ] → 光标停在 9 且归前一个字段', a.o === 9 && a.f === 0,
                `offset ${a.o} 归属 ${a.f}`);
      // 点只读字段的开括号内侧：插入点进不去只读区（选区才进得去），光标留在前一个字段。
      await clickOpenInner(g, z, y, 9, 1);
      const b = await caretOf();
      check('★ 真实鼠标点只读字段的 [ → 光标不进只读区（留在前一个字段的位置上）',
                b.o === 9 && b.f === 0, `offset ${b.o} 归属 ${b.f}`);

      // 于是这一下敲字进的是光标所在的那个字段 —— 只读区的拒绝在引擎判据里单独钉着。
      await probe.read(key('痛'));
      const it1 = await items();
      check('★ 点 ] 之后真实按键 → 字进前一个字段，后一个整体右移',
                JSON.stringify(it1) === '[[5,10],[10,12],[13,17]]', JSON.stringify(it1));
      const au = await probe.read('return await D.auditLayout();');
      check('★★ 点选 + 打字之后增量排版等于全量重排', !!au && au.diff === 0,
                au ? `diff ${au.diff}` : 'null');
    }

    log.section('[14.2] 相邻双共享边界（三连）：两条边界都是 ][，落笔跟着归属（variant 2）');
    {
      await load(2);
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s7 = await slotsOf(7);
      const s9 = await slotsOf(9);
      check('★ 两条共享边界都是 ][', sidesOf(s7) === '][' && sidesOf(s9) === '][',
                `7:${sidesOf(s7)} 9:${sidesOf(s9)}`);
      const p7 = s7.length === 2 ? Math.max(1, s7[1].x - s7[0].x) : 6;

      await clickOpenInner(g, z, y, 7, 1);
      const a = await caretOf();
      check('★ 真实鼠标点中间那个字段的开括号 → 归属 1', a.o === 7 && a.f === 1,
                `offset ${a.o} 归属 ${a.f}`);
      await probe.read(key('痛'));
      const it1 = await items();
      check('★ 接着打字 → 字进中间那个字段，左邻居一个字都没变',
                JSON.stringify(it1) === '[[5,7],[7,10],[10,12]]', JSON.stringify(it1));

      await load(2);
      const s7b = await slotsOf(7);
      await clickPt(g, s7b[0].x + p7 * 0.5, y, z);
      const b = await caretOf();
      check('★ 同一条边界上点 ] → 归属 0', b.o === 7 && b.f === 0,
                `offset ${b.o} 归属 ${b.f}`);
      await probe.read(key('痛'));
      const it2 = await items();
      check('★ 接着打字 → 字进前一个字段，后两个整体右移',
                JSON.stringify(it2) === '[[5,8],[8,10],[10,12]]', JSON.stringify(it2));

      // 真实方向键：一次一格 —— 括号也占一格，所以共享边界 ][ 要按两下。
      await probe.read('return await H.setCaret({ para: 0, offset: 5 });');
      const walk = [];
      for (let k = 0; k < 6; ++k) {
        await probe.read(key('ArrowRight'));
        const a = await caretOf();
        walk.push(`${a.o}/${a.s}`);
      }
      // #0 已经被前面那两笔墨迹撑成 [5,8)：一路右走停在 8/0（内容末尾），不出括号。
      check('★ 真实方向键一次一格，停在 #0 的内容末尾（不跨过 ][）',
                JSON.stringify(walk) ===
                  JSON.stringify(['6/0', '7/0', '8/0', '8/0', '8/0', '8/0']),
                `落点 ${walk.join(' ')}`);
      await probe.read('return await H.setCaret({ para: 0, offset: 12 });');
      const back = [];
      for (let k = 0; k < 4; ++k) {
        await probe.read(key('ArrowLeft'));
        const a = await caretOf();
        back.push(`${a.o}/${a.s}`);
      }
      check('★ 反向同样一次一格，停在最后一个字段的内容起点',
                JSON.stringify(back) === JSON.stringify(['11/0', '10/2', '10/2', '10/2']),
                `落点 ${back.join(' ')}`);

      // 归属随位置换人（重新载入样例，让偏移回到干净的三连区间）
      await load(2);
      await probe.read('return await H.setCaret({ para: 0, offset: 7 });');
      const f7 = (await caretOf()).f;
      await probe.read(key('ArrowRight'));
      const mid = await caretOf();
      await probe.read(key('ArrowRight'));
      const inB = await caretOf();
      check('★ 方向键停在共享边界上（不跨过去，归属也不换人）',
                f7 === 0 && mid.o === 7 && mid.s === 0 && mid.f === 0 && inB.o === 7 &&
                  inB.f === 0,
                `7→${f7} ｜ 右一步 ${mid.o}/${mid.s}→归 ${mid.f} ｜ 再一步→${inB.o}/${inB.f}`);

      // Home / End 是行首行尾，不是字段首尾
      const ncp = await probe.read('return CARET().docCodepoints;');
      await probe.read('return await H.setCaret({ para: 0, offset: 9 });');
      await probe.read(key('Home'));
      const h = (await caretOf()).o;
      await probe.read(key('End'));
      const e2 = (await caretOf()).o;
      check('★ 真实 Home/End 收到字段内容的首尾（行首行尾在字段外）', h === 7 && e2 === 9,
                `Home→${h} End→${e2}（文档 ${ncp} 个码点）`);
    }

    log.section('[14.3] 删除文字：真实退格把后两个字段删空，括号必须交替成 ][][]（variant 2）');
    {
      await load(2);
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      // 用真实 Backspace 键把 #2、#1 依次删空（每删一次都用删除前的端点重新定位光标）
      for (const ci of [2, 1]) {
        for (let guard = 0; guard < 8; ++guard) {
          const cur = await items();
          if (cur[ci][0] === cur[ci][1]) break;
          await probe.read(`return await D.setCaretAt(0, ${cur[ci][1]}, ${ci});`);
          await probe.read(key('Backspace'));
        }
      }
      const it = await items();
      check('两个字段被真实退格删成零宽，仍收在 7 上',
                JSON.stringify(it) === '[[5,7],[7,7],[7,7]]', JSON.stringify(it));
      const s = await slotsOf(7);
      check('★ 该处五枚括号交替成 ][][]（不是互相嵌套成 [[ ]]）',
                s.length === 5 && sidesOf(s) === '][][]', `${sidesOf(s)}（${s.length} 枚）`);
      check('★ 五个槽各归各的字段（0 1 1 2 2）',
                s.map((q2: any) => q2.owner).join(' ') === '0 1 1 2 2',
                s.map((q2: any) => q2.owner).join(' '));

      // 颜色跟着字段走：把 #2 标成只读，它那一对括号必须变灰且排在右侧。
      //   只读标记只有建控件时给得进去，所以整份重建一次（区间不变，只翻 ro）。
      await probe.read(`
        const it2 = D.controls().items;
        return await LOAD.controls(it2.map((x, i) => ({
          start: x.start, end: x.end, kind: 0, id: x.id, ro: i === 2 })));`);
      const sr = await slotsOf(7);
      check('★ 只读字段的一对括号变灰（左三枚可编辑、右两枚只读）',
                sr.length === 5 && sr.map((q2: any) => (q2.ro ? 1 : 0)).join('') === '00011',
                sr.map((q2: any) => (q2.ro ? 1 : 0)).join(''));

      // 删空之后那个零宽字段还得能进、能打：真实鼠标点它那一对 ([ ]) 里的 [ 位置
      await probe.read(key('痛'));  // 先确认当前光标（应停在 #1 的零宽位置上）
      const cur = await items();
      check('★ 删空之后光标仍停在那个零宽字段上，字进它自己（左右邻居没被撑大）',
                JSON.stringify(cur) === '[[5,7],[7,8],[8,8]]', JSON.stringify(cur));

      // 真实鼠标点 #2 的开括号 → 归属 2（两个零宽字段不许被认成同一个）
      await load(2);
      for (const ci of [2, 1]) {
        for (let guard = 0; guard < 8; ++guard) {
          const k = await items();
          if (k[ci][0] === k[ci][1]) break;
          await probe.read(`return await D.setCaretAt(0, ${k[ci][1]}, ${ci});`);
          await probe.read(key('Backspace'));
        }
      }
      const s2 = await slotsOf(7);
      // 槽序是 ] [ ] [ ]：#2 的开括号在下标 3
      await clickOpenInner(g, z, y, 7, 2);
      const at2 = await caretOf();
      check('★ 真实鼠标点最右那枚 [（#2 的开括号）→ 归属 2',
                at2.o === 7 && at2.f === 2, `offset ${at2.o} 归属 ${at2.f}`);
      await probe.read(key('痛'));
      const it3 = await items();
      check('★ 接着打字 → 字进 #2 自己（#1 还是零宽）',
                JSON.stringify(it3) === '[[5,7],[7,7],[7,8]]', JSON.stringify(it3));
      const au3 = await probe.read('return await D.auditLayout();');
      check('★★ 删空与回填的每一步增量排版都等于全量重排', !!au3 && au3.diff === 0,
                au3 ? `diff ${au3.diff}` : 'null');
    }

    log.section('[14.4] 嵌套：三层各自点得进、内层优先、删空后仍是最内层（variant 3）');
    {
      await load(3);
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const six: Record<number, any> = {};
      for (const cp of [5, 6, 7, 9, 10, 11]) six[cp] = await slotsOf(cp);
      check('★ 六处边界各一枚括号：开在前三、闭在后三（[外[中[内 … ]内]中]外）',
                sidesOf(six[5]) === '[' && sidesOf(six[6]) === '[' && sidesOf(six[7]) === '[' &&
                sidesOf(six[9]) === ']' && sidesOf(six[10]) === ']' && sidesOf(six[11]) === ']',
                [5, 6, 7, 9, 10, 11].map((cp) => `${cp}:${sidesOf(six[cp])}`).join(' '));

      // 真实鼠标点三层各自内容框的左缘内侧 → 归属必须是最内层
      const owners = [];
      for (const ci of [0, 1, 2]) {
        const m = await fieldSpot(ci);
        await clickPt(g, m.x, m.y, z);
        owners.push((await caretOf()).f);
      }
      check('★ 真实鼠标点三层各自的第一个字 → 归属 0 / 1 / 2（内层优先）',
                owners.join(' ') === '0 1 2', owners.join(' '));

      // 真实按键在内层打字：外中内三层一起变长
      await probe.read('return await D.setCaretAt(0, 8, 2);');
      await probe.read(key('痛'));
      const it = await items();
      check('★ 真实按键在内层打字 → 外中内三层一起变长（区间不许互相穿过）',
                JSON.stringify(it) === '[[5,12],[6,11],[7,10]]', JSON.stringify(it));

      // 真实退格把内层删空
      for (let guard = 0; guard < 6; ++guard) {
        const cur = await items();
        if (cur[2][0] === cur[2][1]) break;
        await probe.read(`return await D.setCaretAt(0, ${cur[2][1]}, 2);`);
        await probe.read(key('Backspace'));
      }
      const it2 = await items();
      const s7b = await slotsOf(7);
      check('★ 删空的内层画成一对相邻的 []，仍嵌在中层的边界之内',
                JSON.stringify(it2) === '[[5,9],[6,8],[7,7]]' && sidesOf(s7b) === '[]',
                `${JSON.stringify(it2)} ｜ 7:${sidesOf(s7b)}`);
      await probe.read('return await H.setCaret({ para: 0, offset: 7 });');
      const f7 = (await caretOf()).f;
      await probe.read(key('痛'));
      const it3 = await items();
      check('★ 真实按键在删空的内层上打字 → 字进它自己，外中两层一起外推',
                f7 === 2 && JSON.stringify(it3) === '[[5,10],[6,9],[7,8]]',
                `归属 ${f7} ｜ ${JSON.stringify(it3)}`);
      const au4 = await probe.read('return await D.auditLayout();');
      check('★★ 嵌套的每一步增量排版都等于全量重排', !!au4 && au4.diff === 0,
                au4 ? `diff ${au4.diff}` : 'null');

      await load(3);
      await probe.read('return await D.setCaretAt(0, 8, 2);');
      await probe.read('await D.present(); return true;');
      await sleep(150);
      await probe.shot('boundary-nested');
    }

    log.section('[14.5] 换行：跨行字段的两个括号落在不同行，嵌套的只读内层点得进打不进（variant 1）');
    {
      await load(1);
      const cc = await probe.read('return D.controls();');
      check('长字段跨行（矩形块 > 1），只读小字段嵌在它里面',
                cc.items[0].rects.length >= 2 && cc.items[1].depth === 1 && cc.items[1].ro,
                `#0 块数 ${cc.items[0].rects.length} ｜ #1 depth ${cc.items[1].depth} ro ${cc.items[1].ro}`);
      const s5 = await slotsOf(5);
      const s60 = await slotsOf(60);
      check('★ 跨行字段的开括号与闭括号落在不同的行上',
                s5.length === 1 && s60.length === 1 && s5[0].line !== s60[0].line,
                `开括号行 ${s5.length ? s5[0].line : '-'} ｜ 闭括号行 ${s60.length ? s60[0].line : '-'}`);

      const g = await geo();
      const z = await zoomNow();
      const mB = await fieldSpot(1);
      await clickPt(g, mB.x, mB.y, z);
      const inB = await caretOf();
      const blk0 = await probe.read('return D.blockedEdits();');
      await probe.read(key('痛'));
      const blk1 = await probe.read('return D.blockedEdits();');
      const itb = await items();
      // 只读控件里没有插入点：点它就跟点正文一样不落光标（选区另算，那是另一条线）。
      //   这一条同时压住"外层可编辑不能替内层作证" —— 归属取最内层才判得出不可插入。
      check('★ 点嵌套里的只读内层 → 光标不进（插入点只在能打字的地方），打字被拒',
                inB.f !== 1 && blk1 === blk0 + 1 && JSON.stringify(itb[1]) === '[20,26]',
                `归属 ${inB.f} ｜ blocked ${blk0}→${blk1} ｜ #1 ${JSON.stringify(itb[1])}`);

      const mA = await fieldSpot(0);
      await clickPt(g, mA.x, mA.y, z);
      const inA = await caretOf();
      await probe.read(key('痛'));
      const ita = await items();
      check('★ 点外层（不在内层框里）→ 归属 0，字进外层、内层整体右移',
                inA.f === 0 && JSON.stringify(ita) === '[[5,61],[21,27]]', JSON.stringify(ita));
      const au5 = await probe.read('return await D.auditLayout();');
      check('★★ 跨行字段编辑之后增量排版等于全量重排', !!au5 && au5.diff === 0,
                au5 ? `diff ${au5.diff}` : 'null');
    }

    log.section('[14.6] 真实鼠标横扫：命中测试说什么，点击就落在什么上（含无死区）');
    {
      // 这一节量的是坐标链（CDP 取整、y 折算、簇边界），与落点策略无关：表单模式下控件外
      //   的采样点本来就该被拒（插入点只在能打字的地方），在这里跑会把两件事混成一条。
      await load(2);
      await probe.read('return await SET_MODE(0);');
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const lo = await probe.read(
        'return Math.min(...D.controls().items.map(i => i.rects[0].x));');
      const hi = await probe.read(
        'return Math.max(...D.controls().items.map(i => i.rects[0].x + i.rects[0].w));');
      let probed = 0;
      let mismatch = 0;
      let nonmono = 0;
      let prev = -1;
      const covered = new Set();
      const bad = [];
      // 关键：CDP 把鼠标坐标按整型 CSS 像素投递，而且是截断不是四舍五入
      //   （实测 465.85 落到 465）—— 这一点必须与模型一致：用 round 建模时，
      //   计划点 505.85 → 506 → 140.00 磅，而实际投递 505 → 139.33 磅，
      //   差一个像素，簇边界上正好翻一格（症状：命中说 6、实际落 5，而且
      //   偏移是逐 1 磅前进的，看着像引擎在边界上算错）。
      //   所以命中要用"实际投递的那个磅值"去问，x 与 y 都要折算。
      const quantizedPt = (xPt: any) => {
        const cx = Math.trunc(g.left + xPt * z * g.sx);
        return (cx - g.left) / z;
      };
      // y 也要同样折算。上面那条注释只提了 x，但 CDP 把 clientY 一并取整 ——
      //   1 磅 = 1.5 像素 ⇒ y 也能差半个磅。只对 x 建模时，命中的是"计划点"，
      //   而点击实际落在"投递点"，簇边界上两者会差一格（症状：横扫里几个点
      //   命中说 6、实际落 5，而偏移是逐 1 磅前进的，看着像引擎在边界上算错）。
      const quantizedY = () => {
        const cy = Math.trunc(g.top + y * z * g.sy);
        return (cy - g.top) / z;
      };
      const yq = quantizedY();
      for (let x = lo - 4; x <= hi + 4; x += 1) {
        const pt = quantizedPt(x);
        const h = await probe.read(`return await D.hit(${pt.toFixed(3)}, ${yq.toFixed(3)});`);
        if (!h || h.offset > 17) continue;
        // 先断一次连击：横扫是 x 逐 1 磅前进的，相邻两下只差一个像素、又落在 520 ms
        //   之内 —— 那在后端看来就是双击（Word 同一条：连击要"同一处"才算），于是某几
        //   个点会被当成"选这个词"，落点跑到词尾，看起来像命中与落点对不上。
        //   真人的两次单击不会落在同一个像素上，所以这里显式地在正文左边距按一下，
        //   把连击计数打断，再按待测的那一点 —— 判的仍然是"这一下命中的是什么、
        //   落点是不是它"。
        await clickPt(g, lo - 25, y, z);
        await clickPt(g, x, y, z);
        const st = await caretOf();
        ++probed;
        // 归属只在命中确实给出了"某一枚括号的主"时才比：命中返回 -1 表示没点在括号上，
        //   那时面板上的归属是"这个偏移该往哪写"（末尾算在区间内），口径不同不是矛盾。
        const ownerOk = h.field < 0 || st.f === h.field;
        if (st.o !== h.offset || !ownerOk) {
          ++mismatch;
          if (bad.length < 3) {
            bad.push(`x=${x.toFixed(1)} 命中(${h.offset},${h.field}) 实际(${st.o},${st.f})`);
          }
        }
        if (st.o < prev) ++nonmono;
        prev = st.o;
        if (st.o >= 5 && st.o <= 11) covered.add(st.o);
      }
      state.sweep = { probed, mismatch, nonmono, covered: [...covered] };
      check('★★ 真实鼠标横扫：命中偏移与归属都等于点击后的实际落点',
                probed > 50 && mismatch === 0,
                `采样 ${probed} 点，不一致 ${mismatch} 个 ${bad.join(' ')}`);
      check('★ 横扫时落点单调不减（没有回跳的死区）', nonmono === 0, `非单调 ${nonmono} 处`);
      check('★ 三个字段里每一个光标位置都能点出来（5..11 全覆盖）',
                covered.size === 7,
                `覆盖 ${[...covered].sort((a: any, b: any) => a - b).join(' ')}`);

      // 光标画在哪里：逐字点"这个字的中点"，画出来的光标必须就在点击点附近。
      //   这是整条坐标链（CSS 像素 → 帧像素 → 帧磅）的可证伪判据：dpr 或 zoom
      //   少乘一次，落点就会差一整格。
      //
      // 中点量出来的是帧像素（caret.x / caret.h 乘过 zoom），clickPt 收的是帧磅，
      //   所以要先除以 z 再交出去；少这一步只在 zoom 为 1 时看不出（恒等）。
      // "偏得远不远"的尺子要用实测字格宽，不能写死像素值：点的是字格中点，画出
      //   来的光标只能落在这一格的两端之一，上限就是半个字宽（跨到隔壁字才叫偏）。
      //   写死的像素值只在某个特定缩放下等于半个字宽，缩放一变就不是那个意思了。
      let drawnProbed = 0;
      let drawnBad = 0;
      const drawnWorst = [];
      for (let ci = 0; ci < 3; ++ci) {
        const a = await probe.read(`return D.controls().items[${ci}].start[1];`);
        const b = await probe.read(`return D.controls().items[${ci}].end[1];`);
        for (let k = a; k < b; ++k) {
          const m = await probe.read(`
            await D.setCaretAt(0, ${k}, ${ci});
            const c1 = CARET();
            await D.setCaretAt(0, ${k + 1}, ${ci});
            const c2 = CARET();
            return { x: (c1.x + c2.x) / 2, y: c2.y + c2.h / 2, w: Math.abs(c2.x - c1.x) };`);
          await clickPt(g, m.x / z, m.y / z, z);
          const st = await probe.read('return CARET();');
          ++drawnProbed;
          const off = Math.abs(st.x - m.x);
          if (off > m.w / 2 + 2 || st.offset < a || st.offset > b) {
            ++drawnBad;
            if (drawnWorst.length < 3) {
              drawnWorst.push(`字段${ci} 偏移${k}: 偏 ${off.toFixed(1)}px` +
                              `（半个字宽 ${(m.w / 2).toFixed(1)}）落 ${st.offset}`);
            }
          }
        }
      }
      check('★ 逐字点选：落点在该字范围内、且光标画在点击处附近（整条坐标链）',
                drawnProbed === 6 && drawnBad === 0,
                `${drawnProbed} 个字，偏的 ${drawnBad} 个 ${drawnWorst.join(' ')}`);

      const sc = await probe.read('return await D.selfCheck();');
      check('横扫之后屏幕 == 引擎帧', sc.screenVsEngineDiffPx === 0,
                `差 ${sc.screenVsEngineDiffPx} px`);
      await probe.read('await D.present(); return true;');
      await sleep(150);
      await probe.shot('boundary-three-adjacent');
    }

    log.section('[14.7] 括号外侧那一段也点得进去：开括号之前 / 闭括号之后（variant 3）');
    {
      // 同一个偏移在控件边界上对应两个位置（flow.h：控件之前与进入控件），而括号占位簇
      //   的势力范围按左右半边分。判据落在可证伪的数字上：点左半边，光标必须画在括号【之外】。
      await load(3);
      const y = await probe.read(`
        const q = D.controls().items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s6 = await slotsOf(6);
      const s10 = await slotsOf(10);

      await clickPt(g, s6[0].x + 0.5, y, z);  // 中层开括号那一格的左半边
      const a = await caretOf();
      const ax = await probe.read('return CARET().x;');
      check('★★ 点开括号的左半边 → 光标落在括号之前（不是被贴回括号之内）',
                a.o === 6 && ax / z < s6[0].x + 1.0,
                `偏移 ${a.o} ｜ 光标 x=${(ax / z).toFixed(2)} ｜ 括号左缘 ${s6[0].x.toFixed(2)}`);

      const afterPt = await innerCaretPt(10, -2);  // 闭括号之后那个插入点（＝天 的左缘）
      // 落在它左边半磅：那里既是闭括号那一格的右半边（"括号之后"），也紧贴天 的左半边。
      await clickPt(g, afterPt - 0.5, y, z);
      const b = await caretOf();
      const bx = await probe.read('return CARET().x;');
      check('★★ 点闭括号的右半边 → 光标落在括号之后（字与闭括号之间）',
                b.o === 10 && bx / z > s10[0].x + 1.0,
                `偏移 ${b.o} ｜ 光标 x=${(bx / z).toFixed(2)} ｜ 括号左缘 ${s10[0].x.toFixed(2)}`);
      const sc7 = await probe.read('return await D.selfCheck();');
      check('点括号外侧之后屏幕 == 引擎帧', sc7.screenVsEngineDiffPx === 0,
                `差 ${sc7.screenVsEngineDiffPx} px`);
    }

    log.section('[14.8] 方向键在括号边界上一格一格走（嵌套样例：反字前 / 天字前）');
    {
      // 报过的两条现象：光标在「反」前面按左键会一步跳到「冒号」前；在「天」前面按左键
      //   会一步跳到「悸」后面 —— 都是"整段跳过括号那一格"。
      // 判据是像素上的位移：一格 = 一枚括号的宽度（约 5.5pt），跳一整段 = 一个字 + 括号。
      await load(3);
      await probe.read('return await D.stopBlink();');
      const z = await zoomNow();

      // 先看开括号那一侧：外层 [ 之后（＝反字前面）→ 按一下左键，只该退到 [ 之前
      await probe.read('return await H.setCaret({ para: 0, offset: 5 }, { side: (1) > 0 ? "after" : "before" });');
      const x1 = (await probe.read('return CARET().x;')) / z;
      await probe.read(key('ArrowLeft'));
      const a1 = await caretOf();
      const x0 = (await probe.read('return CARET().x;')) / z;
      check('★★ 反字前按左键 → 停在字段内容起点（不退出括号、也不跳到冒号前）',
                a1.o === 5 && a1.s === 1 && Math.abs(x1 - x0) < 0.5,
                `偏移 ${a1.o} 侧别 ${a1.s} ｜ 光标 x ${x1.toFixed(2)} → ${x0.toFixed(2)}` +
                  `（退 ${(x1 - x0).toFixed(2)} pt）`);

      // 闭括号那一侧同样：中层 ] 之后（＝天字前面）→ 按一下左键，只该退到 ] 之前
      await probe.read('return await H.setCaret({ para: 0, offset: 10 }, { side: (1) > 0 ? "after" : "before" });');
      const y1 = (await probe.read('return CARET().x;')) / z;
      await probe.read(key('ArrowLeft'));
      const a2 = await caretOf();
      const y0 = (await probe.read('return CARET().x;')) / z;
      check('★★ 天字前按左键 → 只退一格到闭括号之前（不是一步跳到悸字后面）',
                a2.o === 10 && a2.s === 0 && (y1 - y0) > 2 && (y1 - y0) < 12,
                `偏移 ${a2.o} 侧别 ${a2.s} ｜ 光标 x ${y1.toFixed(2)} → ${y0.toFixed(2)}` +
                  `（退 ${(y1 - y0).toFixed(2)} pt）`);

      // 再按一下才跨过「三」那一格（一次一格，不跳）
      await probe.read(key('ArrowLeft'));
      const a3 = await caretOf();
      check('★ 再按一下才退到「三」之前（一次一格，不跳格）', a3.o === 9,
                `偏移 ${a3.o} 侧别 ${a3.s}`);

      // 反向：从「三」前按右键，依次是 三 之前 → 三 之后（＝] 之前）→ ] 之后
      const seq = [];
      for (let i = 0; i < 3; ++i) {
        await probe.read(key('ArrowRight'));
        const r = await caretOf();
        seq.push(`${r.o}/${r.s}`);
      }
      check('★ 反向同样一格一格（9/2 → 10/0 10/1 11/0）',
                JSON.stringify(seq) === JSON.stringify(['10/0', '10/1', '11/0']),
                `落点 ${seq.join(' ')}`);
      await probe.shot('arrow-boundary-nested');
    }

    log.section('[14.9] 删除之后的落点：一次只退一个字，不跨括号（嵌套 / 三连）');
    {
      // 两条现象：嵌套删「天」之后光标落到 ]中 之前（该在它之后，＝被删那个字所在的位置）；
      //   三连删「悸」之后光标跑进第三个控件（该停在第一个字段末尾）。
      // 判据落在 (偏移, 侧别) 上 —— 侧别就是"这个偏移上跨过了几枚括号"，是落点的完整身份；
      //   再加一条像素证据：光标要落在 ]中 之后，不是它之前。
      await load(3);
      await probe.read('return await H.setCaret({ para: 0, offset: 11 });');
      await probe.read(key('Backspace'));
      const a = await caretOf();
      const s10 = await slotsOf(10);
      const z9 = await zoomNow();
      const ax = (await probe.read('return CARET().x;')) / z9;
      check('★★ 嵌套删「天」→ 落点＝「天」原来的位置（cp 10 跨过 ]中，侧别 1）',
                a.o === 10 && a.s === 1 && s10.length === 2 && ax > s10[0].x + 1,
                `落点 ${a.o}/${a.s} ｜ 光标 x=${ax.toFixed(2)} ｜ cp10 两枚 ] 的 x=` +
                  `${s10.map((q: any) => q.x.toFixed(2)).join(' / ')}`);

      await load(2);
      // 删控件是结构操作：表单模式默认被 structureLock 锁住（[16](c) 是它的对照），
      //   这一段要验"空控件被删掉"，所以切文档模式。
      await probe.read('return await SET_MODE(0);');
      await probe.read('return await H.setCaret({ para: 0, offset: 11 });');
      for (let i = 0; i < 3; ++i) {
        await probe.read(key('Backspace'));
        await sleep(20);
      }
      const b0 = await caretOf();
      const it0 = await items();
      check('★★ 三连：第三个字段删空之后再退一格，删掉的是这个空控件（落点＝它原来那一格）',
                b0.o === 9 && b0.s === 1 && JSON.stringify(it0) === '[[5,7],[7,9]]',
                `落点 ${b0.o}/${b0.s} ｜ ${JSON.stringify(it0)}`);
      await probe.read(key('Backspace'));
      await sleep(20);
      const b = await caretOf();
      check('★★ 再退一格删「悸」→ 落点＝第一个字段末尾（cp 8 侧别 0），不许进别的控件',
                b.o === 8 && b.s === 0, `落点 ${b.o}/${b.s}`);
      await probe.read(key('痛'));
      const it = await items();
      check('★ 接着打字进的是第二个字段（不是别的控件）',
                JSON.stringify(it) === '[[5,7],[7,9]]', JSON.stringify(it));
      await probe.shot('delete-caret-landing');
    }

    log.section('[14.10] 在字段末尾回车（字段因此跨段）之后再退格：整段与控件都不许丢');
    {
      // 现象：三连可编辑字段（variant 2）里把光标放到「天」后面回车，再把光标移到
      //   「悸」后面退格 —— 结果「患者主诉：反复心悸」九个字与三个控件整个消失，只剩一行
      //   「三天」。根因不在删除：回车把段落标记插进了控件内部（字段因此跨段，这是正常形态，
      //   见 compile.h 的 BlockSource），而"只重编译被改的那一段"是按标记的父容器取子节点
      //   区间的 —— 控件里的半截被当成整段编译，前半段的字与控件边界一起丢掉，block_src_ /
      //   block_flow_ 也被覆盖，控件集合随之空掉。
      // 判据三层：文本（只少了「悸」）、控件区间（三条都还在，跨段那条的终点落在第二段）、
      //   以及屏幕 == 引擎帧 —— 半截产物会当场把括号画错。
      await load(2);
      await probe.read('return await H.setCaret({ para: 0, offset: 11 });');
      await probe.read(key('Enter'));
      await sleep(30);
      const p1 = Array.from(await probe.read('return H.text();')).join('');
      check('回车在字段内容里分段：两段的文本都对（字段因此跨段）',
                p1 === '患者主诉：反复心悸三天\n，加重一天。', JSON.stringify(p1));
      await probe.read('return await H.setCaret({ para: 0, offset: 9 });');
      await probe.read(key('Backspace'));
      await sleep(30);
      const p2 = Array.from(await probe.read('return H.text();')).join('');
      check('★★ 退格只删掉「悸」一个字（曾经把前面九个字与所有控件整批抹掉）',
                p2 === '患者主诉：反复心三天\n，加重一天。', JSON.stringify(p2));
      const it2 = await probe.read(`
        return JSON.stringify(D.controls().items
          .map(i => [i.start[0], i.start[1], i.end[0], i.end[1]]));`);
      check('★★ 三个控件都还在，且跨段那条的终点落在第二段第 0 位',
                it2 === '[[0,5,0,7],[0,7,0,8],[0,8,1,0]]', it2);
      const au = await probe.read('return await D.auditCompile();');
      check('★ 产物与整树全量编译逐字段一致（diff 0）', !!au && au.diff === 0,
                au ? `diff ${au.diff}` : 'null');
      const sc = await probe.read('return await D.selfCheck();');
      check('★ 屏幕 == 引擎帧（半截产物会把括号画丢/画错）',
                sc.screenVsEngineDiffPx === 0, `差 ${sc.screenVsEngineDiffPx} px`);
      await probe.shot('ctl-split-then-backspace');
    }

    log.section('[14.11] 两次回车都落进字段内部之后再退格：段首那两枚边界不许丢');
    {
      // 现象：三连字段里「天」后回车、再「悸」后回车，然后把光标放到「天」后面退格 ——
      //   第二段段首的两枚边界（第一个字段的闭括号、第二个字段的开括号）成对消失，
      //   控件从 3 个掉到 1 个。根因见 edit_test [32]：段首第一枚内容是闭括号时，编译层
      //   没把它记进这段的来源身份，而增量重编译的形状判据正是靠那个身份判的。
      // 判据三层：文本与三条区间、产物与整树全量一致、屏幕 == 引擎帧。
      await load(2);
      await probe.read('return await H.setCaret({ para: 0, offset: 11 });');
      await probe.read(key('Enter'));
      await sleep(30);
      await probe.read('return await H.setCaret({ para: 0, offset: 9 });');
      await probe.read(key('Enter'));
      await sleep(30);
      await probe.read('return await H.setCaret({ para: 1, offset: 2 });');
      await probe.read(key('Backspace'));
      await sleep(30);
      const p3 = Array.from(await probe.read('return H.text();')).join('');
      check('★★ 退格只删掉「天」一个字（曾经把第二段的控件边界整对抹掉）',
                p3 === '患者主诉：反复心悸\n三\n，加重一天。', JSON.stringify(p3));
      const it3 = await probe.read(`
        return JSON.stringify(D.controls().items
          .map(i => [i.start[0], i.start[1], i.end[0], i.end[1]]));`);
      check('★★ 三个控件都还在，后两条各自跨到下一段段首',
                it3 === '[[0,5,0,7],[0,7,1,0],[1,0,2,0]]', it3);
      const au3 = await probe.read('return await D.auditCompile();');
      check('★ 产物与整树全量编译逐字段一致（diff 0）', !!au3 && au3.diff === 0,
                au3 ? `diff ${au3.diff}` : 'null');
      const sc3 = await probe.read('return await D.selfCheck();');
      check('★ 屏幕 == 引擎帧', sc3.screenVsEngineDiffPx === 0,
                `差 ${sc3.screenVsEngineDiffPx} px`);
      await probe.shot('ctl-split-twice-then-backspace');
    }
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
