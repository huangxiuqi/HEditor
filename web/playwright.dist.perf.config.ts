// 性能判据的验收口径：跑构建产物，单 worker 独占跑。
import { makePerfConfig } from './tests/support/config';

export default makePerfConfig('dist');
