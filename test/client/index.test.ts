import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createHash } from 'node:crypto';
import { createServer } from 'node:net';
import { App, Client, DEDICATED_COMPRESSOR, SHARED_COMPRESSOR, SSLApp, SSLClient } from '../../src/index';
import { dial, listen, sleep, startConnectProxy, text } from '../harness';
import type { AddressInfo, Server as NetServer } from 'node:net';
import type { TemplatedApp, TemplatedClient } from '../../src/index';
import type { Server } from '../harness';

import { WebSocketServer } from 'ws';


type RawServer = {
    close: () => void;
    port: number;
};

type WsServer = {
    close: () => Promise<void>;
    port: number;
};


const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const TLS_OPTIONS = { cert_file_name: '.tmp/cert.pem', key_file_name: '.tmp/key.pem', passphrase: '1234' };

const captured: Record<string, unknown> = {};


let client: TemplatedClient;
let rawServers: NetServer[] = [];
let server: Server;


function accept(key: string): string {
    return createHash('sha1').update(key + WS_GUID).digest('base64');
}

function build(): TemplatedApp {
    let app = App();

    app.ws('/echo', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    });
    app.ws('/echo-shared', {
        compression: SHARED_COMPRESSOR,
        message: (ws, data, isBinary) => { ws.send(data, isBinary, true); }
    });
    app.ws('/echo-dedicated', {
        compression: DEDICATED_COMPRESSOR,
        message: (ws, data, isBinary) => { ws.send(data, isBinary, true); }
    });
    app.ws('/capture', {
        open: () => {},
        upgrade: (res, req, context) => {
            captured.authorization = req.getHeader('authorization');
            captured.custom = req.getHeader('x-custom');

            res.upgrade(
                {},
                req.getHeader('sec-websocket-key'),
                req.getHeader('sec-websocket-protocol'),
                req.getHeader('sec-websocket-extensions'),
                context
            );
        }
    });
    app.ws('/proto', {
        upgrade: (res, req, context) => {
            let offered = req.getHeader('sec-websocket-protocol'),
                chosen = offered.split(',')[0].trim();

            res.upgrade(
                {},
                req.getHeader('sec-websocket-key'),
                chosen,
                req.getHeader('sec-websocket-extensions'),
                context
            );
        }
    });
    app.ws('/frag', {
        open: (ws) => {
            ws.sendFirstFragment('a');
            ws.sendFragment('b');
            ws.sendLastFragment('c');
        }
    });
    app.ws('/reject', {
        upgrade: (res) => {
            res.writeStatus('403 Forbidden').end('no');
        }
    });
    app.ws('/pubsub', {
        open: (ws) => {
            ws.subscribe('room');
        }
    });

    return app;
}

/** Minimal raw TCP server that answers one crafted HTTP response, computing a valid
 * Sec-WebSocket-Accept from the client's key so tests can flip individual response fields. */
function raw(respond: (key: string) => string | null, mode: 'headers' | 'first-byte' | 'destroy' = 'headers'): Promise<RawServer> {
    return new Promise((resolve) => {
        let netServer = createServer((socket) => {
            let buffer = '';

            socket.on('error', () => {});
            socket.on('data', (chunk) => {
                if (mode === 'destroy') {
                    socket.destroy();
                    return;
                }

                buffer += chunk.toString('latin1');

                if (mode === 'headers' && buffer.indexOf('\r\n\r\n') === -1) {
                    return;
                }

                let match = mode === 'headers' ? /sec-websocket-key:\s*(.+)\r\n/i.exec(buffer) : null,
                    response = respond(match ? match[1].trim() : '');

                if (response !== null) {
                    socket.write(response);
                }
            });
        });

        rawServers.push(netServer);
        netServer.listen(0, '127.0.0.1', () => {
            resolve({ close: () => netServer.close(), port: (netServer.address() as AddressInfo).port });
        });
    });
}

