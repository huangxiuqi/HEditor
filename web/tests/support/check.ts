// 断言口：一次性求值，不许重试。
//
// 引擎状态判据必须"调用返回后立刻读"。同线程宿主是同步的，把这条性质改成异步是本项目
// 反复回归的一类问题，而 Playwright 的 expect.poll 与 locator 断言会自动重试 ——
// 用在这类判据上等于把红改成绿。所以这里只留 expect.soft 的一次性形态。
import { expect } from '@playwright/test';

/**
 * 收一条断言；失败也继续把本节的其余判据跑完。
 *
 * @param name 断言名，报告里原样显示
 * @param ok 判定结果
 * @param detail 复现用的读数，接在断言名后面
 */
export function check(name: string, ok: boolean, detail?: string): void {
  expect.soft(ok, detail ? name + '（' + detail + '）' : name).toBe(true);
}

/**
 * 近值断言：偏差在容差内算通过。
 *
 * @param name 断言名
 * @param actual 实测值
 * @param expected 期望值
 * @param tol 允许偏差
 */
export function near(name: string, actual: number, expected: number, tol: number): void {
  check(name, Math.abs(actual - expected) <= tol, actual + ' 对 ' + expected);
}
