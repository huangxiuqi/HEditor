// 字体注册判据：一行注册、按名字选、清单一律由字体文件自己报名字。
//
// 为什么单独一份：注册字体这条路的失败形态全是"看起来成功了"—— 名字对不上时画面
//   只是排出来另一份字体，下标写错时字体落到别的族上，重复注册时清单里静默多出几张脸。
//   这些都不会抛异常，所以判据必须逐条读回清单与生效值。
import { check } from '../support/check';
import { test } from '../support/probe';

const D = 'window.__demo';

test.describe('字体注册', () => {
  test('清单：名字来自字体文件，角色是内置那三份', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      return { list: e.fonts() };`);

    const list = r.list as Array<any>;
    check('★ 不配置时有三份内置字体', list.length === 3, String(list.length));
    check('下标从 0 起、连续', list.every((f, i) => f.index === i), JSON.stringify(list.map((f) => f.index)));
    // 族名由字体文件的 name 表给出，不是宿主声明的：三份必须各不相同且非空。
    //   NotoSansSC-VF 的 name 表自称 NotoSansSC-Thin，那是字重那一档的名字，族名仍是 Noto Sans SC。
    check('★ 每份都报得出族名', list.every((f) => typeof f.family === 'string' && f.family.length > 0),
          JSON.stringify(list.map((f) => f.family)));
    check('三份的族名互不相同', new Set(list.map((f) => f.family)).size === 3,
          JSON.stringify(list.map((f) => f.family)));
    // 界面那一行是显示名：内置三份给了中文名，字体文件里只有英文族名。
    //   下拉框与 format({font}) 都认它，所以它必须是清单里的一等字段。
    check('★ 显示名是中文那三个', JSON.stringify(list.map((f) => f.label)) ===
          JSON.stringify(['思源黑体', '蒙古文', '拉丁']),
          JSON.stringify(list.map((f) => f.label)));
    check('★ 角色是正文 / 蒙文 / 拉丁', list[0].role === 'cjk' && list[1].role === 'mongol' &&
          list[2].role === 'latin', JSON.stringify(list.map((f) => f.role)));
    check('每份都有文件名（字体身份的最后一级）',
          list.every((f) => typeof f.file === 'string' && /\.(ttf|otf)$/i.test(f.file)),
          JSON.stringify(list.map((f) => f.file)));
    check('默认整份装载', list.every((f) => f.lazy === false), JSON.stringify(list.map((f) => f.lazy)));
  });

  test('注册：重复的那份复用下标，字节注册加一张新脸，名字三种都能选', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      await e.setText('甲乙丙');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.flush();
      const before = e.fonts();
      const digest0 = await PIXH();

      // 同一份地址再注册一次：不该多出一张脸。地址取清单报的那一个 —— 两个口径下
      //   （源码直连 / 构建产物）字体所在的位置不同，测试不该各写一遍。
      const again = await e.addFont(before[2].url);
      await e.flush();
      const afterDup = e.fonts().length;

      // 同一份字节换个文件名：那是一次新的注册（身份的最后一级是文件名）。
      const bytes = await (await fetch(before[2].url)).arrayBuffer();
      const added = await e.addFont(new File([bytes], 'copy.ttf'));
      await e.flush();
      const afterAdd = e.fonts();
      const digest1 = await PIXH();

      // 三种名字各选一次，读回生效的下标。
      const byFile = await e.format({ font: 'copy.ttf' });
      await e.flush();
      const iFile = (await e.formatAt({ effective: true })).font;
      const byFamily = await e.format({ font: added[0].family });
      await e.flush();
      const iFamily = (await e.formatAt({ effective: true })).font;
      const byPs = await e.format({ font: 'NotoSansSC-Thin' });
      await e.flush();
      const iPs = (await e.formatAt({ effective: true })).font;

      // 界面上写的是显示名：照着界面写也得分得出来。先换到别的字体，
      //   否则"再选一次同一个下标"是幂等的（命令回 false，读回却是对的）。
      await e.format({ font: 1 });
      await e.flush();
      const byLabel = await e.format({ font: '思源黑体' });
      await e.flush();
      const iLabel = (await e.formatAt({ effective: true })).font;

      return { before: before.length, again, afterDup, added, afterAdd, digest0, digest1,
               byFile, iFile, byFamily, iFamily, byPs, iPs, byLabel, iLabel };`);

    check('★ 重复注册回同一个下标', r.again.length === 1 && r.again[0].index === 2,
          JSON.stringify(r.again));
    check('★ 重复注册不增脸', r.afterDup === r.before, r.before + ' → ' + r.afterDup);
    check('★ 字节注册加了一张新脸（下标 3）', r.added.length === 1 && r.added[0].index === 3,
          JSON.stringify(r.added));
    check('注册之后清单立刻读得到（同步读）', r.afterAdd.length === 4, String(r.afterAdd.length));
    check('新脸的族名与原文件相同', r.added[0].family === r.afterAdd[2].family,
          JSON.stringify([r.added[0].family, r.afterAdd[2].family]));
    // 只加脸不重排：注册不该让画面动一下（呈现归属 none）。
    check('★ 注册字体不改画面', r.digest0 === r.digest1, r.digest0 + ' → ' + r.digest1);
    check('★ 按文件名选到新脸', r.byFile === true && r.iFile === 3, String(r.iFile));
    check('★ 按族名选到先装的那一张', r.byFamily === true && r.iFamily === 2, String(r.iFamily));
    check('★ 按 PostScript 名选得到', r.byPs === true && r.iPs === 0, String(r.iPs));
    check('★ 按显示名（界面那一行）也选得到', r.byLabel === true && r.iLabel === 0,
          String(r.iLabel));
  });

  test('没注册的名字当场拒绝，文档不动', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.flush();
      const text0 = e.text();
      const mark0 = (await e.formatAt()).font;
      let why = '';
      try {
        await e.format({ font: '没有这份字体' });
      } catch (err) {
        why = String((err && err.message) || err);
      }
      await e.flush();
      let paraWhy = '';
      try {
        await e.paragraphFormat({ font: '也没有' });
      } catch (err) {
        paraWhy = String((err && err.message) || err);
      }
      await e.flush();
      return { text0, after: e.text(), mark0, mark1: (await e.formatAt()).font, why, paraWhy };`);

    check('★ 字符格式：报"没有注册这个字体"', /没有注册这个字体/.test(r.why), r.why);
    check('★ 段落格式：同一条拒绝', /没有注册这个字体/.test(r.paraWhy), r.paraWhy);
    check('★ 拒绝里列出已注册的显示名（知道该写哪一个）', /思源黑体/.test(r.why), r.why);
    check('文档没动', r.text0 === r.after, JSON.stringify([r.text0, r.after]));
    check('格式没动', r.mark0 === r.mark1, JSON.stringify([r.mark0, r.mark1]));
  });
});
