import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { App, getParts } from '../../src/index';
import { listen, raw, request, text } from '../harness';
import type { HttpResponse, TemplatedApp } from '../../src/index';
import type { Server } from '../harness';


const PROXY_V2 = Buffer.concat([
    Buffer.from([0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a]),
    Buffer.from([0x21, 0x11, 0x00, 0x0c]),
    Buffer.from([10, 0, 0, 1, 10, 0, 0, 2]),
    Buffer.from([0x30, 0x39, 0x00, 0x50])
]);


let server: Server;
let helloRequests = 0;
let secondDispatched = false;


function build(): TemplatedApp {
    return App()
        .get('/hello', (res) => {
            helloRequests++;
            res.end('Hello World!');
        })
        .get('/header', (res, req) => {
            res.end(`[${req.getHeader('x-a')}]`);
        })
        .get('/query', (res, req) => {
            res.end(JSON.stringify(['a', 'a+b', '', 'missing'].map((key) => req.getQuery(key))));
        })
        .get('/bin', (res, req) => {
            let value = req.getHeader('x-bin');

            res.end(`${value.length}:${Array.from(value, (char) => char.charCodeAt(0)).join(',')}`);
        })
        .get('/remote', (res) => {
            res.end(`${res.getProxiedRemoteAddressAsText()}:${res.getProxiedRemotePort()}:${res.getProxiedRemoteAddress().byteLength}`);
        })
        .get('/begin', (res) => {
            res.beginWrite();
            res.write('x');
            res.end();
        })
        .get('/closeconn', (res) => {
            res.end('bye', true);
        })
        .post('/echo', (res) => {
            echo(res);
        })
        .post('/ondata', (res) => {
            let chunks: string[] = [];

            res.onAborted(() => {});
            res.onData((chunk, isLast) => {
                chunks.push(`${chunk.byteLength}${isLast ? '!' : ''}`);

                if (isLast) {
                    res.cork(() => {
                        res.end(chunks.join(','));
                    });
                }
            });
        })
        .post('/closer', (res) => {
            res.onData(() => {
                res.close();
            });
        })
        .post('/chunk-close', (res) => {
            res.onAborted(() => {});
            res.onData(() => {
                res.close();
            });
        })
        .get('/second-after-chunk', (res) => {
            secondDispatched = true;
            res.end('second');
        })
        .post('/reject-expect', (res) => {
            res.writeStatus('413 Payload Too Large').end();
        });
}

function echo(res: HttpResponse): void {
    res.onAborted(() => {});
    res.collectBody(1 << 20, (body) => {
        res.cork(() => {
            res.end(body === null ? 'null' : `got ${text(body)}`);
        });
    });
}

function get(path: string, headers: string[] = ['Host: x']): string {
    return `GET ${path} HTTP/1.1\r\n${headers.join('\r\n')}\r\n\r\n`;
}

function status(data: Buffer): string[] {
    return data.toString('latin1').match(/HTTP\/1\.1 \d{3}[^\r]*/g) ?? [];
}


beforeAll(() => {
    server = listen(build());
});

afterAll(() => {
    server.close();
});


describe('request line', () => {
    it('answers pipelined requests in order', async () => {
        let result = await raw(server.port, get('/hello') + get('/header', ['Host: x', 'X-A: 2']));

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('Hello World!');
        expect(result.data.toString()).toContain('[2]');
        expect(result.ended).toBe(false);
    });

    it('rejects HTTP/1.0 with 505 and closes', async () => {
        let result = await raw(server.port, 'GET /hello HTTP/1.0\r\nHost: x\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 505 HTTP Version Not Supported']);
        expect(result.data.toString()).toContain('This server does not support HTTP/1.0.');
        expect(result.ended).toBe(true);
    });

    it('rejects absolute-form targets with 505 (upstream quirk)', async () => {
        let result = await raw(server.port, 'GET http://x/hello HTTP/1.1\r\nHost: x\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 505 HTTP Version Not Supported']);
        expect(result.ended).toBe(true);
    });

    it('parses a request split across segments', async () => {
        let result = await raw(server.port, ['GET /hello HTTP/1.1\r\nHo', 'st: x\r\n\r\n']);

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('Hello World!');
    });

    it('does not map HEAD onto GET routes and answers HEAD without a body', async () => {
        let result = await raw(server.port, 'HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 404 File Not Found']);
        expect(result.data.toString()).not.toContain('<html><body>');
    });

    it('rejects a space in the request target with 505', async () => {
        let space = await raw(server.port, 'GET /a b HTTP/1.1\r\nHost: x\r\n\r\n');

        expect(status(space.data)).toEqual(['HTTP/1.1 505 HTTP Version Not Supported']);
    });

    it('rejects LF-only request lines with 505', async () => {
        let lf = await raw(server.port, 'GET / HTTP/1.1\nHost: x\n\n');

        expect(status(lf.data)).toEqual(['HTTP/1.1 505 HTTP Version Not Supported']);
    });

    it('rejects a NUL in the request target with 505', async () => {
        let nul = await raw(server.port, Buffer.from('GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n'));

        expect(status(nul.data)).toEqual(['HTTP/1.1 505 HTTP Version Not Supported']);
    });

    it('routes a 100-segment URL to 404', async () => {
        let deep = await raw(server.port, get('/' + Array.from({ length: 100 }, () => 'x').join('/')));

        /* S22 intentionally treats routes beyond the segment cap as unmatched. */
        expect(status(deep.data)).toEqual(['HTTP/1.1 404 File Not Found']);
    });
});

