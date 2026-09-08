import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';


const checker = fileURLToPath(new URL('./check-package.mjs', import.meta.url));
const targets = ['win32_x64', 'darwin_x64', 'darwin_arm64', 'linux_x64', 'linux_arm64', 'linux_x64_musl'];

for (const scenario of [
    { name: 'accepts all six packaged binaries', targets, files: ['dist/*.node'], empty: false, valid: true },
    { name: 'rejects a Windows-only package', targets: ['win32_x64'], files: ['dist/*.node'], empty: false, valid: false },
    { name: 'rejects binaries excluded from the package', targets, files: ['build'], empty: false, valid: false },
    { name: 'rejects empty binaries', targets, files: ['dist/*.node'], empty: true, valid: false }
]) {
    test(scenario.name, () => {
        const dir = mkdtempSync(join(tmpdir(), 'uws-package-check-'));

        try {
            mkdirSync(join(dir, 'dist'));
            writeFileSync(join(dir, 'package.json'), JSON.stringify({
                name: 'uws-package-check-fixture', version: '1.0.0', files: scenario.files
            }));

            for (const target of scenario.targets) {
                writeFileSync(join(dir, 'dist', `uws_${target}_147.node`), scenario.empty ? '' : 'inventory fixture');
            }

            const result = spawnSync(process.execPath, [checker], { cwd: dir, encoding: 'utf8' });

            assert.ifError(result.error);
            assert.equal(result.status, scenario.valid ? 0 : 1, result.stderr);
            assert.match(scenario.valid ? result.stdout : result.stderr, /\[uws\/check-package\]/);
        }
        finally {
            assert.ok(dir.startsWith(join(tmpdir(), 'uws-package-check-')));
            rmSync(dir, { recursive: true, force: true });
        }
    });
}
