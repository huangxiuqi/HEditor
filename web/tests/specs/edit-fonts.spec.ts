// 编辑面（六）：字体按需读的账。
//
// 块大小、命中率、常驻字节、每帧 IO：量的是"字体字节什么时候进存储、进多少"，
// 判据落在账上，不落在耗时上。

import { check } from '../support/check';
import { test } from '../support/probe';
import { openEdit, finishEdit } from '../support/edit-helpers';

test.describe('编辑面：字体按需读', () => {
  test('命中率、块大小与真病历文本的缓存账都按契约工作', async ({ open }) => {
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

  log.section('[17] 字体按需读：命中率 / 每页落存储 / 每帧 IO');
  {
    // 整份装载时这六个数应当全是零 —— 没有数据源，也就没有按需读的账。
    //   这一条同时是"按需这条路没被误开"的判据。
    const full = await probe.read('return D.editStats();');
    check('整份装载时按需的账全为零（没有数据源）',
              Number(full.fontStorageReads) === 0 && Number(full.fontResidentBytes) === 0 &&
                Number(full.fontCacheHits) === 0,
              `reads=${full.fontStorageReads} resident=${full.fontResidentBytes}`);

    const switched = await probe.read('return await D.reloadFonts("lazy");');
    check('能切到按需装载（字体写进 MEMFS，引擎按路径装）',
              !!(switched && switched.mode === 'lazy'), JSON.stringify(switched && switched.mode));

    // 三个数要在真实排版上量：装 2000 段并把它排一遍。
    //   用 diverse 那一个入口 —— stress 文档整篇是同一个字，字形集合只有一个，
    //   字形缓存与块缓存都会命中到失真（量出来的数完全不反映真文档）。
    await probe.read('await LOAD.stressDiverse(2000, 20); return true;');
    await probe.read('return await D.present();');
    // 再换几个窗口位置。只量"当前窗口那 3 页"会低估浏览成本 —— 换到新区域
    //   要读它自己的字形。这里直接设视口锚点（比模拟滚动稳），前中后各一次。
    for (const first of [0, 25, 50]) {
      await probe.read(`return await D.setViewportAnchor(${first}, 3);`);
    }
    const st = await probe.read('return D.editStats();');

    const hits = Number(st.fontCacheHits);
    const misses = Number(st.fontCacheMisses);
    const reads = Number(st.fontStorageReads);
    const ioBytes = Number(st.fontStorageBytes);
    const ioMs = Number(st.fontIoMs);
    const resident = Number(st.fontResidentBytes);
    const glyphs = Number(st.glyphCount);
    const pages = Math.max(1, Number(st.totalPages));
    const hitRate = hits + misses > 0 ? hits / (hits + misses) : 0;
    const storageFailures = Number(st.fontStorageFailures);
    const storageEofs = Number(st.fontStorageEofs);

    state.fontLazy = {
      hits, misses, reads, ioBytes, ioMs, resident, glyphs, pages,
      hitRate: +hitRate.toFixed(4),
      readsPerPage: +(reads / pages).toFixed(2),
      residentMB: +(resident / 1048576).toFixed(2),
      ioPerPageMs: +(ioMs / pages).toFixed(3),
      storageFailures, storageEofs,
    };
    check('按需读真的发生了（未命中不为零）', misses > 0, `misses=${misses}`);
    check(`块缓存真的在挡：命中率 ${(hitRate * 100).toFixed(1)}%`,
              hitRate > 0.5, `hits=${hits} misses=${misses}`);
    check(`落存储次数 ${reads} 远小于字形数 ${glyphs}`, reads < glyphs,
              `${reads} vs ${glyphs}`);
    check(`每页落存储 ${(reads / pages).toFixed(2)} 次`, reads / pages < 200,
              `pages=${pages}`);
    check(`每页存储 IO ${(ioMs / pages).toFixed(3)} ms（上限 1 ms）`,
              ioMs / pages < 1.0, `ioMs=${ioMs.toFixed(3)}`);
    // 常驻的两个上界：不会超过累计读进来的字节，也不会超过"数据源个数乘单源上限"。
    check(`常驻 ${(resident / 1048576).toFixed(2)} MB 不超过累计读入`,
              resident <= ioBytes, `${resident} vs ${ioBytes}`);
    const residentCap = 3 * 4 * 1024 * 1024;  // 三张按需脸，每张的上限 4 MB
    check(
      `常驻 ${(resident / 1048576).toFixed(2)} MB（上限 ${(residentCap / 1048576).toFixed(1)} MB）`,
      resident <= residentCap, `resident=${resident}`);
    // 这条才是收益本身：整份三个字体约 20 MB，按需的常驻该只有它的零头。
    check('常驻不到整份字体的四分之一', resident < 5 * 1024 * 1024,
              `${(resident / 1048576).toFixed(2)} MB vs 约 20 MB`);
    check('按需装载后不缺字（骨架没漏掉排版要用的表）',
              Number(st.missingGlyphCount) === 0, `missing=${st.missingGlyphCount}`);
    // 两个诊断字段必须真的接在链条上：接到别处（或没接）时是 undefined，
    //   而 Number(undefined) 是 NaN —— 这一条既查"接上了"也查"是零"。
    check('存储失败计数已接到诊断口且为零（MEMFS 下不该有真失败）',
              Number.isFinite(storageFailures) && storageFailures === 0,
              `fontStorageFailures=${st.fontStorageFailures}`);
    check('文件尾计数已接到诊断口且为有限数',
              Number.isFinite(storageEofs), `fontStorageEofs=${st.fontStorageEofs}`);

    // 最要紧的一条：帧上必须真的有字。读数字再漂亮，画不出来就是没走通 ——
    //   而且这一条不能只看"非零"，要跟整份模式画同一页的墨迹比。
    //
    //   两次取墨迹必须先钉住同一个窗口、同一页：上面那几段把锚点挪到过第 50 页，
    //   而这里比的是"同一页"。不钉的话就成了"第 50 页 vs 第 0 页"（整帧版曾靠
    //   "这段语料只有 20 个词、每页长得差不多"侥幸过关，改成只数第一页就露馅）。
    //   只数第一页那几行也正是判据名字里的"同一页"：帧是窗口大的，按整帧数会掺进
    //   "窗口盖着哪几页"，那个数随窗口策略变，测的就成了窗口在哪而不是字画没画出来。
    const INK = `
      const F = await PIX();
      const f = F.data;
      const rows = F.w * D.pagePx().h * 4;
      let n = 0;
      for (let i = 0; i + 3 < rows; i += 4) {
        if (f[i] < 200 || f[i+1] < 200 || f[i+2] < 200) ++n;
      }
      return n;
    `;
    await probe.read('return await D.setViewportAnchor(0, 3);');
    const inkLazy = await probe.read(INK);
    await probe.read('return await D.reloadFonts("full");');
    await probe.read('await LOAD.stressDiverse(2000, 20); return true;');
    await probe.read('return await D.setViewportAnchor(0, 3);');
    const inkFull = await probe.read(INK);
    state.inkCompare = { lazy: inkLazy, full: inkFull };
    check('按需模式下帧上有字', inkLazy > 1000, `${inkLazy} px`);
    check(`按需与整份渲染同一页的墨迹一致（${inkLazy} vs ${inkFull}）`,
              inkFull > 1000 && Math.abs(inkLazy - inkFull) / Math.max(1, inkFull) < 0.02,
              `${inkLazy} vs ${inkFull}`);
  }

  log.section('[18] 字体块大小：块越小带宽越省、次数越贵');
  {
    // 三档块大小，常驻上限固定 3 MB（块数 = 上限 / 块大小）—— 只让"粒度"这一个变量变。
    //   换档之后必须重装字体：块缓存的配置在建脸时就定了，而且 FreeType 自己缓存过
    //   的字形不会因为换了配置就失效（不重装的话第二次量到的是"没读"）。
    const CAP = 3 * 1024 * 1024;
    const rows = [];
    for (const bs of [4096, 16384, 65536]) {
      const blocks = Math.floor(CAP / bs);
      await probe.read(`return await D.setFontCacheConfig(${bs}, ${blocks});`);
      await probe.read('return await D.reloadFonts("lazy");');
      await probe.read('await LOAD.stressDiverse(2000, 20); return true;');
      await probe.read('return await D.present();');
      const s = await probe.read('return D.editStats();');
      const hits = Number(s.fontCacheHits);
      const misses = Number(s.fontCacheMisses);
      rows.push({
        blockKB: bs / 1024,
        blocks,
        hitRate: hits + misses > 0 ? +(hits / (hits + misses)).toFixed(4) : 0,
        reads: Number(s.fontStorageReads),
        ioMB: +(Number(s.fontStorageBytes) / 1048576).toFixed(2),
        ioMs: +Number(s.fontIoMs).toFixed(2),
        residentMB: +(Number(s.fontResidentBytes) / 1048576).toFixed(2),
      });
    }
    state.blockSizes = rows;
    for (const r of rows) {
      console.log(`     块 ${String(r.blockKB).padStart(3)} KB x ${String(r.blocks).padStart(5)}` +
                  `（上限 3 MB）：命中 ${(r.hitRate * 100).toFixed(1)}%，` +
                  `读 ${r.reads} 次 / ${r.ioMB} MB，IO ${r.ioMs} ms，常驻 ${r.residentMB} MB`);
    }
    const small = rows[0];
    const big = rows[rows.length - 1];
    check('三档都真的读过存储（配置生效了）', rows.every((r) => r.reads > 0),
              rows.map((r) => r.reads).join(' / '));
    check(
      `块越小、读进来的字节越少（${big.blockKB} KB 的 ${big.ioMB} MB -> ${small.blockKB} KB 的 ${small.ioMB} MB）`,
      small.ioMB < big.ioMB, `${big.ioMB} -> ${small.ioMB} MB`);
    check(
      `块越小、命中率越低（${(small.hitRate * 100).toFixed(1)}% vs ${(big.hitRate * 100).toFixed(1)}%）`,
      small.hitRate <= big.hitRate, `${small.hitRate} vs ${big.hitRate}`);
  }

  log.section('[19] 真病历文本：用字更分散，缓存账更紧');
  {
    // 真文本由门面读文件并清洗（清洗逻辑只此一份，页面下拉用的是同一条路）。
    const textInfo = await probe.read('return await LOAD.medical(1);');
    console.log(`     病历文本：${textInfo.paragraphs} 段 / ${textInfo.chars} 字`);
    // 这一条查的是页面自己的下拉：旧宿主页把场景做成了 UI，库不带 UI。
    //   在库页面上跳过它（而不是为了它给库加一个 select —— 那是把页面塞进库）。
    if (await probe.read('return !!(window.__demo && window.__demo.diag);')) {
      log.note('页面下拉里有真病历这一项：库页面没有场景下拉，跳过');
    } else {
      check('页面下拉里有真病历这一项（场景入了 UI，不只是探针能用）',
                await probe.read(`return [...document.querySelectorAll('#scenario option')]
                                .some((o) => o.value === '8');`), 'id=8');
    }

    // 四个档位：块大小与常驻上限各变一次 —— 真文本的工作集比合成文本大，
    //   所以除了"块多大"，还要看"上限给多少"。
    const VARIANTS = [
      { kb: 4, capMB: 3 },
      { kb: 4, capMB: 6 },
      { kb: 16, capMB: 3 },
      { kb: 64, capMB: 3 },
    ];
    const rows = [];
    for (const v of VARIANTS) {
      const bs = v.kb * 1024;
      const blocks = Math.floor((v.capMB * 1024 * 1024) / bs);
      await probe.read(`return await D.setFontCacheConfig(${bs}, ${blocks});`);
      await probe.read('return await D.reloadFonts("lazy");');
      await probe.read('return await LOAD.medical(7);');
      await probe.read('return await D.present();');
      const s = await probe.read('return D.editStats();');
      const hits = Number(s.fontCacheHits);
      const misses = Number(s.fontCacheMisses);
      rows.push({
        label: `${v.kb} KB x ${blocks} 块（上限 ${v.capMB} MB）`,
        blockKB: v.kb,
        capMB: v.capMB,
        hitRate: hits + misses > 0 ? +(hits / (hits + misses)).toFixed(4) : 0,
        reads: Number(s.fontStorageReads),
        ioMB: +(Number(s.fontStorageBytes) / 1048576).toFixed(2),
        residentMB: +(Number(s.fontResidentBytes) / 1048576).toFixed(2),
        glyphs: Number(s.glyphCount),
        pages: Number(s.totalPages),
      });
    }
    state.medicalDoc = { text: textInfo, variants: rows };
    for (const r of rows) {
      console.log(`     ${r.label.padEnd(26)} 命中 ${(r.hitRate * 100).toFixed(1)}%，` +
                  `读 ${String(r.reads).padStart(5)} 次 / ${String(r.ioMB).padStart(6)} MB，` +
                  `常驻 ${String(r.residentMB).padStart(5)} MB（${r.pages} 页 / ${r.glyphs} 字形）`);
    }
    check('真文本装载成功并排出了页', rows.every((r) => r.glyphs > 0 && r.pages > 1),
              rows.map((r) => `${r.pages}页`).join(' / '));
    check('真文本下块越小读入越少', rows[0].ioMB < rows[3].ioMB,
              `4KB=${rows[0].ioMB} 64KB=${rows[3].ioMB} MB`);
    check('上限从 3 MB 提到 6 MB 后读入不上升（工作集越界会淘汰重读）',
              rows[1].ioMB <= rows[0].ioMB, `3MB=${rows[0].ioMB} 6MB=${rows[1].ioMB} MB`);
    // 真文本用字分散、重复少，命中率比合成文本低是正常的；关键是"块越小越省"的关系
    // 不但保持，而且差距拉大 —— 工作集一旦越界，大块会雪崩式淘汰重读。
    const synth = state.blockSizes || [];
    if (synth.length) {
      const rm = rows[3].ioMB / Math.max(0.01, rows[0].ioMB);
      const rs = synth[2].ioMB / Math.max(0.01, synth[0].ioMB);
      check(`真文本下"大块比小块多读"的倍数更大（${rm.toFixed(1)}x vs ${rs.toFixed(1)}x）`,
                rm > rs, `${rm.toFixed(1)}x vs ${rs.toFixed(1)}x`);
    }
  }

    await finishEdit(probe, consoleErrors, state);
  });
});