function wsServer(onConnection: (ws: import('ws').WebSocket) => void): Promise<WsServer> {
    return new Promise((resolve, reject) => {
        let peers = new Set<import('ws').WebSocket>(),
            server = new WebSocketServer({ host: '127.0.0.1', port: 0 });

        server.once('error', reject);
        server.on('connection', (ws) => {
            peers.add(ws);
            ws.once('close', () => { peers.delete(ws); });
            onConnection(ws);
        });
        server.once('listening', () => {
            server.off('error', reject);
            resolve({
                close: () => new Promise((done) => {
                    for (let peer of peers) {
                        peer.terminate();
                    }
                    server.close(() => { done(); });
                }),
                port: (server.address() as AddressInfo).port
            });
        });
    });
}

function upgradeResponse(key: string, overrides: Record<string, string | null> = {}): string {
    let headers: Record<string, string | null> = {
        'Connection': 'Upgrade',
        'Sec-WebSocket-Accept': accept(key),
        'Upgrade': 'websocket',
        ...overrides
    };

    let lines = ['HTTP/1.1 101 Switching Protocols'];

    for (let name in headers) {
        if (headers[name] !== null) {
            lines.push(`${name}: ${headers[name]}`);
        }
    }

    return lines.join('\r\n') + '\r\n\r\n';
}


beforeAll(() => {
    server = listen(build());
    client = Client();
});

afterAll(() => {
    client.close();
    server.close();

    for (let netServer of rawServers) {
        netServer.close();
    }
});


describe('throwing option getters', () => {
    it('throws when the connect maxPayloadLength getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                get maxPayloadLength(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect headers getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                get headers(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect header value getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                headers: { get 'x-custom'(): never { throw new Error('boom'); } }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect protocols getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                get protocols(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect protocol element getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                protocols: Object.defineProperty(['chat'], '0', {
                    get(): never { throw new Error('boom'); }
                })
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect userData getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                get userData(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect open getter throws', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                get open(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect open getter throws after userData is provided', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                userData: { id: 42 },
                get open(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the connect failed getter throws after userData is provided', () => {
        let disposable = Client();

        try {
            expect(() => disposable.connect(`ws://127.0.0.1:${server.port}/echo`, {
                userData: { id: 42 },
                get failed(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            disposable.close();
        }
    });

    it('throws when the Client proxy getter throws', () => {
        let disposable: TemplatedClient | undefined;

        try {
            expect(() => {
                disposable = Client({
                    get proxy(): never { throw new Error('boom'); }
                });
            }).toThrow('boom');
        }
        finally {
            disposable?.close();
        }
    });
});

describe('connect handler validation', () => {
    it('throws for a null open handler', () => {
        let disposable = Client();

        try {
            // @ts-expect-error Exercise runtime validation of an invalid handler.
            expect(() => disposable.connect('ws://127.0.0.1:1/', { open: null })).toThrow();
        }
        finally {
            disposable.close();
        }
    });

    it('throws for a string message handler', () => {
        let disposable = Client();

        try {
            // @ts-expect-error Exercise runtime validation of an invalid handler.
            expect(() => disposable.connect('ws://127.0.0.1:1/', { message: 'x' })).toThrow();
        }
        finally {
            disposable.close();
        }
    });

    it('throws for an object close handler', () => {
        let disposable = Client();

        try {
            // @ts-expect-error Exercise runtime validation of an invalid handler.
            expect(() => disposable.connect('ws://127.0.0.1:1/', { close: {} })).toThrow();
        }
        finally {
            disposable.close();
        }
    });

    it('throws for a string failed handler', () => {
        let disposable = Client();

        try {
            // @ts-expect-error Exercise runtime validation of an invalid handler.
            expect(() => disposable.connect('ws://127.0.0.1:1/', { failed: 'x' })).toThrow();
        }
        finally {
            disposable.close();
        }
    });

    it('accepts valid handlers and echoes a text message', async () => {
        let echo = listen(build()),
            disposable = Client(),
            dialed!: ReturnType<typeof dial>,
            timer: ReturnType<typeof setTimeout> | undefined;

        try {
            expect(() => {
                dialed = dial(disposable, `ws://127.0.0.1:${echo.port}/echo`);
            }).not.toThrow();

            let timeout = new Promise<never>((_resolve, reject) => {
                timer = setTimeout(() => { reject(new Error('Echo round trip timed out')); }, 3000);
            });

            let ws = await Promise.race([dialed.opened, timeout]);

            ws.send('hello');

            let received = await Promise.race([dialed.next(), timeout]);

            expect(text(received.data)).toBe('hello');
            expect(received.isBinary).toBe(false);

            ws.close();
            await Promise.race([dialed.closed, timeout]);
        }
        finally {
            clearTimeout(timer);
            disposable.close();
            echo.close();
        }
    });
});

describe('outbound handshake and framing', () => {
    it('opens and echoes a text message', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened;

        ws.send('hello');

        let received = await dialed.next();

        expect(text(received.data)).toBe('hello');
        expect(received.isBinary).toBe(false);

        ws.close();
        await dialed.closed;
    });

    it('opens and echoes a binary message', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened,
            payload = Buffer.from([1, 2, 3, 4, 5]);

        ws.send(payload, true);

        let received = await dialed.next();

        expect(received.isBinary).toBe(true);
        expect(Buffer.compare(received.data, payload)).toBe(0);

        ws.close();
        await dialed.closed;
    });

    it('reassembles a fragmented server message', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/frag`);

        await dialed.opened;

        let received = await dialed.next();

        expect(text(received.data)).toBe('abc');

        (await dialed.opened).close();
        await dialed.closed;
    });

    it('exposes userData from open through getUserData', async () => {
        let dialed = dial<{ id: number }>(client, `ws://127.0.0.1:${server.port}/echo`, { userData: { id: 42 } }),
            ws = await dialed.opened;

        expect((ws.getUserData() as { id: number }).id).toBe(42);

        ws.close();
        await dialed.closed;
    });

    it('sends custom request headers to the server', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/capture`, {
            headers: { authorization: 'Bearer token', 'x-custom': 'value' }
        });

        await dialed.opened;

        expect(captured.authorization).toBe('Bearer token');
        expect(captured.custom).toBe('value');

        (await dialed.opened).close();
        await dialed.closed;
    });

    it('accepts a subprotocol the server selected from the offer', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/proto`, { protocols: ['chat', 'json'] });

        await dialed.opened;

        (await dialed.opened).close();
        await dialed.closed;
    });

    it('reports close when the server ends the socket', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened;

        ws.send('bye');
        await dialed.next();
        ws.end(1000, 'done');

        let result = await dialed.closed;

        expect(result.code).toBe(1000);
    });
});

