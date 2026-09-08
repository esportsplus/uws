import { execSync } from 'node:child_process';


const targets = ['win32_x64', 'darwin_x64', 'darwin_arm64', 'linux_x64', 'linux_arm64', 'linux_x64_musl'];
const expected = targets.map((target) => `dist/uws_${target}_147.node`);
const [inventory] = JSON.parse(execSync('npm pack --dry-run --json --ignore-scripts', {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'inherit']
}));
const files = new Map(inventory.files.map((file) => [file.path, file.size]));
const missing = expected.filter((path) => !files.get(path));

if (missing.length) {
    throw new Error(`[uws/check-package] Package is missing required nonempty native binaries: ${missing.join(', ')}`);
}

console.log('[uws/check-package] All six platform binaries are included in the npm package.');
