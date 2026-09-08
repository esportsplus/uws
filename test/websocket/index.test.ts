import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { App, DEDICATED_COMPRESSOR, DEDICATED_DECOMPRESSOR, SHARED_COMPRESSOR, SSLApp } from '../../src/index';
import { closed, connect, handshake, listen, loopback, message, raw, sleep, text } from '../harness';
import type { TemplatedApp, WebSocket as UwsWebSocket } from '../../src/index';
import type { Server } from '../harness';

import WebSocket from 'ws';


type UserData = {
    id: number;
    tag: string;
};


const FLOOD = Buffer.alloc(64 * 1024, 7);

const TLS_OPTIONS = { cert_file_name: '.tmp/cert.pem', key_file_name: '.tmp/key.pem', passphrase: '1234' };

const SYMBOL = Symbol('sym');

const captured: Record<string, unknown> = {};

const events: string[] = [];

const filterEvents: number[] = [];


let server: Server;


function build(): TemplatedApp {
    let app = App();

    app.filter((_res, count) => {
        filterEvents.push(count);
    });
    app.ws('/echo', {
        close: (_ws, code, reason) => {
            events.push(`close:${code}:${text(reason)}`);
        },
        message: (ws, data, isBinary) => {
            if (!isBinary && text(data) === 'ping-me') {
                captured.pingStatus = ws.ping('yo');
                return;
            }

            captured.sendStatus = ws.send(data, isBinary);
        },
        ping: (_ws, data) => {
            events.push(`ping:${text(data)}`);
        },
        pong: (_ws, data) => {
            events.push(`pong:${text(data)}`);
        }
    });
    app.ws<UserData>('/upgrade', {
        open: (ws) => {
            let user = ws.getUserData() as UserData & Record<PropertyKey, unknown>,
                raw = ws as unknown as Record<PropertyKey, unknown>;

            captured.upgraded = { id: user.id, mergedOntoWs: 'id' in raw, same: (user as unknown) === ws, sym: user[SYMBOL], tag: user.tag };
        },
        upgrade: (res, req, context) => {
            res.upgrade<UserData>(
                { id: 7, [SYMBOL]: 'sym', tag: req.getUrl() } as UserData,
                req.getHeader('sec-websocket-key'),
                req.getHeader('sec-websocket-protocol'),
                req.getHeader('sec-websocket-extensions'),
                context
            );
        }
    });
    app.ws('/async-upgrade', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        },
        upgrade: (res, req, context) => {
            let aborted = false,
                extensions = req.getHeader('sec-websocket-extensions'),
                key = req.getHeader('sec-websocket-key'),
                protocol = req.getHeader('sec-websocket-protocol');

            res.onAborted(() => {
                aborted = true;
            });
            setTimeout(() => {
                if (aborted) {
                    return;
                }

                res.cork(() => {
                    res.upgrade({}, key, protocol, extensions, context);
                });
            }, 50);
        }
    });
    app.ws('/reject', {
        upgrade: (res) => {
            res.writeStatus('403 Forbidden').end('no');
        }
    });
    app.ws('/invalid-upgrade', {
        upgrade: (res, _req, context) => {
            try {
                res.upgrade({}, undefined as unknown as string, '', '', context);
            }
            catch (error) {
                captured.invalidUpgradeError = (error as Error).message;
                res.writeStatus('400 Bad Request').end();
            }
        }
    });
    app.ws('/bad-context', {
        upgrade: (res, req) => {
            try {
                res.upgrade({}, req.getHeader('sec-websocket-key'), req.getHeader('sec-websocket-protocol'), req.getHeader('sec-websocket-extensions'), undefined as never);
            }
            catch (error) {
                captured.badContext = (error as Error).message;
                res.writeStatus('400').end('bad');
            }
        }
    });
    app.ws('/big-ping', {
        open: (ws) => {
            try {
                ws.ping(Buffer.alloc(200));
            }
            catch (error) {
                captured.bigPing = (error as Error).message;
                ws.send('caught');
            }
        },
        message: (ws) => {
            try {
                ws.ping(Buffer.alloc(50));
                captured.smallPing = true;
            }
            catch (error) {
                captured.smallPing = (error as Error).message;
            }
        }
    });
    app.ws('/small', {
        close: (_ws, code, reason) => {
            events.push(`small:${code}:${text(reason)}`);
        },
        maxPayloadLength: 1024
    });
    app.ws('/idle', {
        close: (_ws, code, reason) => {
            events.push(`idle:${code}:${text(reason)}`);
        },
        idleTimeout: 8,
        sendPingsAutomatically: false
    });
    app.ws('/keepalive', {
        idleTimeout: 8,
        sendPingsAutomatically: true
    });
    app.ws('/idle-disabled', {
        idleTimeout: 0
    });
    app.ws('/idle-disabled-end', {
        idleTimeout: 0,
        close: (_ws, code, reason) => {
            events.push(`idle-disabled-end:${code}:${text(reason)}`);
        },
        open: (ws) => {
            setTimeout(() => {
                ws.end();
            }, 50);
        }
    });
    app.ws('/shared', {
        compression: SHARED_COMPRESSOR,
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary, true);
        }
    });
    app.ws('/dedicated', {
        compression: DEDICATED_COMPRESSOR,
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary, true);
        }
    });
    app.ws('/bomb', {
        compression: DEDICATED_DECOMPRESSOR,
        maxPayloadLength: 16 * 1024,
        close: (_ws, code, reason) => {
            events.push(`bomb:${code}:${text(reason)}`);
        }
    });
    app.ws('/compress-edge', {
        compression: DEDICATED_COMPRESSOR,
        message: (ws) => {
            captured.emptyCompressed = ws.send('', false, true);
            // ping has no compression flag; control frames stay uncompressed.
            captured.controlCompressed = ws.ping('');
        },
        pong: (ws) => { ws.send('pong received'); }
    });
    app.ws('/lifetime', {
        maxLifetime: 1,
        close: (_ws, code, reason) => {
            events.push(`lifetime:${code}:${text(reason)}`);
        }
    });
    app.ws('/end-invalid', {
        message: (ws, data) => {
            ws.end(Number(text(data)));
        }
    });
    app.ws('/end-symbol', {
        open: (ws) => {
            try {
                ws.end(Symbol() as never);
            }
            catch (error) {
                captured.wsEndSymbol = (error as Error).message;
                ws.end(1000);
            }
        }
    });
    app.ws('/end-twice', {
        open: (ws) => {
            ws.end();
            try {
                ws.end();
            }
            catch (error) {
                captured.endTwice = error;
            }
        }
    });
    app.ws('/close-in-open', {
        open: (ws) => {
            // Closing in open is valid; subsequent access must throw.
            ws.close();
            try { ws.close(); }
            catch (error) { captured.closeInOpen = error; }
            try { ws.send('after'); }
            catch (error) { captured.sendAfterClose = error; }
        }
    });
    app.ws('/end-in-close', {
        close: (ws) => {
            try {
                ws.end();
            }
            catch (error) {
                captured.endInClose = error;
            }
        }
    });
    app.ws('/end-in-subscription', {
        close: () => {
            captured.endInSubscriptionCloses = ((captured.endInSubscriptionCloses as number | undefined) ?? 0) + 1;
        },
        message: (ws) => {
            ws.subscribe('doomed');
            ws.send('subscribed');
        },
        subscription: (ws, _topic, newCount, oldCount) => {
            if (newCount < oldCount) {
                captured.endInSubscriptionEvents = ((captured.endInSubscriptionEvents as number | undefined) ?? 0) + 1;
                try {
                    ws.end(4002, 'from subscription');
                }
                catch (error) {
                    captured.endInSubscriptionError = (error as Error).message;
                }
            }
        }
    });
    app.ws('/unlimited-backpressure', {
        drain: (ws) => {
            captured.unlimitedDrained = ws.getBufferedAmount();
        },
        dropped: () => {
            captured.unlimitedDropped = true;
        },
        maxBackpressure: 0,
        message: (ws) => {
            captured.unlimitedStatuses = flood(ws);
            captured.unlimitedBuffered = ws.getBufferedAmount();
        }
    });
    app.ws('/throwing-upgrade', {
        upgrade: () => {
            throw new Error('upgrade test exception');
        }
    });
    app.ws('/status-before-upgrade', {
        upgrade: (res, req, context) => {
            res.writeStatus('403 Forbidden');
            res.upgrade({}, req.getHeader('sec-websocket-key'), req.getHeader('sec-websocket-protocol'), req.getHeader('sec-websocket-extensions'), context);
        }
    });
    app.ws('/idle-close-other', {
        idleTimeout: 8,
        sendPingsAutomatically: false,
        close: (ws) => {
            let other = captured.idleOther as UwsWebSocket<unknown> | undefined;

            if (other && other !== ws) {
                captured.idleOther = undefined;
                other.close();
            }
        },
        open: (ws) => {
            captured.idleOther ??= ws;
        }
    });
    app.ws('/large', {
        maxPayloadLength: 16 * 1024 * 1024,
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    });
    app.ws('/stored-after-close', {
        open: (ws) => {
            captured.stored = ws;
        },
        message: (ws) => {
            ws.close();
            try {
                (captured.stored as UwsWebSocket<unknown>).send('after');
            }
            catch (error) {
                captured.storedAfterClose = (error as Error).message;
            }
        }
    });
    app.ws('/backpressure', {
        drain: (ws) => {
            events.push('drain');

            if (!captured.drained) {
                captured.drained = true;
                ws.send('drained');
            }
        },
        dropped: () => {
            captured.dropped = ((captured.dropped as number | undefined) ?? 0) + 1;
        },
        maxBackpressure: 1024,
        message: (ws) => {
            captured.statuses = flood(ws);
        }
    });
    app.ws('/closelimit', {
        close: (_ws, code, reason) => {
            events.push(`closelimit:${code}:${text(reason)}`);
        },
        closeOnBackpressureLimit: true,
        maxBackpressure: 1024,
        message: (ws) => {
            flood(ws);
        }
    });
    app.ws('/server-close', {
        close: (_ws, code, reason) => {
            events.push(`server-close:${code}:${text(reason)}`);
        },
        message: (ws, data) => {
            if (text(data) === 'end') {
                ws.end(4001, 'server bye');
            }
            else {
                ws.close();
            }

            try {
                ws.send('after');
            }
            catch (error) {
                captured.afterClose = (error as Error).message;
            }
        }
    });
    app.ws('/fragments', {
        open: (ws) => {
            captured.fragmentStatuses = [
                ws.sendFirstFragment('a', false),
                ws.sendFragment('b'),
                ws.sendLastFragment('c'),
                ws.sendFirstFragment(new Uint8Array([1, 2]), true),
                ws.sendFragment(new Uint8Array([3])),
                ws.sendLastFragment(new Uint8Array([4]))
            ];
        }
    });
    app.ws('/remote', {
        open: (ws) => {
            ws.send(JSON.stringify({ address: ws.getRemoteAddress().byteLength, port: ws.getRemotePort(), text: ws.getRemoteAddressAsText() }));
        }
    });
    app.ws('/cork', {
        open: (ws) => {
            captured.corkReturn = ws.cork(() => {
                ws.send('a');
                ws.send('b');
            }) === ws;
        }
    });
    app.ws('/detached-buffers', {
        open: (ws) => {
            ws.subscribe('detached-buffers');
        },
        subscription: (_ws, topic) => {
            captured.subscriptionTopic = topic;
        },
        ping: (ws, data) => {
            captured.pingPayload = data;
            ws.send('ping-captured');
        },
        pong: (ws, data) => {
            captured.pongPayload = data;
            ws.send('pong-captured');
        },
        message: (ws, data) => {
            if (text(data) === 'send-ping') {
                ws.ping('pong payload');
            }
        }
    });

    return app;
}