describe('outbound handshake failures', () => {
    it('reports HTTP_STATUS for a non-101 response', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/reject`),
            error = await dialed.failed;

        expect(error.code).toBe('HTTP_STATUS');
        expect(error.status).toBe(403);
    });

    it('rejects a bad Sec-WebSocket-Accept', async () => {
        let rawServer = await raw((key) => upgradeResponse(key, { 'Sec-WebSocket-Accept': 'not-the-right-value' })),
            dialed = dial(client, `ws://127.0.0.1:${rawServer.port}/`),
            error = await dialed.failed;

        expect(error.code).toBe('HANDSHAKE_INVALID');
    });

    it('rejects a subprotocol that was not offered', async () => {
        let rawServer = await raw((key) => upgradeResponse(key, { 'Sec-WebSocket-Protocol': 'unoffered' })),
            dialed = dial(client, `ws://127.0.0.1:${rawServer.port}/`, { protocols: ['chat'] }),
            error = await dialed.failed;

        expect(error.code).toBe('HANDSHAKE_INVALID');
    });

    it('reports ECONNREFUSED for a closed port', async () => {
        let dialed = dial(client, 'ws://127.0.0.1:1/'),
            error = await dialed.failed;

        expect(error.code).toBe('ECONNREFUSED');
    });

    it('does not require a failed handler', async () => {
        let disposable = Client(),
            opened = false;

        try {
            disposable.connect('ws://127.0.0.1:1/', {
                message: () => {},
                open: () => { opened = true; }
            });
            await sleep(300);

            expect(opened).toBe(false);
        }
        finally {
            disposable.close();
        }
    });

    it('reports ECONNRESET when the peer resets mid-handshake', async () => {
        let reset = await raw(() => null, 'destroy'),
            error = await dial(client, `ws://127.0.0.1:${reset.port}/`).failed;

        expect(error.code).toBe('ECONNRESET');
    });

    it('reports TLS_HANDSHAKE for a plaintext response to TLS', async () => {
        let plaintext = await raw(() => 'HTTP/1.1 400 Bad Request\r\n\r\n', 'first-byte'),
            secure = SSLClient({ reject_unauthorized: false });

        try {
            let error = await dial(secure, `wss://127.0.0.1:${plaintext.port}/`).failed;

            expect(error.code).toBe('TLS_HANDSHAKE');
        }
        finally {
            secure.close();
        }
    });

    it('reports an EAI_* code for an unresolvable host', async () => {
        let dialed = dial(client, 'ws://nonexistent.invalid/'),
            error = await dialed.failed;

        expect(error.code.startsWith('EAI_')).toBe(true);
    }, 10000);

    it.skipIf(!process.env.UWS_NET_TESTS)('reports ETIMEDOUT when the connect exceeds connectTimeout', async () => {
        let start = Date.now(),
            dialed = dial(client, 'ws://192.0.2.1/', { connectTimeout: 500 }),
            error = await dialed.failed;

        expect(error.code).toBe('ETIMEDOUT');
        expect(Date.now() - start).toBeLessThan(4000);
    }, 5000);

    it('times out a silent handshake with ms precision', async () => {
        let silent = await raw(() => null),
            start = Date.now(),
            dialed = dial(client, `ws://127.0.0.1:${silent.port}/`, { handshakeTimeout: 1000 }),
            error = await dialed.failed;

        expect(error.code).toBe('ETIMEDOUT');
        expect(Date.now() - start).toBeGreaterThan(900);
        expect(Date.now() - start).toBeLessThan(3500);
    }, 8000);

    it('throws URL_INVALID without firing connection callbacks', () => {
        for (let url of [
            'http://127.0.0.1/',
            'ws://:80/',
            'ws://127.0.0.1:70000/',
            'wss://',
            'ws://127.0.0.1\r\nX-Injected: 1/',
            'ws://127.0.0.1:9001/\r\nX-Injected: 1',
            'ws://127.0.0.1:9001/path\nfoo'
        ]) {
            let disposable = Client(),
                failed = false,
                opened = false;

            try {
                expect(() => dial(disposable, url, {
                    failed: () => { failed = true; },
                    open: () => { opened = true; }
                })).toThrow('URL_INVALID');
                expect(failed).toBe(false);
                expect(opened).toBe(false);
            }
            finally {
                disposable.close();
            }
        }
    });

    it('throws HEADER_INVALID without firing connection callbacks', () => {
        for (let headers of <Record<string, string>[]>[
            { 'sec-websocket-key': 'x' },
            { host: 'x' },
            { 'x-a': 'v\r\nInjected: 1' }
        ]) {
            let disposable = Client(),
                failed = false,
                opened = false;

            try {
                expect(() => dial(disposable, `ws://127.0.0.1:${server.port}/echo`, {
                    failed: () => { failed = true; },
                    headers,
                    open: () => { opened = true; }
                })).toThrow('HEADER_INVALID');
                expect(failed).toBe(false);
                expect(opened).toBe(false);
            }
            finally {
                disposable.close();
            }
        }
    });

    it('rejects unsafe protocols with HEADER_INVALID without firing connection callbacks', () => {
        for (let protocols of [['a\r\nb'], ['a,b']]) {
            let disposable = Client(),
                failed = false,
                opened = false;

            try {
                expect(() => dial(disposable, `ws://127.0.0.1:${server.port}/echo`, {
                    failed: () => { failed = true; },
                    open: () => { opened = true; },
                    protocols
                })).toThrow('HEADER_INVALID');
                expect(failed).toBe(false);
                expect(opened).toBe(false);
            }
            finally {
                disposable.close();
            }
        }
    });
});

