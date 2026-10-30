/**
 * 样例文档：全部由公开方法搭出来。
 *
 * 每一条都在调 Editor 的公开方法，没有一条走后面。演示页的菜单、判据的夹具、别人抄的样板，
 *   三处用的是同一段代码：样例改坏了判据会红，这正是它该有的耦合。
 *
 * 反过来，专用入口（一行调用就出一份文档的那种）做不到示范：它演示的是抄近路，判据验的
 *   也是那条近路，读代码的人学不到任何东西。
 */
import type { Editor } from '../../src/index';
import { buildForm } from './form';
import { buildLong } from './long';
import { buildMedical } from './medical';
import { buildProse } from './prose';
import { buildTable } from './table';

/**
 * 一份样例：编号、菜单上的名字、搭它的那段代码。
 */
export interface Sample {
  /**
   * 编号（判据按它点名）。
   */
  id: string;

  /**
   * 菜单上显示的名字。
   */
  label: string;

  /**
   * 用公开方法把这份文档搭出来（覆盖当前内容）。
   * @param ed 编辑器
   * @param opts 可选项（目前只有 repeat：整篇重复几遍，给压测用）
   */
  build(ed: Editor, opts?: { repeat?: number }): Promise<void>;
}

/**
 * 全部样例。第一份是启动时装载的那一份。
 */
export const SAMPLES: Sample[] = [
  { id: 'prose', label: '正文', build: buildProse },
  { id: 'form', label: '表单', build: buildForm },
  { id: 'table', label: '表格', build: buildTable },
  { id: 'long', label: '长文', build: buildLong },
  { id: 'medical', label: '病历', build: buildMedical },
];

/**
 * 按编号搭一份样例。
 *
 * @param ed 编辑器
 * @param id 样例编号（见 SAMPLES）
 * @param opts 可选项（repeat：整篇重复几遍）
 * @returns 装载完成
 */
export async function runSample(
  ed: Editor,
  id: string,
  opts?: { repeat?: number },
): Promise<void> {
  const hit = SAMPLES.find((s) => s.id === id);
  if (!hit) throw new Error('没有这份样例：' + id);
  await hit.build(ed, opts);
}
