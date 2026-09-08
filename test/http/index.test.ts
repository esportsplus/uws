import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { spawn } from 'node:child_process';
import { createConnection } from 'node:net';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { App, DeclarativeResponse, LIBUS_LISTEN_EXCLUSIVE_PORT, us_listen_socket_close, us_socket_local_port } from '../../src/index';
import { listen, loopback, raw, request, sleep, text } from '../harness';
import type { HttpResponse, TemplatedApp, us_listen_socket } from '../../src/index';
import type { Server } from '../harness';


const BIG = 'x'.repeat(1_000_000);

const STREAM_CHUNK = Buffer.alloc(64 * 1024, 'a');

const STREAM_SIZE = 8 * 1024 * 1024;

const captured: Record<string, unknown> = {};

const acceptExhaustionChildScript = `
import net from 'node:net';
import { App, us_socket_local_port } from './src/index.ts';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const connect = (port) => new Promise((resolve) => {
    let socket = net.connect(port, '127.0.0.1');
    socket.once('connect', () => resolve(socket));
    socket.once('error', () => resolve(socket));
});

let app = App().get('/*', (res) => res.end('ok')),
    token = await new Promise((resolve) => app.listen(0, resolve));

if (!token) {
    throw new Error('listen failed');
}

let port = us_socket_local_port(token),
    sockets = [];

for (let i = 0; i < 64; i++) {
    sockets.push(await connect(port));
}

let before = process.cpuUsage();
await sleep(1100);
let used = process.cpuUsage(before),
    cpu = used.user + used.system;

for (let socket of sockets.splice(0)) {
    socket.destroy();
}

await sleep(1100);
let recovered = await connect(port);
let body = await new Promise((resolve) => {
    recovered.once('data', (data) => resolve(data.toString()));
    recovered.once('error', () => resolve(''));
    recovered.write('GET / HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n');
});

if (process.send) {
    process.send({ cpu, recovered: body.includes('ok') }, () => process.exit(0));
}
else {
    process.exit(0);
}
`;


let server: Server;