describe('outbound lifecycle and pub/sub', () => {
    it('fans out client.publish to subscribed sockets', async () => {
        let first = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            second = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            firstWs = await first.opened,
            secondWs = await second.opened;

        firstWs.subscribe('room');
        secondWs.subscribe('room');
        client.publish('room', 'broadcast');

        let a = await first.next(),
            b = await second.next();

        expect(text(a.data)).toBe('broadcast');
        expect(text(b.data)).toBe('broadcast');

        firstWs.close();
        secondWs.close();
        await Promise.all([first.closed, second.closed]);
    });

    it('delivers a batched client.publish run in order', async () => {
        let dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened,
            expected = Array.from({ length: 40 }, (_, i) => `msg-${i}`),
            received: string[] = [];

        ws.subscribe('room');

        for (let payload of expected) {
            client.publish('room', payload);
        }

        for (let i = 0; i < expected.length; i++) {
            received.push(text((await dialed.next()).data));
        }

        expect(received).toEqual(expected);

        ws.close();
        await dialed.closed;
    });

    /* A still-connecting (not yet TCP-established) connect is cancelled silently by close(); a local
     * server would instead complete the TCP connect and close() would correctly fire failed. So this needs
     * an unroutable destination that stays in the connecting state - gated like the ETIMEDOUT test. */
    it.skipIf(!process.env.UWS_NET_TESTS)('cancels a pending connect on client.close() without firing callbacks', async () => {
        let quiet = Client(),
            dialed = dial(quiet, 'ws://192.0.2.1/', { connectTimeout: 5000 }),
            fired = false;

        void dialed.opened.then(() => { fired = true; });
        void dialed.failed.then(() => { fired = true; });

        await sleep(50);
        quiet.close();
        await sleep(250);

        expect(fired).toBe(false);
    });

    it('closes open sockets on client.close()', async () => {
        let disposable = Client(),
            dialed = dial(disposable, `ws://127.0.0.1:${server.port}/echo`);

        await dialed.opened;
        disposable.close();

        let result = await dialed.closed;

        expect(typeof result.code).toBe('number');
    });
});

