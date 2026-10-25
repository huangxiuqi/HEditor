// 性能判据口径：Vite 源码直连，单 worker 独占跑。改一行就能量。
import { makePerfConfig } from './tests/support/config';

export default makePerfConfig('dev');