function flood(ws: UwsWebSocket<unknown>): number[] {
    let statuses: number[] = [];

    for (let i = 0; i < 4000; i++) {
        let status = ws.send(FLOOD, true);

        statuses.push(status);

        if (status === 2) {
            break;
        }
    }

    return statuses;
}

async function bounded<T>(promise: Promise<T>, label: string, ms = 3000): Promise<T> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
        return await Promise.race([promise, new Promise<never>((_resolve, reject) => {
            timer = setTimeout(() => reject(new Error(`Timed out: ${label}`)), ms);
        })]);
    }
    finally { clearTimeout(timer); }
}

async function until(predicate: () => boolean, label: string, ms = 3000): Promise<void> {
    let deadline = Date.now() + ms;
    while (!predicate() && Date.now() < deadline) { await sleep(20); }
    expect(predicate(), label).toBe(true);
}

function socketOf(ws: WebSocket): { pause: () => void; resume: () => void } {
    return (ws as unknown as { _socket: { pause: () => void; resume: () => void } })._socket;
}


beforeAll(() => {
    server = listen(build());
});

afterAll(() => {
    server.close();
});


describe('throwing option getters', () => {
    it('throws when App ws maxPayloadLength getter throws', () => {
        let app = App();

        try {
            expect(() => app.ws('/x', {
                get maxPayloadLength(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when App ws idleTimeout getter throws', () => {
        let app = App();

        try {
            expect(() => app.ws('/x', {
                get idleTimeout(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when App ws maxLifetime getter throws', () => {
        let app = App();

        try {
            expect(() => app.ws('/x', {
                get maxLifetime(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when App ws open getter throws', () => {
        let app = App();

        try {
            expect(() => app.ws('/x', {
                get open(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when SSLApp ws maxPayloadLength getter throws', () => {
        let app = SSLApp(TLS_OPTIONS);

        try {
            expect(() => app.ws('/x', {
                get maxPayloadLength(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when SSLApp ws idleTimeout getter throws', () => {
        let app = SSLApp(TLS_OPTIONS);

        try {
            expect(() => app.ws('/x', {
                get idleTimeout(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when SSLApp ws maxLifetime getter throws', () => {
        let app = SSLApp(TLS_OPTIONS);

        try {
            expect(() => app.ws('/x', {
                get maxLifetime(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });

    it('throws when SSLApp ws open getter throws', () => {
        let app = SSLApp(TLS_OPTIONS);

        try {
            expect(() => app.ws('/x', {
                get open(): never { throw new Error('boom'); }
            })).toThrow('boom');
        }
        finally {
            app.close();
        }
    });
});

describe('messages', () => {
    it('echoes text and binary messages and reports send status', async () => {
        let ws = await connect(`${server.url}/echo`),
            reply = message(ws);

        ws.send('hi');

        let received = await reply;

        expect(received.isBinary).toBe(false);
        expect(received.data.toString()).toBe('hi');
        expect(captured.sendStatus).toBe(1);

        reply = message(ws);
        ws.send(Buffer.from([1, 2, 3]));
        received = await reply;

        expect(received.isBinary).toBe(true);
        expect(Array.from(received.data)).toEqual([1, 2, 3]);
        ws.close();
    });

    it('reassembles fragmented client messages', async () => {
        let ws = await connect(`${server.url}/echo`),
            reply = message(ws);

        ws.send('a', { fin: false });
        ws.send('b', { fin: false });
        ws.send('c', { fin: true });

        expect((await reply).data.toString()).toBe('abc');
        ws.close();
    });

    it('sends fragmented server messages', async () => {
        let ws = await connect(`${server.url}/fragments`),
            first = await message(ws),
            second = await message(ws);

        expect(first.isBinary).toBe(false);
        expect(first.data.toString()).toBe('abc');
        expect(second.isBinary).toBe(true);
        expect(Array.from(second.data)).toEqual([1, 2, 3, 4]);
        expect(captured.fragmentStatuses).toEqual([1, 1, 1, 1, 1, 1]);
        ws.close();
    });

    it('corks multiple sends', async () => {
        let ws = await connect(`${server.url}/cork`),
            first = await message(ws),
            second = await message(ws);

        expect(first.data.toString()).toBe('a');
        expect(second.data.toString()).toBe('b');
        expect(captured.corkReturn).toBe(true);
        ws.close();
    });

    it('surfaces ping and pong handlers and answers pings automatically', async () => {
        let ws = await connect(`${server.url}/echo`),
            pong = new Promise<string>((resolve) => {
                ws.once('pong', (data) => resolve(data.toString()));
            });

        ws.ping('hi');

        expect(await pong).toBe('hi');
        await sleep(50);
        expect(events).toContain('ping:hi');

        let ping = new Promise<string>((resolve) => {
            ws.once('ping', (data) => resolve(data.toString()));
        });

        ws.send('ping-me');

        expect(await ping).toBe('yo');
        expect(captured.pingStatus).toBe(1);
        await sleep(50);
        expect(events).toContain('pong:yo');
        ws.close();
    });

    it('throws for oversized pings and still sends valid ping payloads', async () => {
        let ws = await connect(`${server.url}/big-ping`, { handshakeTimeout: 3000 });
        try {
            expect((await bounded(message(ws), 'oversized ping caught')).data.toString()).toBe('caught');
            expect(captured.bigPing).toContain('125');

            let ping = new Promise<Buffer>((resolve) => {
                ws.once('ping', resolve);
            });

            ws.send('ping-me');

            expect((await bounded(ping, 'valid 50-byte ping')).equals(Buffer.alloc(50))).toBe(true);
            expect(captured.smallPing).toBe(true);
        }
        finally { ws.terminate(); }
    }, 10000);

    it('detaches subscription, ping, and pong handler buffers after their callbacks', async () => {
        let ws = await connect(`${server.url}/detached-buffers`);

        expect((captured.subscriptionTopic as ArrayBuffer).byteLength).toBe(0);

        let pingCaptured = message(ws);
        ws.ping('ping payload');
        expect((await pingCaptured).data.toString()).toBe('ping-captured');
        expect((captured.pingPayload as ArrayBuffer).byteLength).toBe(0);

        let pongCaptured = message(ws);
        ws.send('send-ping');
        expect((await pongCaptured).data.toString()).toBe('pong-captured');
        expect((captured.pongPayload as ArrayBuffer).byteLength).toBe(0);
        ws.close();
    });
});

describe('upgrades', () => {
    it('reports an HTTP socket disconnection when upgrading to WebSocket', async () => {
        filterEvents.length = 0;

        let ws = await connect(`${server.url}/echo`),
            done = closed(ws);

        ws.close();
        await done;

        expect(filterEvents).toEqual([1, -1]);
    });

    it('exposes upgrade user data as a separate object via getUserData()', async () => {
        let ws = await connect(`${server.url}/upgrade?x=1`);

        await sleep(50);
        expect(captured.upgraded).toEqual({ id: 7, mergedOntoWs: false, same: false, sym: 'sym', tag: '/upgrade' });
        ws.close();
    });

    it('upgrades asynchronously from a corked callback', async () => {
        let ws = await connect(`${server.url}/async-upgrade`),
            reply = message(ws);

        ws.send('later');

        expect((await reply).data.toString()).toBe('later');
        ws.close();
    });

    it('lets the upgrade handler answer with a plain http response', async () => {
        let ws = new WebSocket(`${server.url}/reject`),
            status = await new Promise<number>((resolve) => {
                ws.on('error', () => {});
                ws.once('unexpected-response', (_req, res) => {
                    resolve(res.statusCode ?? 0);
                });
            });

        expect(status).toBe(403);
    });

    it('throws for an invalid upgrade key and can still send an http response', async () => {
        let ws = new WebSocket(`${server.url}/invalid-upgrade`),
            status = await new Promise<number>((resolve) => {
                ws.on('error', () => {});
                ws.once('unexpected-response', (_req, res) => {
                    resolve(res.statusCode ?? 0);
                });
            });

        expect(status).toBe(400);
        expect(captured.invalidUpgradeError).toBe('Sec-WebSocket-Key must be exactly 24 bytes.');
    });

    it('rejects an invalid upgrade context and keeps serving WebSockets', async () => {
        let response = await raw(server.port, [
                'GET /bad-context HTTP/1.1', 'Host: localhost', 'Connection: Upgrade',
                'Upgrade: websocket', 'Sec-WebSocket-Version: 13',
                'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', '', ''
            ].join('\r\n')),
            headers = response.data.toString().split('\r\n\r\n')[0];

        expect(headers.startsWith('HTTP/1.1 400')).toBe(true);
        expect(headers.startsWith('HTTP/1.1 101')).toBe(false);
        expect(captured.badContext).toMatch(/WebSocketContext|External/);

        let ws = await connect(`${server.url}/echo`, { handshakeTimeout: 3000 });
        try {
            expect(ws.readyState).toBe(WebSocket.OPEN);
            let reply = message(ws);
            ws.send('healthy');
            expect((await bounded(reply, 'echo after invalid upgrade context')).data.toString()).toBe('healthy');
        }
        finally { ws.terminate(); }
    }, 10000);

    it('selects the first offered subprotocol', async () => {
        let ws = await connect(`${server.url}/echo`, { protocols: ['chat', 'other'] });

        expect(ws.protocol).toBe('chat');
        ws.close();
    });

    it('serves http on the same app', async () => {
        let ws = new WebSocket(`${server.url}/nope`),
            status = await new Promise<number>((resolve) => {
                ws.on('error', () => {});
                ws.once('unexpected-response', (_req, res) => {
                    resolve(res.statusCode ?? 0);
                });
            });

        expect(status).toBe(404);
    });
});

describe('closing', () => {
    it('throws for a Symbol end code and then closes cleanly', async () => {
        // Attach before open: the server ends the socket in the upgrade tick.
        let ws = new WebSocket(`${server.url}/end-symbol`, { handshakeTimeout: 3000 }),
            done = closed(ws),
            opened = false;

        ws.on('error', () => {});
        ws.once('open', () => { opened = true; });
        try {
            expect((await bounded(done, 'close after Symbol end code')).code).toBe(1000);
            expect(opened).toBe(true);
            expect(captured.wsEndSymbol).toContain('number');
            expect(ws.readyState).toBe(WebSocket.CLOSED);
        }
        finally { ws.terminate(); }
    });

    it('delivers close code and reason from the client', async () => {
        let ws = await connect(`${server.url}/echo`),
            done = closed(ws);

        ws.close(4000, 'bye');

        expect(await done).toEqual({ code: 4000, reason: 'bye' });
        expect(events).toContain('close:4000:bye');
    });

    it('delivers close code and reason from the server', async () => {
        let ws = await connect(`${server.url}/server-close`),
            done = closed(ws);

        ws.send('end');

        expect(await done).toEqual({ code: 4001, reason: 'server bye' });
        expect(events).toContain('server-close:4001:server bye');
        expect(captured.afterClose).toBe('Invalid access of closed uWS.WebSocket/SSLWebSocket.');
    });

    it('closes abruptly with 1006 on ws.close()', async () => {
        let ws = await connect(`${server.url}/server-close`),
            done = closed(ws);

        ws.send('close');

        expect((await done).code).toBe(1006);
        expect(events).toContain('server-close:1006:');
    });

    it('closes oversized messages with 1006', async () => {
        let ws = await connect(`${server.url}/small`),
            done = closed(ws);

        ws.send(Buffer.alloc(2048));

        expect((await done).code).toBe(1006);
        expect(events).toContain('small:1006:Received too big message');
    });

    it('closes invalid utf-8 text with 1006', async () => {
        let ws = await connect(`${server.url}/echo`),
            done = closed(ws);

        ws.send(Buffer.from([0xff, 0xfe]), { binary: false });

        expect((await done).code).toBe(1006);
        expect(events).toContain('close:1006:Received invalid UTF-8');
    });

    it('closes idle sockets after the idle timeout', async () => {
        let ws = await connect(`${server.url}/idle`),
            started = Date.now(),
            done = await closed(ws),
            elapsed = Date.now() - started;

        expect(done.code).toBe(1006);
        expect(events).toContain('idle:1006:WebSocket timed out from inactivity');
        expect(elapsed).toBeGreaterThanOrEqual(4000);
        expect(elapsed).toBeLessThan(16000);
    }, 20000);

    it('sends pings automatically to keep idle sockets alive', async () => {
        let ws = await connect(`${server.url}/keepalive`),
            ping = new Promise<void>((resolve) => {
                ws.once('ping', () => resolve());
            });

        await ping;
        await sleep(5000);

        expect(ws.readyState).toBe(WebSocket.OPEN);
        ws.close();
    }, 20000);

    it('handles invalid end codes and invalid WebSocket access without crashing', async () => {
        let clients: WebSocket[] = [];
        const healthy = async (): Promise<void> => {
            let ws = await connect(`${server.url}/echo`, { handshakeTimeout: 3000 }), reply = message(ws);
            clients.push(ws);
            ws.send('still serving');
            expect((await bounded(reply, 'survival echo')).data.toString()).toBe('still serving');
            ws.terminate();
        };
        try {
            for (let code of [0, 1005, 1006, 65536, -1]) {
                let ws = await connect(`${server.url}/end-invalid`, { handshakeTimeout: 3000 }), done = closed(ws);
                clients.push(ws);
                ws.send(String(code));
                // Invalid wire codes may cause a client protocol error; require closure.
                await bounded(done, `close after end(${code})`);
                expect(ws.readyState).toBe(WebSocket.CLOSED);
                await healthy();
            }
            for (let path of ['end-twice', 'close-in-open', 'end-in-close']) {
                // Attach before open: the server can close in the upgrade tick.
                let ws = new WebSocket(`${server.url}/${path}`, { handshakeTimeout: 3000 }), done = closed(ws);
                clients.push(ws);
                ws.on('error', () => {});
                if (path === 'end-in-close') { ws.once('open', () => ws.close()); }
                await bounded(done, path);
            }
            await until(() => captured.endInClose !== undefined, 'server close callback');
            for (let key of ['endTwice', 'closeInOpen', 'sendAfterClose', 'endInClose']) {
                expect(captured[key]).toBeInstanceOf(Error);
                expect((captured[key] as Error).message).toContain('Invalid access');
            }
            await healthy();
        }
        finally { for (let ws of clients) { ws.terminate(); } }
    }, 30000);

    it('survives ws.end() from a close-time subscription handler after an abrupt disconnect', async () => {
        let ws = await connect(`${server.url}/end-in-subscription`, { handshakeTimeout: 3000 }),
            reply = message(ws);

        try {
            ws.send('go');
            expect((await bounded(reply, 'subscribed')).data.toString()).toBe('subscribed');
            ws.terminate();
            await until(() => captured.endInSubscriptionCloses !== undefined, 'server close callback');
            await sleep(50);
            expect(captured.endInSubscriptionCloses).toBe(1);
            expect(captured.endInSubscriptionEvents).toBe(1);
            expect(captured.endInSubscriptionError).toBeUndefined();
        }
        finally { ws.terminate(); }
    }, 10000);

    it('throws on same-tick send after close from a stored WebSocket reference', async () => {
        let ws = await connect(`${server.url}/stored-after-close`),
            done = closed(ws);

        ws.send('close');

        expect((await done).code).toBe(1006);
        expect(captured.storedAfterClose).toBe('Invalid access of closed uWS.WebSocket/SSLWebSocket.');
    });
});

describe.skipIf(!process.env.UWS_SLOW_TESTS).concurrent('long WebSocket timeouts', () => {
    it('does not ping or close idle sockets', async () => {
        let ws = await connect(`${server.url}/idle-disabled`),
            pinged = false;

        ws.on('ping', () => {
            pinged = true;
        });

        await sleep(20000);

        expect(pinged).toBe(false);
        expect(ws.readyState).toBe(WebSocket.OPEN);
        ws.close();
    }, 30000);

    it('still closes a socket after end when the peer does not reply', async () => {
        let eventIndex = events.length,
            ws = await connect(`${server.url}/idle-disabled-end`);

        socketOf(ws).pause();

        for (let i = 0; i < 48 && !events.slice(eventIndex).some((event) => event.startsWith('idle-disabled-end:')); i++) {
            await sleep(250);
        }

        expect(events.slice(eventIndex).some((event) => event.startsWith('idle-disabled-end:'))).toBe(true);
        ws.terminate();
    }, 20000);

    it('asks clients to reconnect after one minute', async () => {
        let ws = await connect(`${server.url}/lifetime`),
            done = await closed(ws);

        expect(done).toEqual({ code: 1000, reason: 'please reconnect' });
        expect(events).toContain('lifetime:1000:please reconnect');
    }, 130000);
});

describe('compression', () => {
    it('negotiates the shared compressor', async () => {
        let { headers, ws } = await handshake(`${server.url}/shared`, { perMessageDeflate: true }),
            payload = 'x'.repeat(10000),
            reply = message(ws);

        expect(headers['sec-websocket-extensions']).toBe('permessage-deflate; client_no_context_takeover; server_no_context_takeover');
        ws.send(payload);

        expect((await reply).data.toString()).toBe(payload);
        ws.close();
    });

    it('negotiates the dedicated compressor', async () => {
        let { headers, ws } = await handshake(`${server.url}/dedicated`, { perMessageDeflate: true }),
            payload = Buffer.alloc(10000, 3),
            reply = message(ws);

        expect(headers['sec-websocket-extensions']).toBe('permessage-deflate; client_no_context_takeover');
        ws.send(payload);

        let received = await reply;

        expect(received.isBinary).toBe(true);
        expect(received.data.equals(payload)).toBe(true);
        ws.close();
    });

    it('skips compression when the client does not offer it', async () => {
        let { headers, ws } = await handshake(`${server.url}/shared`, { perMessageDeflate: false }),
            reply = message(ws);

        expect(headers['sec-websocket-extensions']).toBeUndefined();
        ws.send('plain');

        expect((await reply).data.toString()).toBe('plain');
        ws.close();
    });

    it('rejects a compressed message that inflates beyond maxPayloadLength', async () => {
        let ws = await connect(`${server.url}/bomb`, { perMessageDeflate: true }),
            done = closed(ws);

        ws.send(Buffer.alloc(1024 * 1024, 7), { compress: true });

        expect((await done).code).toBe(1006);
    });

    it('pins permessage-deflate extension negotiation edge cases', async () => {
        // Use raw offers: ws generates its own extension header when deflate is enabled.
        for (let [offer, token] of [
            ['permessage-deflate; server_max_window_bits=8', 'permessage-deflate'],
            ['permessage-deflate; client_no_context_takeover', 'permessage-deflate'],
            ['x-webkit-deflate-frame', 'x-webkit-deflate-frame'],
            ['not an extension; = garbage', undefined]
        ] as const) {
            let response = await raw(server.port, [
                    'GET /dedicated HTTP/1.1', 'Host: localhost', 'Connection: Upgrade',
                    'Upgrade: websocket', 'Sec-WebSocket-Version: 13',
                    'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==',
                    `Sec-WebSocket-Extensions: ${offer}`, '', ''
                ].join('\r\n')),
                headers = response.data.toString().split('\r\n\r\n')[0],
                extension = /^sec-websocket-extensions:\s*(.*)$/im.exec(headers)?.[1];
            expect(headers.startsWith('HTTP/1.1 101')).toBe(true);
            if (token === undefined) { expect(extension).toBeUndefined(); }
            else { expect(extension?.includes(token)).toBe(true); }
        }
    }, 15000);

    it('does not compress empty data or control frames when compression is requested', async () => {
        let { headers, ws } = await handshake(`${server.url}/compress-edge`, {
            perMessageDeflate: true, handshakeTimeout: 3000
        });
        try {
            let empty = message(ws), ping = new Promise<Buffer>((resolve) => { ws.once('ping', resolve); });
            expect(String(headers['sec-websocket-extensions']).includes('permessage-deflate')).toBe(true);
            ws.send('', { compress: true });
            let [received, control] = await bounded(Promise.all([empty, ping]), 'empty echo and ping');
            expect(received.data).toHaveLength(0);
            expect(received.isBinary).toBe(false);
            expect(control).toHaveLength(0);
            expect((await bounded(message(ws), 'server received automatic pong')).data.toString()).toBe('pong received');
            expect(captured.emptyCompressed).toBe(1);
            expect(captured.controlCompressed).toBe(1);
        }
        finally { ws.terminate(); }
    }, 10000);
});


describe('backpressure', () => {
    it('reports backpressure, drops messages over the limit and drains', async () => {
        let ws = await connect(`${server.url}/backpressure`),
            drained = new Promise<void>((resolve) => {
                ws.on('message', (data, isBinary) => {
                    if (!isBinary && data.toString() === 'drained') {
                        resolve();
                    }
                });
            });

        socketOf(ws).pause();
        ws.send('flood');
        await sleep(500);
        socketOf(ws).resume();
        await drained;

        let statuses = captured.statuses as number[];

        expect(statuses[0]).toBe(1);
        expect(statuses).toContain(0);
        expect(statuses.at(-1)).toBe(2);
        expect(captured.dropped).toBe(1);
        expect(events).toContain('drain');
        ws.close();
    }, 15000);

    it('closes the socket when closeOnBackpressureLimit is set', async () => {
        let ws = await connect(`${server.url}/closelimit`),
            done = closed(ws);

        socketOf(ws).pause();
        ws.send('flood');
        await sleep(500);
        socketOf(ws).resume();

        expect((await done).code).toBe(1006);
        expect(events.some((event) => event.startsWith('closelimit:1006:'))).toBe(true);
    }, 15000);

    it('does not drop with maxBackpressure zero and drains fully', async () => {
        captured.unlimitedDropped = false;
        captured.unlimitedDrained = undefined;

        let ws = await connect(`${server.url}/unlimited-backpressure`);

        socketOf(ws).pause();
        ws.send('flood');
        await sleep(250);
        expect(captured.unlimitedBuffered as number).toBeGreaterThan(0);
        socketOf(ws).resume();

        for (let i = 0; i < 40 && captured.unlimitedDrained !== 0; i++) {
            await sleep(50);
        }

        expect(captured.unlimitedStatuses as number[]).not.toContain(2);
        expect(captured.unlimitedDropped).toBe(false);
        expect(captured.unlimitedDrained).toBe(0);
        ws.close();
    }, 15000);

    it.skipIf(!process.env.UWS_SLOW_TESTS)('handles SSL backpressure and compression', async () => {
        let buffered = 0, drained: number | undefined, statuses: number[] = [],
            tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                compression: DEDICATED_COMPRESSOR,
                maxBackpressure: 1024,
                maxPayloadLength: 16 * 1024 * 1024,
                drain: (ws) => { drained = ws.getBufferedAmount(); },
                message: (ws, data, isBinary) => {
                    if (!isBinary && text(data) === 'flood') {
                        statuses = flood(ws);
                        buffered = ws.getBufferedAmount();
                    }
                    else { ws.send(data, isBinary, true); }
                }
            }), { secure: true }), clients: WebSocket[] = [];
        const dial = async (): Promise<WebSocket> => {
            let ws = await connect(`wss://127.0.0.1:${tlsServer.port}/`, {
                perMessageDeflate: true, rejectUnauthorized: false, handshakeTimeout: 3000
            });
            clients.push(ws);
            return ws;
        };
        const echo = async (ws: WebSocket): Promise<void> => {
            let payload = 'compressed TLS echo '.repeat(1000), reply = message(ws);
            expect(ws.extensions.includes('permessage-deflate')).toBe(true);
            ws.send(payload, { compress: true });
            expect((await bounded(reply, 'compressed TLS echo')).data.toString()).toBe(payload);
        };
        try {
            let paused = await dial();
            socketOf(paused).pause();
            paused.send('flood');
            await until(() => buffered > 0, 'TLS backpressure builds');
            expect(statuses).toContain(0);
            await echo(await dial());
            socketOf(paused).resume();
            await until(() => drained === 0, 'TLS backpressure drains', 5000);
            await echo(await dial());
        }
        finally {
            for (let ws of clients) { socketOf(ws).resume(); ws.terminate(); }
            tlsServer.close();
        }
    }, 20000);
});

describe('upgrade failures', () => {
    it('keeps a throwing upgrade connection unopened and keeps serving', async () => {
        let uncaught: Error[] = [],
            onUncaught = (error: Error): void => { uncaught.push(error); },
            opened = false, failed: WebSocket | undefined, healthy: WebSocket | undefined;
        process.on('uncaughtException', onUncaught);
        try {
            failed = new WebSocket(`${server.url}/throwing-upgrade`);
            failed.on('error', () => {});
            failed.on('open', () => { opened = true; });
            await until(() => uncaught.length > 0, 'upgrade exception');
            // A throwing upgrade handler relies on the idle timeout to close the connection.
            await sleep(250);
            expect(opened).toBe(false);
            expect(uncaught.map((error) => error.message)).toEqual(['upgrade test exception']);
            healthy = await connect(`${server.url}/echo`, { handshakeTimeout: 3000 });
            let reply = message(healthy);
            healthy.send('healthy');
            expect((await bounded(reply, 'echo after throwing upgrade')).data.toString()).toBe('healthy');
            expect(opened).toBe(false);
            expect(uncaught.map((error) => error.message)).toEqual(['upgrade test exception']);
        }
        finally {
            failed?.terminate();
            healthy?.terminate();
            process.off('uncaughtException', onUncaught);
        }
    }, 10000);

    it('pins the response when writeStatus precedes upgrade', async () => {
        let ws = new WebSocket(`${server.url}/status-before-upgrade`),
            status = await new Promise<number>((resolve) => {
                ws.on('error', () => {});
                ws.once('unexpected-response', (_request, response) => resolve(response.statusCode ?? 0));
            });

        expect(status).toBe(403);
    });
});

describe.skipIf(!process.env.UWS_SLOW_TESTS)('large messages', () => {
    it('reassembles a 16 MB message over plain and TLS WebSockets', async () => {
        let payload = Buffer.alloc(16 * 1024 * 1024, 9),
            plain = await connect(`${server.url}/large`),
            plainReply = message(plain),
            tlsServer = listen(SSLApp(TLS_OPTIONS).ws('/*', {
                maxPayloadLength: 16 * 1024 * 1024,
                message: (ws, data, isBinary) => { ws.send(data, isBinary); }
            }), { secure: true }),
            secure = await connect(`wss://127.0.0.1:${tlsServer.port}/`, { rejectUnauthorized: false }),
            secureReply = message(secure);

        plain.send(payload);
        secure.send(payload, { fin: false });
        secure.send(Buffer.alloc(0), { fin: true });

        expect((await plainReply).data.equals(payload)).toBe(true);
        expect((await secureReply).data.equals(payload)).toBe(true);
        plain.close();
        secure.close();
        tlsServer.close();
    }, 60000);
});

describe.skipIf(!process.env.UWS_SLOW_TESTS)('idle timeout iteration', () => {
    it('allows an idle close handler to close a different socket', async () => {
        let first = await connect(`${server.url}/idle-close-other`),
            second = await connect(`${server.url}/idle-close-other`),
            firstClosed = closed(first),
            secondClosed = closed(second);

        expect((await firstClosed).code).toBe(1006);
        expect((await secondClosed).code).toBe(1006);
    }, 20000);
});

describe('raw WebSocket frames', () => {
    it('reassembles a control frame split across TCP segments', async () => {
        let request = [
                'GET /echo HTTP/1.1',
                'Host: localhost',
                'Connection: Upgrade',
                'Upgrade: websocket',
                'Sec-WebSocket-Version: 13',
                'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==',
                '',
                ''
            ].join('\r\n'),
            ping = Buffer.from([0x89, 0x81, 1, 2, 3, 4, 'p'.charCodeAt(0) ^ 1]),
            result = await raw(server.port, [request, ping.subarray(0, 3), ping.subarray(3)], { delay: 25 });

        expect(result.data.includes(Buffer.from([0x8a, 1, 'p'.charCodeAt(0)]))).toBe(true);
    });
});

describe('addresses', () => {
    it('keeps the cached remote address on upgrade', async () => {
        let ws = await connect(`${server.url}/remote`),
            parsed = JSON.parse((await message(ws)).data.toString()) as { address: number; port: number; text: string };

        expect(parsed.address).toBeGreaterThan(0);
        expect(loopback(parsed.text)).toBe(true);
        expect(parsed.port).toBeGreaterThan(0);
        ws.close();
    });
});