describe('outbound frame-phase settings', () => {
    it('closes when a server message exceeds maxPayloadLength', async () => {
        let peer!: import('ws').WebSocket,
            controlled = await wsServer((ws) => {
                peer = ws;
            }),
            dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, { maxPayloadLength: 1024 });

        await dialed.opened;
        peer.send(Buffer.alloc(2048));

        let closed = await dialed.closed;

        expect(closed.code).toBe(1006);
        expect(closed.reason).toContain('Received too big message');
        await controlled.close();
    });

    it('drops messages over maxBackpressure and drains after the peer resumes', async () => {
        let peer!: import('ws').WebSocket,
            controlled = await wsServer((ws) => {
                peer = ws;
                (ws as unknown as { _socket: { pause: () => void } })._socket.pause();
            }),
            resolveDropped!: () => void,
            resolveDrained!: () => void,
            dropped: Promise<void>,
            drained: Promise<void>;

        dropped = new Promise((resolve) => { resolveDropped = resolve; });
        drained = new Promise((resolve) => { resolveDrained = resolve; });

        let dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, {
                closeOnBackpressureLimit: false,
                dropped: () => { resolveDropped(); },
                drain: () => { resolveDrained(); },
                maxBackpressure: 1024
            }),
            ws = await dialed.opened,
            statuses: number[] = [],
            payload = Buffer.alloc(64 * 1024, 7);

        for (let i = 0; i < 4000; i++) {
            let status = ws.send(payload, true);

            statuses.push(status);
            if (status === 2) {
                break;
            }
        }

        expect(statuses.some((status) => status === 0 || status === 2)).toBe(true);
        expect(statuses).toContain(2);
        await dropped;
        expect(ws.getBufferedAmount()).toBeGreaterThan(0);

        (peer as unknown as { _socket: { resume: () => void } })._socket.resume();
        await drained;

        ws.close();
        await dialed.closed;
        await controlled.close();
    }, 15000);

    it('closes when closeOnBackpressureLimit is set', async () => {
        let controlled = await wsServer((peer) => {
                (peer as unknown as { _socket: { pause: () => void } })._socket.pause();
            }),
            dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, {
                closeOnBackpressureLimit: true,
                maxBackpressure: 1024
            }),
            ws = await dialed.opened,
            payload = Buffer.alloc(64 * 1024, 7);

        for (let i = 0; i < 4000; i++) {
            if (ws.send(payload, true) === 2) {
                break;
            }
        }

        expect((await dialed.closed).code).toBe(1006);
        await controlled.close();
    }, 15000);

    it('handles pings and pongs with masked client control frames', async () => {
        let resolveServerPong!: (data: string) => void,
            resolveServerPing!: (data: string) => void,
            serverPong = new Promise<string>((resolve) => { resolveServerPong = resolve; }),
            serverPing = new Promise<string>((resolve) => { resolveServerPing = resolve; }),
            controlled = await wsServer((peer) => {
                peer.once('pong', (data) => { resolveServerPong(data.toString()); });
                peer.once('ping', (data) => { resolveServerPing(data.toString()); });
                peer.ping('x');
            }),
            resolveClientPing!: (data: string) => void,
            clientPing = new Promise<string>((resolve) => { resolveClientPing = resolve; }),
            dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, {
                ping: (_ws, data) => { resolveClientPing(text(data)); }
            }),
            ws = await dialed.opened;

        ws.ping('y');

        expect(await clientPing).toBe('x');
        expect(await serverPong).toBe('x');
        expect(await serverPing).toBe('y');

        ws.close();
        await dialed.closed;
        await controlled.close();
    });
});

