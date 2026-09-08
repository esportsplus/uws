import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const tsc = join(root, 'node_modules', '@esportsplus', 'typescript', 'bin', 'tsc');
const nativeBinaryExists = existsSync(join(root, 'dist', `uws_${process.platform}_${process.arch}_${process.versions.modules}.node`));
const examples = [
  'AsyncFunction.ts',
  'AutomaticPortSelection.ts',
  'Backpressure.ts',
  'Benchmarker.ts',
  'Broadcast.ts',
  'CachedHelloWorld.ts',
  'GracefulShutdown.ts',
  'Headers.ts',
  'HelloWorld.ts',
  'JsonPost.ts',
  'PeerCertificate.ts',
  'ProxyProtocol.ts',
  'PubSub.ts',
  'RateLimit.ts',
  'ReconnectingClient.ts',
  'Router.ts',
  'ServerName.ts',
  'ServerSentEvents.ts',
  'SlowReceiver.ts',
  'Upload.ts',
  'Upgrade.ts',
  'UpgradeAsync.ts',
  'WebSockets.ts',
  'WorkerThreads.ts'
];

const skippedExamples = [
  'VideoStreamer.ts',
  'VideoStreamerChunked.ts',
  'VideoStreamerSync.ts'
];

function typecheckExamples(): void {
  const directory = mkdtempSync(join(tmpdir(), 'uws-examples-'));
  const configPath = join(directory, 'tsconfig.json');

  try {
    writeFileSync(configPath, JSON.stringify({
      extends: join(root, 'tsconfig.json'),
      include: [join(root, 'storage', 'examples', '**', '*.ts')]
    }));
    const result = spawnSync(process.execPath, [tsc, '--noEmit', '-p', configPath], {
      cwd: root,
      encoding: 'utf8'
    });
    expect(result.status, result.stdout + result.stderr).toBe(0);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
}

function runForHalfASecond(example: string): Promise<{ code: number | null; signal: NodeJS.Signals | null; stderr: string }> {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['--import', 'tsx', join(root, 'storage', 'examples', example)], {
      cwd: root,
      stdio: ['ignore', 'ignore', 'pipe']
    });
    let stderr = '';
    let killed = false;
    const timer = setTimeout(() => {
      killed = true;
      child.kill('SIGTERM');
    }, 500);

    child.stderr.on('data', (chunk: Buffer) => { stderr += chunk; });
    child.once('error', (error) => {
      clearTimeout(timer);
      child[Symbol.dispose]();
      reject(error);
    });
    child.once('exit', (code, signal) => {
      child[Symbol.dispose]();
      clearTimeout(timer);
      if (!killed) {
        reject(new Error(`${example} exited before the smoke-test kill (code ${code}, signal ${signal}): ${stderr}`));
        return;
      }
      resolve({ code, signal, stderr });
    });
  });
}

describe('examples', () => {
  it('type-checks every example with the project TypeScript configuration', () => {
    typecheckExamples();
  }, 30_000);

  it.skipIf(!nativeBinaryExists)('starts every runnable example without throwing', async () => {
    for (const example of examples) {
      const result = await runForHalfASecond(example);
      expect(result.signal, `${example}: ${result.stderr}`).toBe('SIGTERM');
    }
  }, 30_000);

  it('accounts for every example in the smoke-test plan', () => {
    expect([...examples, ...skippedExamples].sort()).toEqual(
      readdirSync(join(root, 'storage', 'examples')).filter((file) => file.endsWith('.ts')).sort()
    );
  });
});
