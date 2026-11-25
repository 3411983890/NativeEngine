import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
let executable;
try {
  executable = (await import('electron')).default;
} catch {
  process.stderr.write('缺少编辑器依赖，请先在项目目录运行 npm install。\n');
  process.exit(1);
}
const environment = { ...process.env };
// Some terminals inherit this flag from their own Electron host.
delete environment.ELECTRON_RUN_AS_NODE;
// Escape hatch for environments where Chromium's sandbox or GPU process cannot start (CI,
// restricted service sessions). Empty on a normal desktop, so nothing is disabled by default.
const extra = (process.env.NG_ELECTRON_FLAGS ?? '').split(/\s+/).filter(Boolean);
const child = spawn(executable, [...extra, path.join(root, 'editor', 'main.mjs')], {
  cwd: root, env: environment, stdio: 'inherit', windowsHide: false,
});
child.once('error', error => {
  process.stderr.write(`无法启动编辑器：${error.message}\n`);
  process.exitCode = 1;
});
child.once('exit', code => { process.exitCode = code ?? 1; });
