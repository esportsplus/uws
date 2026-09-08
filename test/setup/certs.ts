import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';


const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');


export default function setup(): void {
    execFileSync('bash', [join(root, 'scripts', 'gen-test-certs.sh')], {
        cwd: root,
        stdio: 'inherit'
    });
}
