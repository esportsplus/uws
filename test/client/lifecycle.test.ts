import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { spawn } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { App, Client, SSLApp, SSLClient } from '../../src/index';
import { dial, listen, sleep } from '../harness';
import type { TemplatedApp } from '../../src/index';
import type { Server } from '../harness';


let server: Server;
let sslServer: Server;


function build(): TemplatedApp {
    let app = App();

    app.ws('/echo', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    });
    app.ws('/reject', {
        upgrade: (res) => {
            res.writeStatus('403 Forbidden').end('no');
        }
    });

    return app;
}


beforeAll(() => {
    server = listen(build());
    sslServer = listen(SSLApp({ cert_file_name: '.tmp/cert.pem', key_file_name: '.tmp/key.pem', passphrase: '1234' }).ws('/echo', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    }));
});

afterAll(() => {
    server.close();
    sslServer.close();
});


describe('outbound connection lifecycle', () => {
    it('retires a graceful end() close before the next connect', async () => {
        let client = Client();

        for (let i = 0; i < 30; i++) {
            let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
                ws = await dialed.opened;

            ws.send('bye');
            await dialed.next();
            ws.end(1000, 'done');
            await dialed.closed;

            let rejected = dial(client, `ws://127.0.0.1:${server.port}/reject`);

            await rejected.failed;
        }

        client.close();
        await sleep(20);
    });

    it('retires failed connects to a closed port', async () => {
        let client = Client();

        for (let i = 0; i < 60; i++) {
            let dialed = dial(client, 'ws://127.0.0.1:1/');

            expect((await dialed.failed).code).toBe('ECONNREFUSED');
        }

        client.close();
        await sleep(20);
    });

    it('throws CLIENT_CLOSED when a closed client is reused', () => {
        let client = Client();

        client.close();

        expect(() => dial(client, `ws://127.0.0.1:${server.port}/echo`)).toThrow('CLIENT_CLOSED');
        expect(() => client.publish('topic', 'message')).toThrow('CLIENT_CLOSED');
        expect(() => client.numSubscribers('topic')).toThrow('CLIENT_CLOSED');
        expect(client.close()).toBe(client);
    });

    it('can close from failed, open, message and close handlers', async () => {
        let failedClient = Client();
        await new Promise<void>((resolve) => {
            failedClient.connect('ws://127.0.0.1:1/', {
                failed: () => {
                    failedClient.close();
                    resolve();
                },
                open: () => {}
            });
        });
        await sleep(10);

        let openClient = Client(), openDialed = dial(openClient, `ws://127.0.0.1:${server.port}/echo`, {
            open: () => { openClient.close(); }
        });
        await openDialed.closed;
        await sleep(10);

        let messageClient = Client(), messageDialed = dial(messageClient, `ws://127.0.0.1:${server.port}/echo`, {
            open: (ws) => { ws.send('close'); },
            message: () => { messageClient.close(); }
        });
        await messageDialed.closed;
        await sleep(10);

        let closeClient = Client(), closeDialed = dial(closeClient, `ws://127.0.0.1:${server.port}/echo`, {
            open: (ws) => { ws.end(); },
            close: () => { closeClient.close(); }
        });
        await closeDialed.closed;
        await sleep(10);
    });

    it('safely retires a slot and closes its client in the same tick', async () => {
        let client = Client(), dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, {
            open: (ws) => { ws.end(); },
            close: () => { client.close(); }
        });

        await dialed.closed;
        await sleep(20);
    });

    it('safely frees a client closed while resolving', async () => {
        let client = Client();

        client.connect('ws://a-host-that-does-not-resolve.invalid.test/', {
            failed: () => {},
            open: () => {}
        });
        client.close();
        await sleep(200);
    });

    /* RSS-based, so skip under ASAN: the sanitizer's shadow memory and redzones inflate per-allocation
     * growth far past the bound even with the leak fixed. The ASAN run instead proves no use-after-free. */
    it.skipIf(process.platform !== 'linux' || !!process.env.ASAN_OPTIONS)('stays bounded across thousands of reconnects', async () => {
        let client = Client();

        async function cycle(): Promise<void> {
            let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
                ws = await dialed.opened;

            ws.send('x');
            await dialed.next();
            ws.end(1000);
            await dialed.closed;
        }

        for (let i = 0; i < 500; i++) {
            await cycle();
        }

        await sleep(50);

        let baseline = process.memoryUsage().rss;

        for (let i = 0; i < 2500; i++) {
            await cycle();
        }

        await sleep(50);

        let growth = process.memoryUsage().rss - baseline;

        /* A per-connect leak would add three socket contexts plus a fistful of V8-persistent-holding
         * closures every cycle; 2500 cycles would blow well past this. The bound is generous to absorb
         * ordinary heap/JIT noise. */
        expect(growth).toBeLessThan(24 * 1024 * 1024);

        client.close();
        await sleep(20);
    });

    /* Each cycle owns a new parent context, TopicTree and (for SSL) SSL_CTX. close() must release
     * those owners on the next loop tick, rather than retaining them until environment teardown. */
    it.skipIf(process.platform !== 'linux' || !!process.env.ASAN_OPTIONS)('stays bounded across thousands of closed clients', async () => {
        async function cycle(client: ReturnType<typeof Client>, url: string): Promise<void> {
            let dialed = dial(client, url), ws = await dialed.opened;

            client.close();
            await dialed.closed;
            void ws;
        }

        for (let i = 0; i < 100; i++) {
            await cycle(Client(), `ws://127.0.0.1:${server.port}/echo`);
        }
        await sleep(50);
        let baseline = process.memoryUsage().rss;

        for (let i = 0; i < 2000; i++) {
            await cycle(Client(), `ws://127.0.0.1:${server.port}/echo`);
        }
        await sleep(50);
        expect(process.memoryUsage().rss - baseline).toBeLessThan(24 * 1024 * 1024);
    });

    /* Every SSLClient() parses Node's full default CA bundle into a fresh X509 store and then runs a TLS
     * handshake, so 2100 serial cycles take well over the default 10 s on CI runners. */
    it.skipIf(process.platform !== 'linux' || !!process.env.ASAN_OPTIONS)('stays bounded across thousands of closed SSL clients', async () => {
        async function cycle(): Promise<void> {
            let client = SSLClient({ reject_unauthorized: false }),
                dialed = dial(client, `wss://127.0.0.1:${sslServer.port}/echo`, { rejectUnauthorized: false });

            await dialed.opened;
            client.close();
            await dialed.closed;
        }

        for (let i = 0; i < 100; i++) {
            await cycle();
        }
        await sleep(50);
        let baseline = process.memoryUsage().rss;

        for (let i = 0; i < 2000; i++) {
            await cycle();
        }
        await sleep(50);
        expect(process.memoryUsage().rss - baseline).toBeLessThan(32 * 1024 * 1024);
    }, 60000);
});