function build(): TemplatedApp {
    let app = App();

    app.get('/hello', (res) => {
        res.end('Hello World!');
    });
    app.get('/status', (res) => {
        res.writeStatus('201 Created').writeHeader('X-Custom', 'yes').end('created');
    });
    app.get('/empty', (res) => {
        res.end();
    });
    app.get('/chunked', (res) => {
        res.write('a');
        res.write('b');
        res.end('c');
    });
    app.get('/headers', (res, req) => {
        let all: string[][] = [];

        req.forEach((key, value) => {
            all.push([key, value]);
        });

        json(res, { all, custom: req.getHeader('x-custom'), missing: req.getHeader('x-missing') });
    });
    app.get('/method', (res, req) => {
        json(res, [req.getCaseSensitiveMethod(), req.getMethod(), req.getCaseSensitiveMethod()]);
    });
    app.get('/url', (res, req) => {
        json(res, [req.getUrl(), req.getQuery() ?? null]);
    });
    app.get('/query', (res, req) => {
        json(res, [req.getQuery(), req.getQuery('a'), req.getQuery('b'), req.getQuery('c'), req.getQuery('missing') ?? null, req.getQuery('empty')]);
    });
    app.get('/query-twice', (res, req) => {
        json(res, [req.getQuery('q'), req.getQuery('q')]);
    });
    app.get('/user/:id/:name', (res, req) => {
        json(res, [req.getParameter(0), req.getParameter(1), req.getParameter('id'), req.getParameter('name'), req.getParameter(5), req.getParameter('nope')]);
    });
    app.get('/x/static', (res) => {
        res.end('static');
    });
    app.get('/x/:p', (res, req) => {
        res.end(`param ${req.getParameter(0)}`);
    });
    app.get('/x/*', (res) => {
        res.end('wild');
    });
    app.get('/yield', (_res, req) => {
        req.setYield(true);
    });
    app.any('/yield', (res, req) => {
        res.end(`fallback ${req.getMethod()}`);
    });
    app.any('/anything', (res, req) => {
        res.end(`any ${req.getMethod()}`);
    });
    app.post('/echo', (res) => {
        res.onAborted(() => {});
        res.collectBody(1024, (body) => {
            res.cork(() => {
                res.end(body === null ? 'null' : text(body));
            });
        });
    });
    app.post('/ondata', (res) => {
        let chunks: string[] = [],
            lasts: boolean[] = [];

        res.onAborted(() => {});
        res.onData((chunk, isLast) => {
            chunks.push(text(chunk));
            lasts.push(isLast);

            if (isLast) {
                res.cork(() => {
                    json(res, { chunks, lasts });
                });
            }
        });
    });
    app.post('/cl0', (res) => {
        res.onAborted(() => {});
        res.onData((_chunk, isLast) => {
            if (isLast) {
                res.cork(() => res.end('done'));
            }
        });
    });
    app.post('/chunk-close', (res) => {
        res.onAborted(() => {});
        res.onData(() => {
            res.close();
        });
    });
    app.get('/second-after-chunk', (res) => {
        captured.secondDispatched = true;
        res.end('second');
    });
    app.post('/ondatav2', (res) => {
        let remaining: string[] = [];

        res.onAborted(() => {});
        res.onDataV2((chunk, maxRemaining) => {
            remaining.push(`${chunk.byteLength}:${maxRemaining}`);

            if (maxRemaining === 0n) {
                res.cork(() => {
                    json(res, remaining);
                });
            }
        });
    });
    app.post('/pause', (res) => {
        let done = false;

        res.onAborted(() => {
            done = true;
        });
        res.pause();
        res.collectBody(65536, (body) => {
            done = true;
            res.cork(() => {
                res.end(body === null ? 'null' : String(body.byteLength));
            });
        });
        setTimeout(() => {
            if (!done) {
                res.resume();
            }
        }, 50);
    });
    app.get('/tryend', (res) => {
        captured.tryEnd = [res.tryEnd('123', 5), res.tryEnd('45', 5)];
    });
    app.get('/write-offset', (res) => {
        let before = res.getWriteOffset();

        res.tryEnd('abc', 6);
        captured.writeOffset = [before, res.getWriteOffset()];
        res.tryEnd('def', 6);
    });
    app.get('/abort', (res) => {
        res.onAborted(() => {
            captured.aborted = true;
        });
    });
    app.get('/pending', (res) => {
        res.onAborted(() => {
            captured.pendingAborted = true;
        });
    });
    app.get('/parameter-edge/:value', (res, req) => {
        json(res, [req.getParameter(65536), req.getParameter(-1)]);
    });
    app.get('/header-count', (res, req) => {
        let count = 0;

        req.forEach(() => {
            count++;
        });
        res.end(String(count));
    });
    app.post('/collect-exact', (res) => {
        res.onAborted(() => {});
        res.collectBody(5, (body) => {
            res.end(body === null ? 'null' : Buffer.from(body).toString('hex'));
        });
    });
    app.post('/throughput', (res) => {
        res.onAborted(() => {});
        res.onData(() => {});
    });
    app.post('/pause-slow', (res) => {
        let resume = setTimeout(() => res.resume(), 5000);

        res.onAborted(() => { clearTimeout(resume); });
        res.pause();
        res.collectBody(65536, (body) => {
            clearTimeout(resume);
            res.end(body === null ? 'null' : text(body));
        });
    });
    app.get('/after-end', (res) => {
        res.end('x');

        try {
            res.end('y');
        }
        catch (error) {
            captured.afterEnd = (error as Error).message;
        }
    });
    app.get('/req-later', (res, req) => {
        res.end('ok');
        setTimeout(() => {
            try {
                req.getUrl();
            }
            catch (error) {
                captured.reqLater = (error as Error).message;
            }
        }, 0);
    });
    app.get('/bad-body', (res) => {
        try {
            res.end(5 as never);
        }
        catch (error) {
            captured.badBody = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/end-symbol', (res) => {
        try {
            res.endWithoutBody(Symbol() as never);
        }
        catch (error) {
            captured.endSymbol = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/tryend-symbol', (res) => {
        try {
            res.tryEnd('x', Symbol() as never);
        }
        catch (error) {
            captured.tryEndSymbol = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/bad-header-value', (res) => {
        try {
            res.writeHeader('X', 'a\r\nInjected: 1');
        }
        catch (error) {
            captured.badHeaderValue = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/bad-header-key', (res) => {
        try {
            res.writeHeader('X\r\nY', 'v');
        }
        catch (error) {
            captured.badHeaderKey = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/bad-status', (res) => {
        try {
            res.writeStatus('200 OK\r\nX: 1');
        }
        catch (error) {
            captured.badStatus = (error as Error).message;
            res.end('caught');
        }
    });
    app.get('/remote', (res) => {
        json(res, {
            address: res.getRemoteAddress().byteLength,
            port: res.getRemotePort(),
            proxied: res.getProxiedRemoteAddressAsText(),
            proxiedAddress: res.getProxiedRemoteAddress().byteLength,
            proxiedPort: res.getProxiedRemotePort(),
            text: res.getRemoteAddressAsText()
        });
    });
    app.get('/cork', (res) => {
        let returned = res.cork(() => {
            res.writeHeader('X-Corked', '1');
            res.write('a');
            res.end('b');
        });

        captured.corkReturnsThis = returned === res;
    });
    app.head('/head', (res) => {
        res.endWithoutBody(5);
    });
    app.head('/head-body', (res) => {
        res.end('body');
    });
    app.head('/head-write', (res) => {
        res.write('body');
        res.end();
    });
    app.get('/end-without-body', (res) => {
        res.endWithoutBody();
    });
    app.get('/end-without-body-close', (res) => {
        res.endWithoutBody(undefined, true);
    });
    app.get('/end-backpressure', (res) => {
        res.end('b'.repeat(4 * 1024 * 1024), true);
    });
    app.get('/close', (res) => {
        res.close();
    });
    app.get('/no-response', () => {
        /* Intentionally returns without responding and without onAborted: the server must
         * close just this connection after the idle timeout, not abort the whole process. */
    });
    app.get('/late-response', (res) => {
        setTimeout(() => {
            res.end('late');
        }, 50);
    });
    app.get('/late-after-abort', (res) => {
        setTimeout(() => {
            try {
                res.end('late');
            }
            catch (error) {
                captured.lateAfterAbort = (error as Error).message;
            }
        }, 100);
    });
    app.ws('/late-upgrade', {
        upgrade: (res, req, context) => {
            let extensions = req.getHeader('sec-websocket-extensions'),
                key = req.getHeader('sec-websocket-key'),
                protocol = req.getHeader('sec-websocket-protocol');

            setTimeout(() => {
                try {
                    res.upgrade({}, key, protocol, extensions, context);
                }
                catch (error) {
                    captured.lateUpgrade = (error as Error).message;
                }
            }, 100);
        }
    });
    app.get('/closeconn', (res) => {
        res.end('bye', true);
    });
    app.get('/nulled', (res) => {
        res.end('x');
    });
    app.get('/nulled', null as never);
    app.get('/declarative/:p', declarative() as never);
    app.get('/utf8', (res) => {
        res.end('héllo wörld ✓');
    });
    app.get('/buffer', (res) => {
        res.end(Buffer.from('buf'));
    });
    app.get('/arraybuffer', (res) => {
        res.end(new Uint8Array([104, 105]).buffer);
    });
    app.get('/dataview', (res) => {
        res.end(new DataView(new Uint8Array([100, 118]).buffer));
    });
    app.get('/shared', (res) => {
        let shared = new SharedArrayBuffer(2);

        new Uint8Array(shared).set([115, 97]);
        res.end(shared);
    });
    app.get('/big', (res) => {
        res.end(BIG);
    });
    app.get('/stream', (res) => {
        stream(res);
    });

    return app;
}

function declarative(): ArrayBuffer {
    try {
        return new DeclarativeResponse()
            .writeStatus('202 Accepted')
            .writeHeader('X-Decl', 'yes')
            .writeQueryValue('q')
            .write('|')
            .writeHeaderValue('x-in')
            .write('|')
            .writeParameterValue('p')
            .writeBody()
            .end('|done');
    }
    catch (error) {
        captured.declarativeError = error;

        return new ArrayBuffer(0);
    }
}

function json(res: HttpResponse, value: unknown): void {
    res.writeHeader('Content-Type', 'application/json').end(JSON.stringify(value));
}

function stream(res: HttpResponse): void {
    let pump = (): boolean => {
        while (true) {
            let offset = res.getWriteOffset(),
                result = res.tryEnd(STREAM_CHUNK.subarray(0, Math.min(STREAM_CHUNK.length, STREAM_SIZE - offset)), STREAM_SIZE),
                ok = (result & 1) !== 0,
                done = (result & 2) !== 0;

            if (done) {
                return true;
            }

            if (!ok) {
                return false;
            }
        }
    };

    res.onAborted(() => {});

    if (pump()) {
        return;
    }

    res.onWritable(() => pump());
}


beforeAll(() => {
    server = listen(build());
});

afterAll(() => {
    server.close();
});


describe('responses', () => {
    it('serves a plain body with content-length and date but no server mark', async () => {
        let response = await request(`${server.url}/hello`);

        expect(response.status).toBe(200);
        expect(response.body.toString()).toBe('Hello World!');
        expect(response.headers['content-length']).toBe('12');
        expect(response.headers.date).toMatch(/^[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT$/);
        expect(response.headers.uws).toBeUndefined();
        expect(response.headers['transfer-encoding']).toBeUndefined();
    });

    it('serves the default 404 page for unmatched routes', async () => {
        let response = await request(`${server.url}/nope`);

        expect(response.status).toBe(404);
        expect(response.body.toString()).toBe('<html><body><h1>File Not Found</h1><hr><i>uws Server</i></body></html>');
    });

    it('writes custom status and headers', async () => {
        let response = await request(`${server.url}/status`);

        expect(response.status).toBe(201);
        expect(response.headers['x-custom']).toBe('yes');
        expect(response.body.toString()).toBe('created');
    });

    it('ends with an empty body', async () => {
        let response = await request(`${server.url}/empty`);

        expect(response.status).toBe(200);
        expect(response.headers['content-length']).toBe('0');
        expect(response.body.length).toBe(0);
    });

    it('streams writes as chunked transfer encoding', async () => {
        let response = await request(`${server.url}/chunked`);

        expect(response.headers['transfer-encoding']).toBe('chunked');
        expect(response.headers['content-length']).toBeUndefined();
        expect(response.body.toString()).toBe('abc');
    });

    it('reports tryEnd progress as packed flags', async () => {
        let response = await request(`${server.url}/tryend`);

        expect(response.body.toString()).toBe('12345');
        expect(response.headers['content-length']).toBe('5');
        expect(captured.tryEnd).toEqual([1, 3]);
    });

    it('tracks the write offset', async () => {
        let response = await request(`${server.url}/write-offset`);

        expect(response.body.toString()).toBe('abcdef');
        expect(captured.writeOffset).toEqual([0, 3]);
    });

    it('answers HEAD with a reported content length', async () => {
        let response = await request(`${server.url}/head`, { method: 'HEAD' });

        expect(response.status).toBe(200);
        expect(response.headers['content-length']).toBe('5');
        expect(response.body.length).toBe(0);
    });

    it('suppresses HEAD bodies for end and write responses', async () => {
        let ended = await raw(server.port, 'HEAD /head-body HTTP/1.1\r\nHost: x\r\n\r\n'),
            written = await raw(server.port, 'HEAD /head-write HTTP/1.1\r\nHost: x\r\n\r\n');

        expect(ended.data.toString()).toContain('Content-Length: 4\r\n');
        expect(ended.data.toString().split('\r\n\r\n').slice(1).join('\r\n\r\n')).toBe('');
        expect(written.data.toString()).toContain('Transfer-Encoding: chunked\r\n');
        expect(written.data.toString().split('\r\n\r\n').slice(1).join('\r\n\r\n')).toBe('');
    });

    it('ends without a body with optional connection close', async () => {
        let ordinary = await raw(server.port, 'GET /end-without-body HTTP/1.1\r\nHost: x\r\n\r\n'),
            closing = await raw(server.port, 'GET /end-without-body-close HTTP/1.1\r\nHost: x\r\n\r\n');

        expect(ordinary.data.toString()).toMatch(/^HTTP\/1\.1 200 OK\r\n/);
        expect(ordinary.data.toString()).not.toContain('Content-Length:');
        expect(ordinary.data.toString()).toMatch(/\r\n\r\n$/);
        expect(closing.data.toString()).toContain('Connection: close\r\n');
        expect(closing.ended).toBe(true);
    });

    it('finishes an end(data, true) response after a paused client drains it', async () => {
        let received = await new Promise<Buffer>((resolve, reject) => {
            let chunks: Buffer[] = [],
                socket = createConnection({ host: '127.0.0.1', port: server.port }),
                paused = false;

            socket.on('connect', () => socket.write('GET /end-backpressure HTTP/1.1\r\nHost: x\r\n\r\n'));
            socket.on('data', (chunk) => {
                chunks.push(typeof chunk === 'string' ? Buffer.from(chunk) : chunk);
                if (!paused) {
                    paused = true;
                    socket.pause();
                    setTimeout(() => socket.resume(), 50);
                }
            });
            socket.on('end', () => {
                socket.destroy();
                resolve(Buffer.concat(chunks));
            });
            socket.on('error', (error) => {
                socket.destroy();
                reject(error);
            });
        });

        expect(received.toString()).toContain('Content-Length: 4194304');
        expect(received.subarray(received.indexOf(Buffer.from('\r\n\r\n')) + 4).length).toBe(4 * 1024 * 1024);
    }, 10000);

    it('skips the connection close header when the client already asked to close', async () => {
        let response = await request(`${server.url}/closeconn`);

        expect(response.headers.connection).toBeUndefined();
        expect(response.body.toString()).toBe('bye');
    });

    it('returns res from cork and applies corked writes', async () => {
        let response = await request(`${server.url}/cork`);

        expect(captured.corkReturnsThis).toBe(true);
        expect(response.headers['x-corked']).toBe('1');
        expect(response.headers['transfer-encoding']).toBe('chunked');
        expect(response.body.toString()).toBe('ab');
    });

    it('executes declarative responses', async () => {
        let response = await request(`${server.url}/declarative/PARAM?q=QUERY`, { headers: { 'X-In': 'HDR' } });

        expect(captured.declarativeError).toBeUndefined();
        expect(response.status).toBe(202);
        expect(response.headers['x-decl']).toBe('yes');
        expect(response.headers['transfer-encoding']).toBe('chunked');
        expect(response.body.toString()).toBe('QUERY|HDR|PARAM|done');
    });

    it('removes a route registered again with a null handler', async () => {
        expect((await request(`${server.url}/nulled`)).status).toBe(404);
    });

    it('encodes string bodies as utf-8', async () => {
        let response = await request(`${server.url}/utf8`);

        expect(response.body.toString()).toBe('héllo wörld ✓');
        expect(response.headers['content-length']).toBe(String(Buffer.byteLength('héllo wörld ✓')));
    });

    it('accepts Buffer, ArrayBuffer, DataView and SharedArrayBuffer bodies', async () => {
        expect((await request(`${server.url}/buffer`)).body.toString()).toBe('buf');
        expect((await request(`${server.url}/arraybuffer`)).body.toString()).toBe('hi');
        expect((await request(`${server.url}/dataview`)).body.toString()).toBe('dv');
        expect((await request(`${server.url}/shared`)).body.toString()).toBe('sa');
    });

    it('sends bodies larger than the cork buffer', async () => {
        let response = await request(`${server.url}/big`);

        expect(response.headers['content-length']).toBe(String(BIG.length));
        expect(response.body.toString()).toBe(BIG);
    });

    it('streams a large body through tryEnd and onWritable', async () => {
        let response = await request(`${server.url}/stream`);

        expect(response.headers['content-length']).toBe(String(STREAM_SIZE));
        expect(response.body.length).toBe(STREAM_SIZE);
        expect(response.body.equals(Buffer.alloc(STREAM_SIZE, 'a'))).toBe(true);
    }, 30000);

    it('destroys the connection on close()', async () => {
        await expect(request(`${server.url}/close`)).rejects.toThrow();
    });

    it('responds when a handler returns before responding', async () => {
        expect((await request(`${server.url}/late-response`)).body.toString()).toBe('late');

        /* The process survived the retained response, so a normal request still works. */
        let response = await request(`${server.url}/hello`);

        expect(response.body.toString()).toBe('Hello World!');
    });

    it('invalidates a retained response when the client disconnects', async () => {
        await raw(server.port, 'GET /late-after-abort HTTP/1.1\r\nHost: x\r\n\r\n', { timeout: 50 });
        await sleep(100);

        expect(captured.lateAfterAbort).toContain('uWS.HttpResponse must not be accessed after');
    });

    it('invalidates a retained upgrade response when the client disconnects', async () => {
        await raw(server.port, 'GET /late-upgrade HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n', { timeout: 50 });
        await sleep(100);

        expect(captured.lateUpgrade).toContain('uWS.HttpResponse must not be accessed after');
    });

    it.skipIf(!process.env.UWS_SLOW_TESTS).concurrent('closes the connection after the idle timeout when a handler never responds', async () => {
        let result = await raw(server.port, 'GET /no-response HTTP/1.1\r\nHost: x\r\n\r\n', { timeout: 16000 });

        expect(result.ended).toBe(true);

        /* The process survived the misbehaving handler, so a normal request still works. */
        let response = await request(`${server.url}/hello`);

        expect(response.body.toString()).toBe('Hello World!');
    }, 20000);

    it.skipIf(!process.env.UWS_SLOW_TESTS).concurrent('closes a slow-loris request that never completes its headers', async () => {
        let result = await raw(server.port, 'GET / HTTP/1.1\r\nHost: x\r\n', { timeout: 16000 });

        expect(result.ended).toBe(true);
    }, 20000);

    it.skipIf(!process.env.UWS_SLOW_TESTS).concurrent('enforces the upload throughput floor', async () => {
        let ended = await new Promise<boolean>((resolve, reject) => {
            let socket = createConnection({ host: '127.0.0.1', port: server.port }),
                timer: NodeJS.Timeout | undefined,
                writes = 0;

            socket.on('connect', () => {
                socket.write('POST /throughput HTTP/1.1\r\nHost: x\r\nContent-Length: 2097152\r\n\r\n');
                timer = setInterval(() => {
                    writes++;
                    socket.write(Buffer.alloc(4096));
                }, 1000);
            });
            socket.on('close', () => {
                socket.destroy();
                clearInterval(timer);
                resolve(writes < 15);
            });
            socket.on('error', (error) => {
                clearInterval(timer);
                socket.destroy();
                reject(error);
            });
        });

        expect(ended).toBe(true);
    }, 20000);
});

describe('requests', () => {
    it('exposes url and raw query', async () => {
        expect(JSON.parse((await request(`${server.url}/url?x=1&y=2`)).body.toString())).toEqual(['/url', 'x=1&y=2']);
        expect(JSON.parse((await request(`${server.url}/url`)).body.toString())).toEqual(['/url', null]);
    });

    it('lowercases the method but keeps the case-sensitive copy', async () => {
        expect(JSON.parse((await request(`${server.url}/method`)).body.toString())).toEqual(['GET', 'get', 'GET']);
    });

    it('decodes query values', async () => {
        let response = await request(`${server.url}/query?a=1&b=hello%20world&c=x+y&empty=`);

        expect(JSON.parse(response.body.toString())).toEqual(['a=1&b=hello%20world&c=x+y&empty=', '1', 'hello world', 'x y', null, '']);
    });

    it('decodes a query value the same way on repeated reads', async () => {
        expect(JSON.parse((await request(`${server.url}/query-twice?q=%2541`)).body.toString())).toEqual(['%41', '%41']);
    });

    it('exposes route parameters by index and name', async () => {
        expect(JSON.parse((await request(`${server.url}/user/42/bob`)).body.toString())).toEqual(['42', 'bob', '42', 'bob', '', '']);
    });

    it('returns empty for out-of-range and negative parameter indexes', async () => {
        /* getParameter now rejects indexes above the unsigned-short cap instead of wrapping to param 0. */
        expect(JSON.parse((await request(`${server.url}/parameter-edge/x`)).body.toString())).toEqual(['', '']);
    });

    it('iterates headers lowercased and reads them by name', async () => {
        let response = await request(`${server.url}/headers`, { headers: { 'X-Custom': 'v', 'Y-Other': 'w' } }),
            parsed = JSON.parse(response.body.toString()) as { all: string[][]; custom: string; missing: string },
            keys = parsed.all.map((header) => header[0]);

        expect(keys).toEqual(keys.map((key) => key.toLowerCase()));
        expect(keys).toContain('host');
        expect(parsed.all).toContainEqual(['x-custom', 'v']);
        expect(parsed.all).toContainEqual(['y-other', 'w']);
        expect(parsed.custom).toBe('v');
        expect(parsed.missing).toBe('');
    });

    it('iterates nearly the maximum number of request headers', async () => {
        let headers: Record<string, string> = {};

        for (let i = 0; i < 89; i++) {
            headers[`x-${i}`] = 'v';
        }
        expect((await request(`${server.url}/header-count`, { headers })).body.toString()).toBe('91');
    });
});

describe('routing', () => {
    it('rejects registering routes from within a request handler', async () => {
        delete captured.m7;
        let app = App();

        app.get('/register', (res) => {
            try {
                app.get('/late', (res) => { res.end('late'); });
                captured.m7 = 'no-throw';
            }
            catch (error) {
                captured.m7 = (error as Error).message;
            }
            res.end('ok');
        });
        app.get('/existing', (res) => { res.end('existing'); });
        let temporary = listen(app);

        try {
            expect((await request(`${temporary.url}/register`)).body.toString()).toBe('ok');
            expect(captured.m7).toContain('request handler');
            expect((await request(`${temporary.url}/existing`)).body.toString()).toBe('existing');
            expect((await request(`${temporary.url}/late`)).status).toBe(404);
        }
        finally {
            temporary.close();
        }
    }, 3000);

    it('prefers static over parameter over wildcard segments', async () => {
        expect((await request(`${server.url}/x/static`)).body.toString()).toBe('static');
        expect((await request(`${server.url}/x/foo`)).body.toString()).toBe('param foo');
        expect((await request(`${server.url}/x/foo/bar`)).body.toString()).toBe('wild');
    });

    it('falls through to any() handlers when a handler yields', async () => {
        expect((await request(`${server.url}/yield`)).body.toString()).toBe('fallback get');
        expect((await request(`${server.url}/yield`, { method: 'POST' })).body.toString()).toBe('fallback post');
    });

    it('matches any() for every method', async () => {
        expect((await request(`${server.url}/anything`)).body.toString()).toBe('any get');
        expect((await request(`${server.url}/anything`, { method: 'PUT' })).body.toString()).toBe('any put');
        expect((await request(`${server.url}/anything`, { method: 'DELETE' })).body.toString()).toBe('any delete');
    });

    it('removes parameter, wildcard and any routes and permits re-adding them', async () => {
        let app = App()
                .get('/gone/:id', (res) => { res.end('parameter'); })
                .get('/wild/*', (res) => { res.end('wildcard'); })
                .any('/any-gone', (res) => { res.end('any'); }),
            temporary = listen(app);

        app.get('/gone/:id', null as never).get('/wild/*', null as never).any('/any-gone', null as never).get('/never-there', null as never);
        expect((await request(`${temporary.url}/gone/x`)).status).toBe(404);
        expect((await request(`${temporary.url}/wild/x`)).status).toBe(404);
        expect((await request(`${temporary.url}/any-gone`, { method: 'POST' })).status).toBe(404);

        app.get('/gone/:id', (res) => { res.end('again'); });
        expect((await request(`${temporary.url}/gone/x`)).body.toString()).toBe('again');
        temporary.close();
    });
});

describe('request bodies', () => {
    it('collects a body', async () => {
        expect((await request(`${server.url}/echo`, { body: 'hello', method: 'POST' })).body.toString()).toBe('hello');
    });

    it('hands null to collectBody when the body exceeds the limit', async () => {
        expect((await request(`${server.url}/echo`, { body: 'x'.repeat(2048), method: 'POST' })).body.toString()).toBe('null');
    });

    it('collects identical bytes on fast and split paths and accepts an exactly-maximal body', async () => {
        let fast = await raw(server.port, 'POST /collect-exact HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello'),
            slow = await raw(server.port, ['POST /collect-exact HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe', 'llo']);

        expect(fast.data.toString()).toMatch(/\r\n\r\n68656c6c6f$/);
        expect(slow.data.toString()).toMatch(/\r\n\r\n68656c6c6f$/);
    });

    it('streams chunks through onData', async () => {
        let parsed = JSON.parse((await request(`${server.url}/ondata`, { body: 'hello world', method: 'POST' })).body.toString()) as { chunks: string[]; lasts: boolean[] };

        expect(parsed.chunks.join('')).toBe('hello world');
        expect(parsed.lasts.at(-1)).toBe(true);
        expect(parsed.lasts.slice(0, -1).every((last) => !last)).toBe(true);
    });

    it('fires onData for Content-Length: 0 after fragmented large headers', async () => {
        let headers = `POST /cl0 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nCookie: ${'x'.repeat(3000)}`,
            split = Math.floor(headers.length / 2),
            result = await raw(server.port, [headers.slice(0, split), headers.slice(split) + '\r\n\r\n'], { delay: 25, timeout: 2000 });

        expect(result.data.toString()).toMatch(/^HTTP\/1\.1 200 OK\r\n/);
        expect(result.data.toString().split('\r\n\r\n').slice(1).join('\r\n\r\n')).toBe('done');
    }, 3000);

    it('reports the remaining length through onDataV2', async () => {
        let parsed = JSON.parse((await request(`${server.url}/ondatav2`, { body: 'hello world', method: 'POST' })).body.toString()) as string[];

        expect(parsed.at(-1)).toMatch(/:0$/);
        expect(parsed.reduce((sum, entry) => sum + Number(entry.split(':')[0]), 0)).toBe(11);
    });

    it('accepts chunked request bodies', async () => {
        let response = await request(`${server.url}/echo`, { body: 'chunked body', headers: { 'Transfer-Encoding': 'chunked' }, method: 'POST' });

        expect(response.body.toString()).toBe('chunked body');
    });

    it('does not dispatch a pipelined request after a split chunked body handler closes', async () => {
        captured.secondDispatched = false;
        let result = await raw(server.port, [
            'POST /chunk-close HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
            '5\r\nhello\r\n0\r\n\r\nGET /second-after-chunk HTTP/1.1\r\nHost: x\r\n\r\n'
        ], { delay: 25 });

        expect(captured.secondDispatched).toBe(false);
        expect(result.ended).toBe(true);

        expect((await request(`${server.url}/hello`)).status).toBe(200);
    });

    it('reports unknown remaining length for chunked bodies', async () => {
        let parsed = JSON.parse((await request(`${server.url}/ondatav2`, { body: 'chunked body', headers: { 'Transfer-Encoding': 'chunked' }, method: 'POST' })).body.toString()) as string[];

        expect(parsed.at(-1)).toBe('0:0');
        expect(parsed.some((entry) => entry.endsWith(':18446744073709551615'))).toBe(true);
    });

    it('pauses and resumes body streaming', async () => {
        expect((await request(`${server.url}/pause`, { body: 'y'.repeat(30000), method: 'POST' })).body.toString()).toBe('30000');
    });

    it.skipIf(!process.env.UWS_SLOW_TESTS)('does not time out while a paused body is resumed later', async () => {
        expect((await request(`${server.url}/pause-slow`, { body: 'buffered', method: 'POST' })).body.toString()).toBe('buffered');
    }, 10000);

    it('calls onAborted when the client disconnects', async () => {
        await raw(server.port, 'GET /abort HTTP/1.1\r\nHost: x\r\n\r\n', { timeout: 200 });
        await sleep(100);

        expect(captured.aborted).toBe(true);
    });

    it('closes a pipelined connection and aborts the first pending response', async () => {
        captured.pendingAborted = false;
        let result = await raw(server.port, 'GET /pending HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n', { timeout: 1000 });

        expect(result.ended).toBe(true);
        expect(captured.pendingAborted).toBe(true);
    });
});

describe('errors', () => {
    it('throws when a response is used after end', async () => {
        await request(`${server.url}/after-end`);

        expect(captured.afterEnd).toContain('uWS.HttpResponse must not be accessed after');
    });

    it('throws when a request is used after the handler returns', async () => {
        await request(`${server.url}/req-later`);
        await sleep(50);

        expect(captured.reqLater).toContain('uWS.HttpRequest must not be accessed after await or route handler return');
    });

    it('throws for unsupported body types', async () => {
        expect((await request(`${server.url}/bad-body`)).body.toString()).toBe('caught');
        expect(captured.badBody).toBe('Text and data can only be passed by String, ArrayBuffer or ArrayBufferView.');
    });

    it('rejects a Symbol length in endWithoutBody', async () => {
        delete captured.endSymbol;

        expect((await request(`${server.url}/end-symbol`)).body.toString()).toBe('caught');
        expect(captured.endSymbol).toContain('number');
    });

    it('rejects a Symbol length in tryEnd', async () => {
        delete captured.tryEndSymbol;

        expect((await request(`${server.url}/tryend-symbol`)).body.toString()).toBe('caught');
        expect(captured.tryEndSymbol).toContain('number');
    });

    it('rejects response header injection', async () => {
        for (let path of ['/bad-header-value', '/bad-header-key', '/bad-status']) {
            expect((await request(`${server.url}${path}`)).body.toString()).toBe('caught');
        }

        expect(captured.badHeaderValue).toBe('uWS: header contains control characters');
        expect(captured.badHeaderKey).toBe('uWS: header contains control characters');
        expect(captured.badStatus).toBe('uWS: header contains control characters');
    });

    it('rejects header injection in declarative responses', () => {
        expect(() => new DeclarativeResponse().writeHeader('X', 'a\r\nb')).toThrow('uWS: header contains control characters');
    });
});

describe('addresses', () => {
    it('reports the remote address in binary and text', async () => {
        let parsed = JSON.parse((await request(`${server.url}/remote`)).body.toString()) as Record<string, number | string>;

        expect([4, 16]).toContain(parsed.address);
        expect(loopback(parsed.text as string)).toBe(true);
        expect(parsed.port).toBeGreaterThan(0);
        expect(parsed.proxied).toBe('');
        expect(parsed.proxiedAddress).toBe(0);
        expect(parsed.proxiedPort).toBe(0);
    });

    it.skipIf(!process.env.UWS_NET_TESTS || process.platform === 'win32')('reports IPv6 and IPv4-mapped peer addresses', async () => {
        /* Requires an IPv6-enabled loopback interface and dual-stack sockets. */
        let ipv6 = listen(App().get('/*', (res) => {
                json(res, { bytes: res.getRemoteAddress().byteLength, text: res.getRemoteAddressAsText() });
            }), { host: '::1' }),
            dual = listen(App().get('/*', (res) => { res.end(res.getRemoteAddressAsText()); }), { host: '::' });

        let v6 = JSON.parse((await request(`http://[::1]:${ipv6.port}/`)).body.toString()) as { bytes: number; text: string };
        expect(v6).toEqual({ bytes: 16, text: '::1' });
        expect((await request(`http://127.0.0.1:${dual.port}/`)).body.toString()).toBe('127.0.0.1');
        ipv6.close();
        dual.close();
    });
});

describe('filter', () => {
    it('invalidates a retained response after the filter callback returns', async () => {
        delete captured.filterRes;
        let filtered = listen(App().filter((res, count) => {
            if (count === 1) {
                captured.filterRes = res;
            }
        }).get('/*', (res) => {
            res.end('ok');
        }));

        try {
            expect((await request(`${filtered.url}/`)).body.toString()).toBe('ok');
            expect(() => (captured.filterRes as HttpResponse).getRemoteAddressAsText()).toThrow('must not be accessed');
        }
        finally {
            filtered.close();
            delete captured.filterRes;
        }
    }, 3000);

    it('reports connection open and close', async () => {
        let events: number[] = [],
            filtered = listen(App().filter((_res, count) => {
                events.push(count);
            }).get('/*', (res) => {
                res.end('x');
            }));

        await request(`${filtered.url}/`);
        await sleep(100);

        expect(events).toEqual([1, -1]);
        filtered.close();
    });
});

describe('listen', () => {
    it.skipIf(process.platform !== 'linux')('backs off accepting while file descriptors are exhausted', async () => {
        let result = await new Promise<{ cpu: number; recovered: boolean }>((resolve, reject) => {
            let child = spawn('sh', [
                    '-c',
                    'ulimit -n 64; exec "$1" --import tsx --input-type=module --eval "$2"',
                    'sh',
                    process.execPath,
                    acceptExhaustionChildScript
                ], {
                    cwd: process.cwd(),
                    stdio: ['ignore', 'ignore', 'pipe', 'ipc'],
                    timeout: 15000
                }),
                stderr = '',
                message: { cpu: number; recovered: boolean } | undefined;

            if (child.stderr) {
                child.stderr.setEncoding('utf8');
                child.stderr.on('data', (chunk: string) => { stderr += chunk; });
            }
            child.once('message', (value: unknown) => {
                message = value as { cpu: number; recovered: boolean };
            });
            child.once('error', (error) => {
                child[Symbol.dispose]();
                reject(error);
            });
            child.once('close', (code) => {
                child[Symbol.dispose]();
                if (code || !message) {
                    reject(new Error(`fd-exhaustion child failed (${code}): ${stderr}`));
                }
                else {
                    resolve(message);
                }
            });
        });

        /* A level-triggered accept loop consumed about one full core before the backoff. */
        expect(result.cpu).toBeLessThan(400000);
        expect(result.recovered).toBe(true);
    }, 20000);

    it('selects a free port when given 0', () => {
        let free = listen(App());

        expect(free.port).toBeGreaterThan(0);
        free.close();
    });

    it('reports failure through the callback for an exclusively held port', () => {
        let holder: { result: unknown; socket: us_listen_socket | false } = { result: 'unset', socket: false },
            port = 0;

        App().listen(0, LIBUS_LISTEN_EXCLUSIVE_PORT, (token) => {
            holder.socket = token;

            if (token) {
                port = us_socket_local_port(token);
            }
        });
        App().listen(port, LIBUS_LISTEN_EXCLUSIVE_PORT, (token) => {
            holder.result = token;
        });

        expect(port).toBeGreaterThan(0);
        expect(holder.result).toBe(false);

        if (holder.socket) {
            us_listen_socket_close(holder.socket);
        }
    });

    it('shares an explicit port between two apps by default', () => {
        let probe = listen(App()),
            port = probe.port,
            sockets: (us_listen_socket | false)[] = [];

        probe.close();
        App().listen(port, (token) => {
            sockets.push(token);
        });
        App().listen(port, (token) => {
            sockets.push(token);
        });

        expect(sockets[0]).toBeTruthy();
        expect(sockets[1]).toBeTruthy();

        for (let i = 0, n = sockets.length; i < n; i++) {
            let socket = sockets[i];

            if (socket) {
                us_listen_socket_close(socket);
            }
        }
    });

    it('shares an automatically selected port', () => {
        let first = listen(App()),
            holder: { socket: us_listen_socket | false } = { socket: false };

        App().listen(first.port, (token) => {
            holder.socket = token;
        });

        expect(holder.socket).toBeTruthy();

        if (holder.socket) {
            us_listen_socket_close(holder.socket);
        }
        first.close();
    });

    it('listens on a specific host', async () => {
        let bound = listen(App().get('/*', (res) => {
            res.end('host');
        }), { host: '127.0.0.1' });

        expect((await request(`${bound.url}/`)).body.toString()).toBe('host');
        bound.close();
    });

    it('listens on all interfaces when given an empty host', async () => {
        let app = App().get('/*', (res) => { res.end('ok'); }),
            holder: { socket: us_listen_socket | false } = { socket: false };

        try {
            app.listen('', 0, (token) => {
                holder.socket = token;
            });
            expect(holder.socket).toBeTruthy();

            let port = us_socket_local_port(holder.socket as us_listen_socket);

            expect(port).toBeGreaterThan(0);
            expect((await request(`http://127.0.0.1:${port}/`)).body.toString()).toBe('ok');
        }
        finally {
            if (holder.socket) {
                us_listen_socket_close(holder.socket);
            }
        }
    }, 3000);

    it('rejects connections after the listen socket is closed', async () => {
        let closed = listen(App().get('/*', (res) => {
            res.end('x');
        }));

        closed.close();
        await expect(request(`${closed.url}/`)).rejects.toThrow();
    });

    it('makes listen socket tokens safe to close repeatedly', () => {
        let server = listen(App());

        us_listen_socket_close(server.socket);
        expect(() => us_listen_socket_close(server.socket)).not.toThrow();
        expect(() => us_socket_local_port(server.socket)).toThrow('listen socket is closed');
    });

    it('invalidates listen socket tokens when the app closes', () => {
        let app = App(),
            server = listen(app);

        app.close();
        expect(() => us_listen_socket_close(server.socket)).not.toThrow();
        expect(() => us_socket_local_port(server.socket)).toThrow('listen socket is closed');
    });

    it('rejects invalid listen socket tokens', () => {
        expect(() => us_listen_socket_close(false as unknown as us_listen_socket)).toThrow(TypeError);
        expect(() => us_listen_socket_close({} as us_listen_socket)).toThrow(TypeError);
    });

    it('closes open connections with app.close()', async () => {
        let aborted = false,
            app = App().get('/*', (res) => {
                res.onAborted(() => {
                    aborted = true;
                });
            }),
            pending = listen(app),
            outcome = request(`${pending.url}/`);

        await sleep(100);
        app.close();
        await expect(outcome).rejects.toThrow();

        expect(aborted).toBe(true);
    });

    it('can close from a route handler and listen again afterwards', async () => {
        let app = App(),
            first = listen(app);

        app.get('/close-app', (res) => {
            app.close();
            res.end('closed');
        });
        await expect(request(`${first.url}/close-app`)).rejects.toThrow();

        let second = listen(app);
        app.get('/again', (res) => { res.end('again'); });
        expect((await request(`${second.url}/again`)).body.toString()).toBe('again');
        second.close();
    });

    it('can close from a route handler without responding', async () => {
        let app = App(),
            closed = false;

        app.get('/shutdown', () => {
            app.close();
            closed = true;
        });
        let temporary = listen(app);

        try {
            await expect(request(`${temporary.url}/shutdown`)).rejects.toThrow();
            expect(closed).toBe(true);
        }
        finally {
            if (!closed) {
                temporary.close();
            }
        }
    }, 3000);

    it.skipIf(process.platform === 'win32')('listens on a unix socket', async () => {
        let holder: { socket: us_listen_socket | false } = { socket: false },
            path = join(tmpdir(), `uws-${process.pid}-${Date.now()}.sock`);

        App().get('/*', (res) => {
            res.end('unix');
        }).listen_unix((token) => {
            holder.socket = token;
        }, path);

        expect(holder.socket).toBeTruthy();
        expect((await request('http://localhost/', { socketPath: path })).body.toString()).toBe('unix');

        if (holder.socket) {
            us_listen_socket_close(holder.socket);
        }
    });

    it.skipIf(process.platform === 'win32')('rejects an overlong unix socket path', () => {
        let socket: us_listen_socket | false = false;

        App().listen_unix((token) => {
            socket = token;
        }, join(tmpdir(), 'x'.repeat(200)));

        expect(socket).toBe(false);
    });
});