describe('headers', () => {
    it('rejects requests without a host header with 400', async () => {
        let result = await raw(server.port, 'GET /hello HTTP/1.1\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 400 Bad Request']);
        expect(result.ended).toBe(true);
    });

    it('rejects duplicate host headers with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'Host: y']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects duplicate content-length headers with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'Content-Length: 5', 'Content-Length: 6']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'Content-Length: 5', 'Content-Length: 5']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects duplicate transfer-encoding headers with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'Transfer-Encoding: chunked', 'Transfer-Encoding: chunked']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects both content-length and transfer-encoding with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'Content-Length: 0', 'Transfer-Encoding: chunked']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects control characters in header values with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'X-A: a\x01b']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects whitespace before the colon with 400', async () => {
        expect(status((await raw(server.port, get('/hello', ['Host: x', 'X-A : v']))).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('rejects header blocks over 4096 bytes with 431', async () => {
        let result = await raw(server.port, get('/hello', ['Host: x', `X-A: ${'v'.repeat(5000)}`]));

        expect(status(result.data)).toEqual(['HTTP/1.1 431 Request Header Fields Too Large']);
        expect(result.ended).toBe(true);
    });

    it('rejects more than 98 headers with 431', async () => {
        let headers = ['Host: x'];

        for (let i = 0; i < 120; i++) {
            headers.push(`H${i}: v`);
        }

        expect(status((await raw(server.port, get('/hello', headers))).data)).toEqual(['HTTP/1.1 431 Request Header Fields Too Large']);
    });

    it('trims whitespace around header values', async () => {
        expect((await raw(server.port, get('/header', ['Host: x', 'X-A:   v  ']))).data.toString()).toContain('[v]');
    });

    it('lowercases header names for lookup', async () => {
        expect((await raw(server.port, get('/header', ['Host: x', 'X-A: upper']))).data.toString()).toContain('[upper]');
    });

    it('decodes header bytes as latin1', async () => {
        let result = await raw(server.port, Buffer.from(get('/bin', ['Host: x', 'X-Bin: ca\xe9f']), 'latin1'));

        expect(result.data.toString()).toMatch(/\r\n\r\n4:99,97,233,102$/);
    });

    it('keeps high bytes at the ends of header values', async () => {
        let result = await raw(server.port, Buffer.from(get('/bin', ['Host: x', 'X-Bin: \xe9caf\xe9']), 'latin1'));

        expect(result.data.toString()).toMatch(/\r\n\r\n5:233,99,97,102,233$/);
    });

    it('closes the connection after a connection close request', async () => {
        expect((await raw(server.port, get('/hello', ['Host: x', 'Connection: close']))).ended).toBe(true);
    });

    it('adds a connection close header when the handler asks to close', async () => {
        let result = await raw(server.port, get('/closeconn'));

        expect(result.data.toString()).toContain('\r\nConnection: close\r\n');
        expect(result.data.toString()).toMatch(/\r\n\r\nbye$/);
        expect(result.ended).toBe(true);
    });

    it('keeps the connection open otherwise', async () => {
        expect((await raw(server.port, get('/hello', ['Host: x', 'Connection: keep-alive']))).ended).toBe(false);
    });

    it('closes only when the connection header is close', async () => {
        expect((await raw(server.port, get('/hello', ['Host: x', 'Connection: nope!']))).ended).toBe(false);
        expect((await raw(server.port, get('/hello', ['Host: x', 'Connection: CLOSE']))).ended).toBe(true);
    });

    it('writes 100 continue before invoking the handler for expect requests', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello');

        expect(result.data.toString().startsWith('HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK')).toBe(true);
        expect(result.data.toString()).toContain('got hello');
    });

    it('does not hang when an Expect request is rejected before its body is read', async () => {
        let result = await raw(server.port, 'POST /reject-expect HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello');

        expect(status(result.data).at(-1)).toBe('HTTP/1.1 413 Payload Too Large');
    });

    it('accepts transfer-encoding combinations as chunked', async () => {
        let gzipChunked = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n'),
            chunkedIdentity = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, identity\r\n\r\n0\r\n\r\n');

        /* Upstream behavior: TE combinations, including non-final chunked, are treated leniently as chunked. */
        expect(status(gzipChunked.data)).toEqual(['HTTP/1.1 200 OK']);
        expect(status(chunkedIdentity.data)).toEqual(['HTTP/1.1 200 OK']);
    });
});

describe('query parser', () => {
    it('pins malformed escapes, key decoding, and empty query fields', async () => {
        let malformed = await request(`${server.url}/query?a=%2`),
            invalid = await request(`${server.url}/query?a=%zz`),
            percent = await request(`${server.url}/query?a=%`),
            keys = await request(`${server.url}/query?a+b=v&a=first&a=second`),
            empty = await request(`${server.url}/query?a&=&`);

        /* Malformed escapes and absent values return undefined, serialized as null in arrays. */
        expect(JSON.parse(malformed.body.toString())).toEqual([null, null, null, null]);
        expect(JSON.parse(invalid.body.toString())).toEqual([null, null, null, null]);
        expect(JSON.parse(percent.body.toString())).toEqual([null, null, null, null]);
        expect(JSON.parse(keys.body.toString())).toEqual(['first', 'v', null, null]);
        expect(JSON.parse(empty.body.toString())).toEqual([null, null, null, null]);
    });
});

describe('bodies', () => {
    it('does not dispatch a pipelined request after an inline body handler closes', async () => {
        let before = helloRequests,
            result = await raw(server.port, 'POST /closer HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx' + get('/hello'));

        expect(status(result.data)).not.toContain('HTTP/1.1 200 OK');
        expect(result.data.toString()).not.toContain('Hello World!');
        expect(helloRequests).toBe(before);

        expect((await request(`${server.url}/hello`)).body.toString()).toBe('Hello World!');
    });

    it('does not dispatch a pipelined request after a split chunked body handler closes', async () => {
        secondDispatched = false;
        let result = await raw(server.port, [
            'POST /chunk-close HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
            '5\r\nhello\r\n0\r\n\r\n' + get('/second-after-chunk')
        ], { delay: 25 });

        expect(secondDispatched).toBe(false);
        expect(result.ended).toBe(true);

        expect((await request(`${server.url}/hello`)).status).toBe(200);
    });

    it('keeps a fixed-length body across segments', async () => {
        let result = await raw(server.port, ['POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n01234', '56789']);

        expect(result.data.toString()).toContain('got 0123456789');
    });

    it('reports chunk boundaries as they arrive', async () => {
        let result = await raw(server.port, ['POST /ondata HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n01234', '56789']);

        expect(result.data.toString()).toMatch(/\r\n\r\n(0,)?5,5!$/);
    });

    it('emits an empty last chunk for bodyless requests', async () => {
        expect((await raw(server.port, 'POST /ondata HTTP/1.1\r\nHost: x\r\n\r\n')).data.toString()).toMatch(/\r\n\r\n0!$/);
    });

    it('decodes chunked bodies with lowercase and uppercase hex sizes', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\na\r\n0123456789\r\nB\r\n0123456789A\r\n0\r\n\r\n');

        expect(result.data.toString()).toContain('got 01234567890123456789A');
    });

    it('decodes chunked bodies split across segments', async () => {
        let result = await raw(server.port, ['POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel', 'lo\r\n5\r\nwor', 'ld\r\n0\r\n\r\n']);

        expect(result.data.toString()).toContain('got helloworld');
    });

    it('rejects chunk data without a CRLF terminator with 400', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n', { timeout: 2000 });

        expect(status(result.data)).toEqual(['HTTP/1.1 400 Bad Request']);
        expect(result.ended).toBe(true);
    }, 3000);

    it('ignores chunk extensions', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n0\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('got hello');
    });

    it('rejects non-hex chunk sizes with 400', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\ng\r\n0123456789abcdef\r\n0\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });

    it('consumes chunked trailers', async () => {
        let result = await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-Trailer: 1\r\n\r\n');

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('got hello');
    });

    it('consumes a split chunked terminator before a pipelined request', async () => {
        let result = await raw(server.port, ['POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r', '\n' + get('/hello')]);

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('got hello');
        expect(result.data.toString()).toContain('Hello World!');
    });

    it('consumes chunked terminators and trailers split at every offset', async () => {
        let request = 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n',
            terminator = '0\r\n\r\n',
            trailer = '0\r\nX-T: 1\r\n\r\n';

        for (let i = 0; i < terminator.length; i++) {
            let result = await raw(server.port, [request + terminator.slice(0, i), terminator.slice(i) + get('/hello')]);

            expect(status(result.data)).toEqual(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
            expect(result.data.toString()).toContain('got hello');
            expect(result.data.toString()).toContain('Hello World!');
        }

        for (let i = 0; i < trailer.length; i++) {
            let result = await raw(server.port, [request + trailer.slice(0, i), trailer.slice(i) + get('/hello')]);

            expect(status(result.data)).toEqual(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
            expect(result.data.toString()).toContain('got hello');
            expect(result.data.toString()).toContain('Hello World!');
        }
    });

    it('consumes a chunked trailer block larger than one segment', async () => {
        let trailer = `X-Long: ${'x'.repeat(3000)}\r\n\r\n`,
            split = Math.floor(trailer.length / 2),
            result = await raw(server.port, ['POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n' + trailer.slice(0, split), trailer.slice(split) + get('/hello')]);

        expect(status(result.data)).toEqual(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
        expect(result.data.toString()).toContain('got hello');
        expect(result.data.toString()).toContain('Hello World!');
    });

    it('rejects non-numeric content lengths with 400', async () => {
        expect(status((await raw(server.port, 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 12a\r\n\r\n')).data)).toEqual(['HTTP/1.1 400 Bad Request']);
    });
});

describe('proxy protocol', () => {
    it('reads PROXY protocol v2 headers', async () => {
        let result = await raw(server.port, Buffer.concat([PROXY_V2, Buffer.from(get('/remote'))]));

        expect(result.data.toString()).toContain('10.0.0.1:12345:4');
    });

    it('serves plain requests on the same context', async () => {
        expect((await raw(server.port, get('/remote'))).data.toString()).toMatch(/\r\n\r\n:0:0$/);
    });
});

describe('chunked responses', () => {
    it('frames beginWrite output without an extra CRLF', async () => {
        let result = await raw(server.port, get('/begin'));

        expect(result.data.toString()).toMatch(/Transfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n$/);
    });
});

describe('multipart parser', () => {
    it('accepts an 11-header part and handles boundary edge cases', () => {
        let boundary = 'space boundary',
            headers = Array.from({ length: 10 }, (_, i) => `X-${i}: v`).join('\r\n'),
            body = `--${boundary}\r\nContent-Disposition: form-data; name="upload"; filename="semi;colon.txt"\r\n${headers}\r\n\r\npayload\r\n--${boundary}--\r\n`,
            longBoundary = 'x'.repeat(70),
            completeLong = `--${longBoundary}\r\nContent-Disposition: form-data; name="a"\r\n\r\nb\r\n--${longBoundary}--\r\n`;

        /* S22 raises the per-part header cap, so this 11-header part is accepted. */
        let parts = getParts(body, `multipart/form-data; boundary="${boundary}"`);
        expect(parts).toHaveLength(1);
        expect(parts?.[0].filename).toBe('semi;colon.txt');
        expect(text(parts![0].data)).toBe('payload');
        /* A valid boundary with no complete part yields an empty array; undefined is reserved for a missing boundary. */
        expect(getParts(`--${boundary}\r\nContent-Disposition: form-data; name="a"\r\n\r\nx`, `multipart/form-data; boundary="${boundary}"`)).toEqual([]);
        expect(getParts(completeLong, `multipart/form-data; boundary=${longBoundary}`)).toHaveLength(1);
        expect(getParts('--short', 'multipart/form-data; boundary=much-longer-boundary')).toEqual([]);
    });
});
