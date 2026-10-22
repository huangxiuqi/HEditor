import { DIAG_MEMBER, PUBLIC_MEMBERS } from './public_members.gen';

/**
 * 公开面的白名单闸。
 *
 * 为什么是 Proxy 而不是"约定"：约定在 JS 里等于没有 —— 使用者只要写 `editor.auditFrame`
 *   就能拿到诊断面，而那份形状不承诺稳定。这里让它当场抛，错误消息直接说清能调什么。
 *
 * 四类键必须放行，否则守卫会把正常用法也拦掉：
 *   · `then` —— `await editor` 会先读它探测 thenable；
 *   · `toJSON` —— `JSON.stringify(editor)`；
 *   · `constructor` —— 有些库会读它做类型判断；
 *   · Symbol —— 迭代协议、`Symbol.toStringTag` 之类。
 *
 * @param target 实现对象
 * @param debug  是否带 diag（只有 createEditor({ debug: true }) 才有）
 */
export function guard<T extends object>(target: T, debug: boolean): T {
  const allowed = new Set<string>(PUBLIC_MEMBERS);
  if (debug) allowed.add(DIAG_MEMBER);
  return new Proxy(target, {
    get(t, key, receiver) {
      if (typeof key === 'symbol') return Reflect.get(t, key, receiver);
      if (key === 'then' || key === 'toJSON' || key === 'constructor') {
        return Reflect.get(t, key, receiver);
      }
      if (!allowed.has(key)) {
        throw new TypeError(
          `${key} 不是公开 API —— 能调的只有 web/src/types.ts 里那 ${PUBLIC_MEMBERS.length} 个成员`,
        );
      }
      return Reflect.get(t, key, receiver);
    },
    set() {
      throw new TypeError('配置走 configure()，实例上没有可写的属性');
    },
    deleteProperty() {
      throw new TypeError('公开面是只读的');
    },
  }) as T;
}
