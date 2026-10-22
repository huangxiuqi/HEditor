// 验收口径：跑构建产物 web/dist-app/。上线前与改判据时用这一份。
import { makeConfig } from './tests/support/config';

export default makeConfig('dist');
