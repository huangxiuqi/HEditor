/**
 * 病历样例：一份 markdown 正文读进来铺成文档。
 *
 * 示范的是内容从哪来 —— 正文是一份可替换的文件，不编进产物。服务端下发、用户上传、
 *   模板库拉取，三条路在宿主这边是同一件事：拿到字符串，清掉 markdown 记号，交给 setText。
 * 清洗是宿主的活（引擎不认识 markdown），切段是 setText 的契约。
 */
import type { Editor } from '../../src/index';

/**
 * 清掉 markdown 的记号：加粗、标题、有序列表、行尾反斜杠。
 *
 * 不是通用 markdown 解析器 —— 样例只要"读起来像一份公文"，不追求语法完整。
 * @param raw 原文
 * @returns 一行一段的纯文本
 */
function cleanLines(raw: string): string {
  return raw
    .split('\n')
    .map((l) =>
      l
        .replace(/\*\*/g, '')
        .replace(/\s*\\$/, '')
        .replace(/^#+\s*/, '')
        .replace(/^\d+[.、]\s*/, '')
        .trim(),
    )
    .filter((l) => l.length > 0)
    .join('\n');
}

/**
 * 搭病历样例。
 * @param ed 编辑器
 * @param opts repeat：整篇重复几遍（压测要看工作集更大时的字体缓存账）
 * @returns 搭建完成
 */
export async function buildMedical(ed: Editor, opts?: { repeat?: number }): Promise<void> {
  // 绝对路径：素材在仓库根（web/ 之外），开发服务器与构建产物都由同一个静态根发出去。
  const res = await fetch('/assets/samples/medical-records.md', { cache: 'no-store' });
  if (!res.ok) throw new Error(`病历样例读取失败：HTTP ${res.status}`);
  const repeat = Math.max(1, Math.trunc(opts?.repeat ?? 1));
  await ed.setText(cleanLines(await res.text()), { repeat });
  await ed.selectAll();
  await ed.paragraphFormat({ size: 12, spaceAfter: 6, lineSpacing: { rule: 'auto', factor: 1.5 } });
  await ed.setCaret({ para: 0, offset: 0 });
}
