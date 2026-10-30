// 编辑面（一）：基准、键盘路径、命中往返、码点与字素簇、输入法、屏幕与引擎帧一致。
//
// 这一份是编辑语义的地基：后面几份都假定"敲字真的改了文档、光标真的跟着走"。

import { check } from '../support/check';
import { test } from '../support/probe';
import {
  openEdit, finishEdit, resetAndGetFullBaseline, key, sleep, CP_LEN, S6, ZOOM,
} from '../support/edit-helpers';

test.describe('编辑面：基准与键盘', () => {
  test('全量基准、键盘、命中、码点、输入法、屏幕与引擎帧都按契约工作', async ({ open }) => {
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

  log.section('[1] 全量基准：先量出"整页重画"到底是多少像素');
  {
    const st = await resetAndGetFullBaseline(probe);
    state.baseline = st;
    check('切到可编辑场景后确实重排过', st.layoutRan === true);
    // 场景 6 是单段文档：脏段就是第 0 段，前面没有可复用的前缀，
    //   所以这里必然是全量 —— 多段文档的增量见 [9]。不标 ok/warn，
    //   是因为"全量"在这里是唯一可能的结果，不是缺陷。
    check('单段文档的首次排版是全量（没有前缀可复用）', st.layoutWasFull === true);
    check('重画页数 = 总页数', st.repaintedPages === st.totalPages,
              `${st.repaintedPages} / ${st.totalPages}`);
    // 参照是窗口内那几页，不是整个帧：帧按页号分槽之后，帧高 = 槽数 x 步距，
    //   而槽数可以多于窗口页数（多出来的是空槽）—— 拿 framePixels 当分母，判据会
    //   随槽数一起松掉。
    const pageH0 = await probe.read('return D.pagePx().h;');
    const winPx = st.windowPages * st.frameW * pageH0;
    check('整页重画写入 ≥ 半窗（窗口内几页，不是整个帧）',
              st.repaintedPixels > winPx / 2,
              `${st.repaintedPixels} / ${winPx}`);
    check('初始文档与预期一致', (await probe.read(CP_LEN + 'return cplen(H.paragraph(0));'))
              === Array.from(S6).length);
  }

  // 整页重画的分母，[2] 与 [2b] 的像素判据都拿它作参照。
  const full = state.baseline.repaintedPixels;
  log.section('[2] 键盘路径：派真实 keydown，文档要变、光标要走（单段文档全量重排、只重画一行带）');
  {
    const b = await probe.read(CP_LEN + 'return { n: cplen(H.paragraph(0)), o: CARET().offset };');
    await probe.read(key('心'));
    const a = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)), o: CARET().offset,
                                              st: D.editStats() };`);
    check('文档码点数 +1', a.n === b.n + 1, `${b.n} → ${a.n}`);
    check('光标停在新字之后（不是字前、也不是跳段首）', a.o === b.o + 1, `${b.o} → ${a.o}`);
    check('插入的就是那个字', (await probe.read('return H.paragraph(0).includes("心");')) === true);
    check(' 单段文档：敲字仍是全量重排（脏段就是第一段）',
              a.st.layoutRan === true && a.st.layoutWasFull === true);
    // 重排是全量，重画不是：行带只覆盖这一行的墨迹（页顶那段空白不属于任何行）。
    check(' 单段文档：敲字只重画那一行带，不是整页',
              a.st.repaintedPixels > 0 && a.st.repaintedPixels < full / 10,
              `${a.st.repaintedPixels} vs 全量 ${full}`);

    // 改键不该落字：带 ctrl/alt/meta 一律忽略
    //
    // 顺便钉住"该吞的吞、不该吞的不吞"：
    //   dispatchEvent 返回 false 表示监听器调了 preventDefault。
    //   普通可打印字符 → 必须吞掉（否则字符会落进 textarea，和 canvas 上的正文叠成两层）
    //   Ctrl/Alt/Meta 组合 → 必须不吞（否则用户按 Ctrl+C 就被吃掉了）
    //   例外只有 Ctrl+Z / Ctrl+Y 这一对（撤销键位，见 [16] (g)）—— 这里用 'x'
    //     探的就是"修饰键本身不构成吞掉的理由"，不是"任何一个组合都不吞"。
    //   只断言"文档没变"是测不出后者的 —— 合成事件本来就没有默认行为。
    //
    // 顺序也要小心：下面"该吞的吞"那一条会真的敲进一个 z。
    //   探针自己制造的状态变化必须显式收掉，否则后面每一条断言都被带偏
    //   （否则多出来的 z 会让"退格恢复原样"跟着失败）。
    const beforeMod = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');

    let modSwallowed = 0;
    for (const m of ['ctrlKey', 'altKey', 'metaKey']) {
      const r = await probe.read(`
        const e = new KeyboardEvent('keydown',
          { key: 'x', ${m}: true, bubbles: true, cancelable: true });
        return IME().dispatchEvent(e);`);
      if (r === false) ++modSwallowed;
    }
    check('Ctrl/Alt/Meta 组合不被吞（否则 Ctrl+C 之类会被吃掉）', modSwallowed === 0,
              `被吞 ${modSwallowed} / 3`);
    const afterMod = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    check('带 Ctrl/Alt/Meta 的按键不落字', afterMod === beforeMod,
              `${beforeMod} → ${afterMod}`);

    const swallow = await probe.read(`
      const e = new KeyboardEvent('keydown', { key: 'z', bubbles: true, cancelable: true });
      return IME().dispatchEvent(e);   // false = 被 preventDefault
    `);
    check('普通可打印字符的 keydown 被宿主吞掉（不落进 textarea）', swallow === false);
    const afterSwallow = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    check(' 但字确实进了文档一次（"吞掉 keydown" 和 "不落字" 是两回事）',
              afterSwallow === beforeMod + 1, `${beforeMod} → ${afterSwallow}`);

    // 收尾：退格两次，把 z 和前面那个 心 都收掉
    await probe.read(key('Backspace'));
    await probe.read(key('Backspace'));
    const back = await probe.read(CP_LEN + 'return { n: cplen(H.paragraph(0)), o: CARET().offset };');
    check('两次退格把文档恢复原样', back.n === b.n && back.o === b.o,
              `${back.n} / offset ${back.o}（期望 ${b.n} / ${b.o}）`);
  }

  log.section('[3] 方向键 = 真增量：不重排、0 脏页、写入量差两个数量级');
  {
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const beforeMove = await probe.read('return D.editStats();');
    await probe.read(key('ArrowRight'));
    const right = await probe.read('return { c: CARET(), st: D.editStats() };');
    check('右移后光标偏移 +1', right.c.offset === 1, `offset=${right.c.offset}`);
    check(' 光标移动不重排（layoutRan=false）', right.st.layoutRan === false);
    check(' 光标移动不抹掉排版账（仍是上一轮的重排行数）',
              right.st.relaidLines === beforeMove.relaidLines,
              `${right.st.relaidLines} / 上一轮 ${beforeMove.relaidLines}`);
    check(' 光标移动不抹掉布局耗时', right.st.layoutMs === beforeMove.layoutMs);
    check(' 光标移动 0 个脏页（底图一个字都没变）', right.st.dirtyPageCount === 0);
    check(' 光标移动写入量 < 全量的 1/100',
              right.st.repaintedPixels * 100 < full,
              `${right.st.repaintedPixels} vs ${full}（${(100 * right.st.repaintedPixels / full).toFixed(4)}%）`);

    // 上屏面积：引擎在 present 里算出的脏矩形，必须真的落到宿主的 putImageData 上。
    //   这一条挡的是"引擎给了矩形、宿主还是整页贴"——那种情况下像素判据全都照样绿，
    //   只有这个数看得出来。整页约 1.1 Mpx，一次光标移动是几十像素。
    const blit3 = await probe.read('return D.blitStats();');
    check('★ 光标移动的上屏面积是几十像素，不是整页（宿主按脏矩形贴）',
              typeof blit3.px === 'number' && blit3.px > 0 && blit3.px < 2000,
              `px=${blit3.px}`);

    // 段首再左移：引擎应当返回 false（没动就不该重绘）
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const at0 = await probe.read('return await MOVE(-1);');
    check('已在段首，向左移动不重绘（返回 null）', at0 === null);

    // Home / End
    await probe.read(key('End'));
    const atEnd = await probe.read('return CARET().offset;');
    const len = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    check('End 走到段末', atEnd === len, `${atEnd} / 段长 ${len}`);
    await probe.read(key('Home'));
    check('Home 回到段首', (await probe.read('return CARET().offset;')) === 0);
  }

  log.section('[4] 画布点选 = 命中测试往返：正查找出的光标 x，反查必须回到同一个偏移');
  {
    const len = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const g0 = await probe.read('return CARET();');
    const z = await probe.read('return ZOOM();');

    // 画出来的光标矩形（的东西）应当逐个右移
    const xs = [];
    for (let o = 0; o <= len; ++o) {
      await probe.read(`await H.setCaret({ para: 0, offset: ${o} }); return true;`);
      xs.push(await probe.read('return CARET().x;'));
    }
    let mono = true;
    for (let i = 1; i < xs.length; ++i) if (xs[i] <= xs[i - 1]) mono = false;
    check(`画出的 ${xs.length} 个光标矩形 x 严格递增`, mono,
              `x ∈ [${xs[0]}, ${xs[xs.length - 1]}]`);

    // 从引擎自己报出的几何里取插入点的真实位置。
    //
    //   不要用 caret.x 去算 —— 那是光标矩形的左边缘，不是插入点：
    //   光标以插入点为中心，左边还差半个光标宽。实测差 0.7pt：
    //       行内起点=56.700 pt      光标矩形(页面磅) x=56.000
    //   拿 caret.x 的中点当"势力范围边界"，会让 18 对全错：真正的边界比它靠右
    //   0.7pt，±1px 的探针根本跨不过去。
    //
    //   控件报告里 行内起点 + 本偏移的沿轴位置 才是插入点在帧磅坐标下的位置。
    const repOf = async (o: any) => {
      await probe.read(`await H.setCaret({ para: 0, offset: ${o} }); return true;`);
      return probe.read('return D.caretReport();');
    };
    const rep0 = await repOf(0);
    const mOrigin = /行内起点=([-\d.]+) pt/.exec(rep0);
    if (!mOrigin) throw new Error('caretReport 里找不到「行内起点」，格式变了？');
    const originPt = Number(mOrigin[1]);

    const insPt = [0];
    for (let o = 1; o <= len; ++o) {
      const rep = await repOf(o);
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      if (!m) throw new Error('caretReport 里找不到「本偏移的沿轴位置」，格式变了？');
      insPt.push(Number(m[1]));
    }
    const absPt = insPt.map((v) => originPt + v);   // 帧磅（单页文档，页内磅 = 帧磅）
    let m2 = true;
    for (let i = 1; i < absPt.length; ++i) if (absPt[i] <= absPt[i - 1]) m2 = false;
    check(`引擎报出的 ${absPt.length} 个插入点位置严格递增`, m2,
              `pt ∈ [${absPt[0].toFixed(2)}, ${absPt[absPt.length - 1].toFixed(2)}]`);

    // 在每个插入点处派真实 mousedown，看落点是否就是那个偏移
    let ok = 0;
    for (let o = 0; o <= len; ++o) {
      const hit = await probe.read(`
        const cv = FRAME(), r = cv.getBoundingClientRect();
        const sx = r.width / cv.width, sy = r.height / cv.height;
        const zz = ZOOM();
        // 帧磅 → 帧像素（× zoom）→ 客户端像素（× CSS 缩放）
        const clientX = r.left + (${absPt[o] + 0.2} * zz) * sx;
        const clientY = r.top + ${g0.y + g0.h / 2} * sy;
        cv.dispatchEvent(new MouseEvent('mousedown',
          { clientX, clientY, bubbles: true, cancelable: true }));
        await H.flush();  // 异步宿主：命中+落光标要走往返，读之前必须先排空
        return CARET().offset;`);
      if (hit === o) ++ok;
    }
    check(` ${len + 1} 个插入点，点画布反查全部回到原位置（画光标与点选共用同一份行盒几何）`,
              ok === len + 1, `成功 ${ok} / ${len + 1}`);

    // 中点规则：两个相邻插入点的中点两侧，应当分别落到左、右两个插入点。
    //   这就是"点 e+U+0301 的右半边，光标跑到它后面，而不是钻进它内部"那条语义 ——
    //   它必须能被证伪，而不是只在注释里写着。
    const yPt = (g0.y + g0.h / 2) / z;
    let bok = 0;
    const bad = [];
    const at = (x: number) => probe.read<{ para: number; offset: number } | null>(`
        const q = await D.hit(${x}, ${yPt});
        if (!q || !q.position || q.position.flow < 0) return null;
        return await D.textPosOf(q.position.flow);`);
    for (let o = 0; o < len; ++o) {
      const mid = (absPt[o] + absPt[o + 1]) / 2;
      // 命中给的是位置（流位置 + 行别）；要断言"落到第几个插入点"，用诊断的
      //   位置→地址换算读回来 —— 不拿命中载荷里的地址键，那套键已经不再吐了。
      const left = await at(mid - 0.2);
      const right = await at(mid + 0.2);
      if (left && right && left.offset === o && right.offset === o + 1) ++bok;
      else bad.push(`o=${o}: 左→${left ? left.offset : 'null'} 右→${right ? right.offset : 'null'}`);
    }
    check(` ${len} 对相邻插入点，中点左半→左边、右半→右边（势力范围 = 到邻居的中点）`,
              bok === len, `成立 ${bok} / ${len}${bad.length ? '；反例 ' + bad.slice(0, 3).join(' | ') : ''}`);

    // 点在画布左边界之外：命中必须失败，而不是胡乱给一个位置。
    // （注意：页内的空白不算失败 —— hitTestPt 的语义是"取同页最近的一行"，
    //   所以"点页面底部空白光标会跳到第一行"是正确行为，不该拿来断言。）
    await probe.read('await H.setCaret({ para: 0, offset: 5 }); return true;');
    const outside = await probe.read(`
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const sy = r.height / cv.height;
      // caret.y 是页内像素（页号另给），画布代理的 rect 是窗口坐标：
      //   窗口内 y = (光标页 - 窗口首页) x 步距 + 页内 y。
      const step = D.pagePx().h + D.pagePx().gap;
      const wy = (CARET().page - D.editStats().windowFirstPage) * step + CARET().y;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left - 50, clientY: r.top + (wy + 5) * sy,
          bubbles: true, cancelable: true }));
      await H.flush();  // 异步宿主：命中+落光标要走往返
      return CARET().offset;`);
    check('点画布左边界之外 → 命中失败，光标停在原地（不猜）', outside === 5,
              `offset=${outside}`);
  }

  log.section('[5] 偏移单位是码点、且吸附到字素簇（经 WASM 边界验证）');
  {
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await probe.read('return await TYPE("e");');
    await probe.read('return await TYPE("\\u0301");');   // 组合重音 U+0301
    const st = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                               o: CARET().offset };`);
    check('e 类序列：插入 2 个码点，光标偏移 = 2（偏移数的是码点）', st.o === 2, `offset=${st.o}`);

    const posOf = async (o: any) => {
      await probe.read(`await H.setCaret({ para: 0, offset: ${o} }); return true;`);
      const rep = await probe.read('return D.caretReport();');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const p0 = await posOf(0), p1 = await posOf(1), p2 = await posOf(2);
    check(' 偏移 1 不是合法插入点 → 吸附到簇首，位置与偏移 0 相同',
              Math.abs(p1 - p0) < 1e-6, `pos(0)=${p0} pos(1)=${p1}`);
    check('偏移 2 已经是下一个簇 → 位置前进', p2 > p0, `pos(2)=${p2}`);

    const cps = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    await probe.read('await H.setCaret({ para: 0, offset: 2 }); return true;');
    await probe.read('return await H.deleteBackward();');
    const after = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                                 o: CARET().offset };`);
    check(' 一次退格删掉 2 个码点（删的是簇，不是码点；否则会留下孤立的组合重音）',
              after.n === cps - 2 && after.o === 0, `${cps} → ${after.n}，offset=${after.o}`);
  }

  log.section('[5b] 连字 ff：真实按键路径下，两个 f 就是两个字符');
  {
    // 这一节守的是删除语义，只能在这一层验。
    //
    //   原生单测（edit_test.cpp 的 [8]）验的是引擎内部的簇表；而"用户按了两次 f 键"
    //   这条路上还多了一层宿主事件分发 —— 万一宿主把两次 keydown 合并、或者
    //   textarea 的 value 攒了一串才 commit，簇表再对也没用。所以这里必须
    //   派真实的 KeyboardEvent，而不是调 TYPE()。
    //
    //   为什么偏偏是 f：liga 连字表里 ff / fi / fl / ffi / ffl 全是以 f 开头的，
    //   而中文测试一个都盖不到 —— 这正是这个 bug 能活到现在的原因。
    await probe.read(`await LOAD.short(); await D.stopBlink(); return true;`);
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');

    const b = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    await probe.read(key('f'));
    await probe.read(key('f'));
    const a = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                               t: H.paragraph(0),
                                               o: CARET().offset };`);
    check('两次按 f → 文档 +2 个码点', a.n === b + 2, `${b} → ${a.n}`);
    check('两个 f 都真的写进了文本', Array.from(a.t).slice(0, 2).join('') === 'ff',
              JSON.stringify(Array.from(a.t).slice(0, 4).join('')));
    check('光标停在 2 —— 每个 f 各占一格（不是 1）', a.o === 2, `offset=${a.o}`);

    // 决定性判据：偏移 1（两个 f 之间）必须是一个合法插入点。
    //   连字一旦把 ff 合成一个簇，解析器给出的簇表里就没有偏移 1 的起点，
    //   光标会被吸附回 0 → 这两次取值相等。用 caretReport 的沿轴位置直接比。
    const posOf = async (o: any) => {
      await probe.read(`await H.setCaret({ para: 0, offset: ${o} }); return true;`);
      const rep = await probe.read('return D.caretReport();');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const q0 = await posOf(0), q1 = await posOf(1);
    check(' 偏移 1（两个 f 之间）是合法插入点，位置严格前移（连字一合并这里就相等）',
              q1 > q0 + 1e-6, `pos(0)=${q0} pos(1)=${q1}`);

    await probe.read('await H.setCaret({ para: 0, offset: 2 }); return true;');
    await probe.read(key('Backspace'));
    const d = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                               o: CARET().offset };`);
    check(' 一次退格只删掉一个 f（连字一合并这里会一下少两个）',
              d.n === a.n - 1 && d.o === 1, `${a.n} → ${d.n}，offset=${d.o}`);

    // Delete 走的是另一条分支（clusterCeil），也得守。
    // 必须先把光标移回 0：退格之后光标停在 1，而 1 处是原文第一个字，
    //   在那儿按 Delete 删掉的是"患"而不是剩余的那个 f —— 那测的就不是连字了。
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    await probe.read(key('Delete'));
    const fwd = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                                 o: CARET().offset };`);
    check(' Delete 也只删掉了那一个 f', fwd.n === d.n - 1 && fwd.o === 0,
              `${d.n} → ${fwd.n}，offset=${fwd.o}`);

    // 收尾：把文档恢复成 reset 后的原样，后面几节才不会被这一节带偏
    await probe.read(`await LOAD.short(); await D.stopBlink(); return true;`);
    const restored = await probe.read(CP_LEN + 'return cplen(H.paragraph(0));');
    check('收尾：文档已恢复（后续小节的起点干净）', restored === Array.from(S6).length,
              `${restored} / ${Array.from(S6).length}`);
  }

  log.section('[6] IME：组合期间一次都不碰引擎，只有 commit 才落文档');
  {
    await probe.read('await H.setCaret({ para: 0, offset: 0 }); return true;');
    const before = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)), t: H.paragraph(0) };`);

    await probe.read(`
      IME().dispatchEvent(
        new CompositionEvent('compositionstart', { bubbles: true }));
      return true;`);
    // 组合更新来多少次都不该动文档
    for (const d of ['n', 'ni', 'ni3']) {
      await probe.read(`
        IME().dispatchEvent(
          new CompositionEvent('compositionupdate', { bubbles: true, data: ${JSON.stringify(d)} }));
        return true;`);
    }
    const mid = await probe.read(CP_LEN + 'return { n: cplen(H.paragraph(0)), t: H.paragraph(0) };');
    check(' 组合期间（3 次 compositionupdate）文档一个字都没变',
              mid.n === before.n && mid.t === before.t, `${before.n} → ${mid.n}`);

    await probe.read(`
      IME().dispatchEvent(
        new CompositionEvent('compositionend', { bubbles: true, data: '\\u4f60' }));
      return true;`);
    const after = await probe.read(CP_LEN + `return { n: cplen(H.paragraph(0)),
                                                  t: H.paragraph(0),
                                                  o: CARET().offset };`);
    check('commit 后文档 +1 码点', after.n === before.n + 1, `${before.n} → ${after.n}`);
    check('commit 落下的就是那个字（你）', after.t.startsWith('你') && after.t.length > 0);
    check('commit 后光标停在新字之后', after.o === 1, `offset=${after.o}`);
    check('状态栏已退出"组合中"', (await probe.read('return document.getElementById("status").textContent;'))
              .indexOf('组合中') < 0);
  }

  log.section('[2b] 连打一长串字符：屏幕必须等于引擎那一帧（含闪烁在跑）');
  {
    // 这条断言的存在理由（真机现象）：
    //   "输入时光标往后走，但插入处的文字没有往后移，两个字符叠在一起。"
    //   这类症状只可能出在两处：引擎画错，或者上屏那一小步贴错。
    //   所以下面同时钉两根桩：
    //   屏幕（canvas 的真实像素）必须逐字节等于引擎当前那一帧 —— 抓"上屏贴错"。
    //   一个一个真按键敲出来的那一帧，必须逐字节等于同样文档一次性插入后的那一帧
    //   —— 抓"增量重绘与全量重绘不一致"。
    //
    //   两个刻意的设计，不许省：
    //     闪烁保持开着，并按 ~110ms 的间隔按键 —— 真人是这个节奏，
    //       而闪烁每 530ms 会自己 present 一次。关掉闪烁去测，等于把这条
    //       最容易出问题的交错整个绕过了。
    //     两次采样之间不能有 await 之外的东西：canvas 和引擎帧必须在
    //       同一时刻读，否则光标相位不同，差异里混着闪烁。
    const RUN = 'ssssss' + 'd'.repeat(15) + 'f'.repeat(14) + 'g'.repeat(25);
    const AT = 8;   // 「患者主诉：反复心」之后（8 个码点）

    // 同刻读 canvas 与引擎帧并逐字节比较。
    //   帧切成一页一张画布之后，这件事交给 diag.selfCheck()：它按页读、按页比，
    //   口径与"整篇一张画布"时相同 —— 屏幕上每一页的像素都必须等于引擎那一帧，
    //   页与页之间的间隙不比（宿主没有它的画布，它只是引擎帧上的背景色）。
    const screenVsFrame = () => probe.read(`
      const sc = await D.selfCheck();
      const b = sc.screenVsEngineDiffBox || [0, 0, 0, 0];
      return { diff: sc.screenVsEngineDiffPx, w: sc.framePx[0], h: sc.framePx[1],
               minx: b[0], miny: b[1], maxx: b[2], maxy: b[3] };
    `);

    const resetTo = async () => probe.read(`
      await LOAD.short();
      await H.setCaret({ para: 0, offset: ${AT} });
      return true;`);

    // A：逐个真按键，闪烁开着，按 ~110ms 的真人节奏
    await resetTo();
    const perKey = [];
    const drift = [];
    for (let i = 0; i < RUN.length; ++i) {
      await probe.read(key(RUN[i]));
      perKey.push(await probe.read('return D.editStats();'));
      // 每一次按键都查，不是抽样。
      //   "光标走了、文字没动"这种症状只在某一次贴错；抽样会把漏掉。
      drift.push(await screenVsFrame());
      await sleep(110);
    }
    const worstDrift = drift.reduce((m, d) => Math.max(m, d.diff), 0);
    const worstBox = drift.find((d) => d.diff === worstDrift) || {};
    const textA = await probe.read('return H.paragraph(0);');
    // 定格成确定的相位再取 A 帧，否则和 B 帧的差异里混着闪烁
    await probe.read(`await D.stopBlink(); await D.present(); return true;`);
    await probe.read('window.__frameA = (await PIX()).data; return true;');
    await probe.shot('typing-state');

    // B：同样的文档，一次性插入，同样定格
    await resetTo();
    await probe.read(`return await TYPE(${JSON.stringify(RUN)});`);
    const textB = await probe.read('return H.paragraph(0);');
    await probe.read(`await D.stopBlink(); await D.present(); return true;`);

    const d = await probe.read(`
      const A = window.__frameA;
      const F = await PIX();
      const B = F.data;
      const w = F.w;
      const n = Math.min(A.length, B.length);
      let diff = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
      for (let i = 0; i + 3 < n; i += 4) {
        if (A[i] !== B[i] || A[i+1] !== B[i+1] || A[i+2] !== B[i+2]) {
          ++diff;
          const p = i >> 2, x = p % w, y = (p - x) / w;
          if (x < minx) minx = x; if (x > maxx) maxx = x;
          if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
      }
      return { diff, w, h: F.h, minx, miny, maxx, maxy,
               lenA: A.length, lenB: B.length };
    `);

    const full = state.baseline.repaintedPixels;
    const minPx = perKey.reduce((m, s) => Math.min(m, s.repaintedPixels), Infinity);
    check('文档内容一致（逐个敲 vs 一次性插入）',
              textA === textB && Array.from(textA).length > AT,
              `${Array.from(textA).length} vs ${Array.from(textB).length} 码点`);
    check(' 屏幕像素 == 引擎帧（闪烁在跑、110ms 节奏连打，每一次按键都查）',
              worstDrift === 0,
              worstDrift === 0 ? '全部相同'
                : `最大差 ${worstDrift} 像素，包围盒 x∈[${worstBox.minx},${worstBox.maxx}]` +
                  ` y∈[${worstBox.miny},${worstBox.maxy}]（帧 ${worstBox.w}×${worstBox.h}）`);
    check(' 帧逐字节相同：增量重绘的结果 == 重新排一遍的结果',
              d.diff === 0 && d.lenA === d.lenB,
              d.diff === 0
                ? '完全相同'
                : `差 ${d.diff} 像素，包围盒 x∈[${d.minx},${d.maxx}] y∈[${d.miny},${d.maxy}]` +
                  `（帧 ${d.w}×${d.h}）`);
    // 这一条一度断言"每一次按键都整页重画"，用来证明上面那条"增量 == 从零"不是
    //   "两边错得一样"。行带重画之后它不再成立：改一行只重画那一条带。
    //   "不是空话"这件事改由上面两条更强的保证 —— 屏幕 == 引擎帧、增量帧 == 从零重画的帧，
    //   两条都是逐像素比。这里只留一个上界：写入量不许超过整页（带 + 光标 + 标记）。
    const maxPx = perKey.reduce((m, s) => Math.max(m, s.repaintedPixels), 0);
    check(' 每一次按键的写入量都不超过整页（行带重画的上界）', maxPx <= full * 1.05,
              `最大那次 ${maxPx} vs 整页 ${full}`);

    // 把给用户用的取证接口也当场跑一遍 —— 它要是抛异常，用户那边就白跑一趟。
    const sc = await probe.read('return await D.selfCheck();');
    check(' 自检接口可用且当场判为一致（diag.selfCheck()）',
              !!sc && sc.screenVsEngineDiffPx === 0 && Array.isArray(sc.framePx),
              sc ? `屏幕与引擎帧差 ${sc.screenVsEngineDiffPx} px，帧 ${sc.framePx.join('×')}，` +
                   `dpr=${sc.dpr}，窗口 第${sc.window.first}页起 ${sc.window.count} 页` +
                   `（比了 ${sc.window.comparedPages} 页 / 页画布 ${sc.pageCanvases} 张），` +
                   `页 css [${sc.pages.cssSize}] / rect [${sc.pages.rectPx}]`
                 : '返回 null');
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
