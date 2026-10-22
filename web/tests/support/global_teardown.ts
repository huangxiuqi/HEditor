// 收尾：两条传输的输入轨迹必须逐位相同。
//
// 这条判据靠"两个 project 都跑过"才成立。只跑了一个 project 时跳过 ——
//   那时候没有可比对象，硬比就等于拿旧文件骗自己（开跑前已清，所以这里缺文件即未跑）。
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

// 轨迹目录与输入探针写的是同一处。
const OUT = join(process.cwd(), '..', 'out', 'web');

// 一步轨迹的比较形状：标签 + 段号 + 偏移 + 选区两端。
type Row = [string, number, number, boolean, number, number];

/**
 * 读一份轨迹，读不到就是没跑过。
 *
 * @param transport 传输名
 * @returns 轨迹行；文件不存在返回 null
 */
function load(transport: string): Row[] | null {
  try {
    return JSON.parse(readFileSync(join(OUT, 'trail-' + transport + '.json'), 'utf8')) as Row[];
  } catch {
    return null;
  }
}

// 比对两份轨迹，不一致就抛（整轮判失败）。
export default function globalTeardown(): void {
  const inline = load('inline');
  const worker = load('worker');
  if (!inline || !worker) return;

  const n = Math.min(inline.length, worker.length);
  for (let i = 0; i < n; i++) {
    const a = JSON.stringify(inline[i]);
    const b = JSON.stringify(worker[i]);
    if (a !== b) {
      throw new Error('两条传输的输入轨迹在第 ' + (i + 1) + ' 步分叉：inline ' + a + ' / worker ' + b);
    }
  }
  if (inline.length !== worker.length) {
    throw new Error('两条传输的输入轨迹步数不同：inline ' + inline.length + ' / worker ' +
      worker.length);
  }
}