describe.skipIf(!process.env.UWS_SLOW_TESTS)('outbound idle frame-phase settings', () => {
    it('closes an idle client when automatic pings are disabled', async () => {
        let controlled = await wsServer(() => {}),
            dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, {
                idleTimeout: 8,
                sendPingsAutomatically: false
            }),
            closed = await dialed.closed;

        expect(closed.code).toBe(1006);
        await controlled.close();
    }, 20000);

    it('sends masked automatic pings and remains open', async () => {
        let resolvePing!: () => void,
            serverPing = new Promise<void>((resolve) => { resolvePing = resolve; }),
            controlled = await wsServer((peer) => {
                peer.once('ping', () => { resolvePing(); });
            }),
            closed = { value: false },
            dialed = dial(client, `ws://127.0.0.1:${controlled.port}/`, {
                close: () => { closed.value = true; },
                idleTimeout: 8,
                sendPingsAutomatically: true
            }),
            ws = await dialed.opened;

        await serverPing;
        await sleep(12000);

        expect(closed.value).toBe(false);

        ws.close();
        await dialed.closed;
        await controlled.close();
    }, 20000);
});

describe('outbound compression', () => {
    it('round trips with the shared compressor', async () => {
        let payload = 'x'.repeat(4096),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo-shared`, { compression: SHARED_COMPRESSOR }),
            ws = await dialed.opened;

        ws.send(payload);

        expect(text((await dialed.next()).data)).toBe(payload);

        ws.close();
        await dialed.closed;
    });

    it('round trips with the dedicated compressor', async () => {
        let payload = 'x'.repeat(4096),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo-dedicated`, { compression: DEDICATED_COMPRESSOR }),
            ws = await dialed.opened;

        ws.send(payload);

        expect(text((await dialed.next()).data)).toBe(payload);

        ws.close();
        await dialed.closed;
    });

    it('round trips uncompressed when the server offers none', async () => {
        let payload = 'y'.repeat(2048),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { compression: SHARED_COMPRESSOR }),
            ws = await dialed.opened;

        ws.send(payload);

        expect(text((await dialed.next()).data)).toBe(payload);

        ws.close();
        await dialed.closed;
    });
});

describe('outbound TLS', () => {
    it('opens when verification is disabled', async () => {
        let tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            secure = SSLClient({ reject_unauthorized: false }),
            dialed = dial(secure, `wss://127.0.0.1:${tlsServer.port}/`),
            ws = await dialed.opened;

        ws.send('over-tls');

        expect(text((await dialed.next()).data)).toBe('over-tls');

        secure.close();
        tlsServer.close();
    }, 5000);

    it('fails verification against an untrusted certificate', async () => {
        let tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            strict = SSLClient(),
            dialed = dial(strict, `wss://127.0.0.1:${tlsServer.port}/`),
            error = await dialed.failed;

        expect(error.code).toBe('TLS_VERIFY');

        strict.close();
        tlsServer.close();
    }, 5000);

    it('honors a per-connect rejectUnauthorized:false on a verifying client', async () => {
        let tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            secure = SSLClient(),
            dialed = dial(secure, `wss://127.0.0.1:${tlsServer.port}/`, { rejectUnauthorized: false }),
            ws = await dialed.opened;

        ws.send('per-socket-off');

        expect(text((await dialed.next()).data)).toBe('per-socket-off');

        secure.close();
        tlsServer.close();
    }, 5000);

    it('honors a per-connect rejectUnauthorized:true on a non-verifying client', async () => {
        let tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            lenient = SSLClient({ reject_unauthorized: false }),
            dialed = dial(lenient, `wss://127.0.0.1:${tlsServer.port}/`, { rejectUnauthorized: true }),
            error = await dialed.failed;

        expect(error.code).toBe('TLS_VERIFY');

        lenient.close();
        tlsServer.close();
    }, 5000);
});

