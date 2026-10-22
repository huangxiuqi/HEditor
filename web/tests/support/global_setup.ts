// 开跑之前清掉上一轮的输入轨迹。
//
// 不清的后果不是"多一份文件"，是"拿这一轮的 inline 与上一轮的 worker 比"——
//   两个 project 未必在同一次命令里都跑过，旧文件会让比对在没有真比对的情况下通过。
import { rmSync } from 'node:fs';
import { join } from 'node:path';

// 轨迹目录与输入探针写的是同一处。
const OUT = join(process.cwd(), '..', 'out', 'web');

// 清掉两条传输的轨迹文件。
export default function globalSetup(): void {
  for (const transport of ['inline', 'worker']) {
    rmSync(join(OUT, 'trail-' + transport + '.json'), { force: true });
  }
}