describe('worker teardown', () => {
    function runScenario(scenario: string): Promise<{ code: number | null; stderr: string }> {
        return new Promise((resolve, reject) => {
            let child = spawn(process.execPath, ['--import', 'tsx', 'test/support/teardown-host.ts', scenario]),
                stderr = '';

            child.stderr.setEncoding('utf8');
            child.stderr.on('data', (chunk: string) => { stderr += chunk; });
            child.once('error', (error) => {
                child[Symbol.dispose]();
                reject(error);
            });
            child.once('close', (code) => {
                child[Symbol.dispose]();
                resolve({ code, stderr });
            });
        });
    }

    for (let scenario of ['resolving', 'handshaking', 'open', 'cooperative', 'server-listening', 'server-http-pending', 'server-ws-open', 'server-tls-handshaking', 'server-cooperative']) {
        it(`exits cleanly while ${scenario}`, async () => {
            let result = await runScenario(scenario);

            expect(result.code).toBe(0);
            expect(result.stderr).not.toMatch(/uv_loop_close|AddressSanitizer/i);
        });
    }

    it.skipIf(!process.env.UWS_NET_TESTS)('exits cleanly while connecting', async () => {
        let result = await runScenario('connecting');

        expect(result.code).toBe(0);
        expect(result.stderr).not.toMatch(/uv_loop_close|AddressSanitizer/i);
    });

    it('exits cleanly when close and worker termination share a tick', async () => {
        let worker = new Worker(new URL('../support/teardown-worker.ts', import.meta.url), {
            workerData: { scenario: 'closed' },
            execArgv: ['--import', 'tsx']
        });
        try {
            await new Promise<void>((resolve, reject) => {
                worker.once('message', resolve);
                worker.once('error', reject);
            });
        }
        finally {
            await worker.terminate();
        }
    });
});

describe('cancelled connect ownership', () => {
    it('safely frees userData for connects cancelled before open', async () => {
        /* connect() then close() before DNS resolves leaves the WebSocketClientContextData destructor as the
         * sole owner of pending.user (no emitFailed fires on a silent cancel). The frees run when each client
         * tears down; under ASAN this proves the destructor free is reached and single (no double-free), which
         * is the safety half of the leak fix. The reclaim half is the gc-guarded test below. */
        for (let i = 0; i < 40; i++) {
            let client = Client();

            client.connect('ws://a-host-that-does-not-resolve.invalid.test/', {
                userData: { i },
                failed: () => {},
                open: () => {}
            });
            client.close();
        }

        await sleep(30);
        expect(true).toBe(true);
    });

    it.skipIf(!globalThis.gc)('releases userData when close cancels a connect in the same tick', async () => {
        let client = Client(),
            userData: object | undefined = {},
            ref = new WeakRef(userData),
            resolveFinalized!: () => void,
            finalized = new Promise<void>((resolve) => { resolveFinalized = resolve; }),
            registry = new FinalizationRegistry<void>(() => resolveFinalized());

        registry.register(userData, undefined);

        client.connect('ws://a-host-that-does-not-resolve.invalid.test/', {
            userData,
            failed: () => {},
            open: () => {}
        });
        client.close();
        userData = undefined;

        await sleep(50);
        globalThis.gc!();
        await Promise.race([finalized, sleep(50)]);
        expect(ref.deref()).toBeUndefined();
    });
});