describe('outbound proxy', () => {
    it('tunnels ws through a per-connect proxy', async () => {
        let proxy = await startConnectProxy(),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { proxy: proxy.url }),
            ws = await dialed.opened;

        ws.send('via-proxy');
        expect(text((await dialed.next()).data)).toBe('via-proxy');
        expect(proxy.seen.url).toBe(`127.0.0.1:${server.port}`);

        ws.close();
        await dialed.closed;
        proxy.close();
    }, 5000);

    it('uses a Client-level default proxy', async () => {
        let proxy = await startConnectProxy(),
            proxied = Client({ proxy: proxy.url }),
            dialed = dial(proxied, `ws://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened;

        expect(proxy.seen.url).toBe(`127.0.0.1:${server.port}`);

        ws.close();
        await dialed.closed;
        proxied.close();
        proxy.close();
    }, 5000);

    it('per-connect proxy \'\' bypasses the client default', async () => {
        let proxy = await startConnectProxy(),
            proxied = Client({ proxy: proxy.url }),
            dialed = dial(proxied, `ws://127.0.0.1:${server.port}/echo`, { proxy: '' }),
            ws = await dialed.opened;

        expect(proxy.seen.url).toBeUndefined();

        ws.close();
        await dialed.closed;
        proxied.close();
        proxy.close();
    }, 5000);

    it('sends Proxy-Authorization from the URL userinfo', async () => {
        let proxy = await startConnectProxy({ auth: 'u:p' }),
            withAuth = proxy.url.replace('http://', 'http://u:p@'),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { proxy: withAuth }),
            ws = await dialed.opened;

        expect(proxy.seen.authorization).toBe('Basic dTpw');

        ws.close();
        await dialed.closed;
        proxy.close();
    }, 5000);

    it('fails with PROXY_AUTH when auth is required but absent', async () => {
        let proxy = await startConnectProxy({ auth: 'u:p' }),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { proxy: proxy.url }),
            error = await dialed.failed;

        expect(error.code).toBe('PROXY_AUTH');
        expect(error.status).toBe(407);
        proxy.close();
    }, 5000);

    it('fails with PROXY_STATUS on a non-200 CONNECT', async () => {
        let proxy = await startConnectProxy({ respond: '503 Service Unavailable' }),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { proxy: proxy.url }),
            error = await dialed.failed;

        expect(error.code).toBe('PROXY_STATUS');
        expect(error.status).toBe(503);
        proxy.close();
    }, 5000);

    it('fails with PROXY_STATUS when CONNECT 200 has trailing bytes', async () => {
        let proxy = await startConnectProxy({ respond: '200 OK\r\n\r\nGARBAGE' }),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { proxy: proxy.url }),
            error = await dialed.failed;

        expect(error.code).toBe('PROXY_STATUS');
        expect(error.status).toBe(200);
        proxy.close();
    }, 5000);

    it('tunnels wss through a proxy', async () => {
        let proxy = await startConnectProxy(),
            tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            secure = SSLClient({ reject_unauthorized: false, proxy: proxy.url }),
            dialed = dial(secure, `wss://127.0.0.1:${tlsServer.port}/`),
            ws = await dialed.opened;

        ws.send('secure-proxy');
        expect(text((await dialed.next()).data)).toBe('secure-proxy');

        ws.close();
        await dialed.closed;
        secure.close();
        tlsServer.close();
        proxy.close();
    }, 5000);

    it('throws URL_INVALID for a proxy without a port', () => {
        expect(() => Client({ proxy: 'http://127.0.0.1' })).toThrow();
    });

    it('throws URL_INVALID for a non-http proxy scheme', () => {
        expect(() => client.connect(`ws://127.0.0.1:${server.port}/echo`, { proxy: 'https://127.0.0.1:8080' })).toThrow();
    });
});
