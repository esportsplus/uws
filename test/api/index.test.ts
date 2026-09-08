import { describe, expect, it } from 'vitest';
import { createServer } from 'node:net';
import {
    App,
    DeclarativeResponse,
    DEDICATED_COMPRESSOR,
    DEDICATED_COMPRESSOR_3KB,
    DEDICATED_COMPRESSOR_4KB,
    DEDICATED_COMPRESSOR_8KB,
    DEDICATED_COMPRESSOR_16KB,
    DEDICATED_COMPRESSOR_32KB,
    DEDICATED_COMPRESSOR_64KB,
    DEDICATED_COMPRESSOR_128KB,
    DEDICATED_COMPRESSOR_256KB,
    DEDICATED_DECOMPRESSOR,
    DEDICATED_DECOMPRESSOR_512B,
    DEDICATED_DECOMPRESSOR_1KB,
    DEDICATED_DECOMPRESSOR_2KB,
    DEDICATED_DECOMPRESSOR_4KB,
    DEDICATED_DECOMPRESSOR_8KB,
    DEDICATED_DECOMPRESSOR_16KB,
    DEDICATED_DECOMPRESSOR_32KB,
    DISABLED,
    getParts,
    LIBUS_LISTEN_EXCLUSIVE_PORT,
    SHARED_COMPRESSOR,
    SHARED_DECOMPRESSOR,
    SSLApp,
    us_listen_socket_close,
    us_socket_local_port
} from '../../src/index';
import { connect, listen, request, text } from '../harness';
import type { us_listen_socket } from '../../src/index';


type Encoded = {
    bytes?: number[];
    error?: Error;
};


const APP_METHODS = [
    'addServerName',
    'any',
    'close',
    'connect',
    'del',
    'domain',
    'filter',
    'get',
    'head',
    'listen',
    'listen_unix',
    'missingServerName',
    'numSubscribers',
    'options',
    'patch',
    'post',
    'publish',
    'put',
    'removeServerName',
    'trace',
    'ws'
];

const MULTIPART_BODY = [
    '--xyz',
    'Content-Disposition: form-data; name="field"',
    '',
    'value',
    '--xyz',
    'Content-Disposition: form-data; name="file"; filename="a.txt"',
    'Content-Type: text/plain',
    '',
    'file body',
    '--xyz--',
    ''
].join('\r\n');


function bytes(value: string): number[] {
    return Array.from(Buffer.from(value));
}

function encode(response: DeclarativeResponse, body?: string): Encoded {
    try {
        return { bytes: Array.from(new Uint8Array(response.end(body))) };
    }
    catch (error) {
        return { error: error as Error };
    }
}


describe('exports', () => {
    it('exposes the compression enum with the engine bit layout', () => {
        expect(DISABLED).toBe(0);
        expect(SHARED_COMPRESSOR).toBe(1);
        expect(SHARED_DECOMPRESSOR).toBe(256);
        expect(DEDICATED_COMPRESSOR_3KB).toBe(145);
        expect(DEDICATED_COMPRESSOR_4KB).toBe(146);
        expect(DEDICATED_COMPRESSOR_8KB).toBe(163);
        expect(DEDICATED_COMPRESSOR_16KB).toBe(180);
        expect(DEDICATED_COMPRESSOR_32KB).toBe(197);
        expect(DEDICATED_COMPRESSOR_64KB).toBe(214);
        expect(DEDICATED_COMPRESSOR_128KB).toBe(231);
        expect(DEDICATED_COMPRESSOR_256KB).toBe(248);
        expect(DEDICATED_COMPRESSOR).toBe(248);
        expect(DEDICATED_DECOMPRESSOR_512B).toBe(2304);
        expect(DEDICATED_DECOMPRESSOR_1KB).toBe(2560);
        expect(DEDICATED_DECOMPRESSOR_2KB).toBe(2816);
        expect(DEDICATED_DECOMPRESSOR_4KB).toBe(3072);
        expect(DEDICATED_DECOMPRESSOR_8KB).toBe(3328);
        expect(DEDICATED_DECOMPRESSOR_16KB).toBe(3584);
        expect(DEDICATED_DECOMPRESSOR_32KB).toBe(3840);
        expect(DEDICATED_DECOMPRESSOR).toBe(3840);
    });

    it('exposes listen options', () => {
        expect(LIBUS_LISTEN_EXCLUSIVE_PORT).toBe(1);
    });

    it('exposes constructors and helpers as functions', () => {
        expect(typeof App).toBe('function');
        expect(typeof SSLApp).toBe('function');
        expect(typeof getParts).toBe('function');
        expect(typeof us_listen_socket_close).toBe('function');
        expect(typeof us_socket_local_port).toBe('function');
    });
});

describe('App', () => {
    it('returns a builder with the templated app surface', () => {
        let app = App() as unknown as Record<string, unknown>;

        for (let i = 0, n = APP_METHODS.length; i < n; i++) {
            expect(typeof app[APP_METHODS[i]], APP_METHODS[i]).toBe('function');
        }
    });

    it('returns this from route registration', () => {
        let app = App();

        expect(app.get('/', (res) => {
            res.end();
        })).toBe(app);
    });

    it('rejects non-object options', () => {
        expect(() => App('x' as never)).toThrow('Options must be an object.');
    });

    it('constructs an SSLApp without a certificate', () => {
        expect(typeof SSLApp({}).get).toBe('function');
    });

    it('throws when the certificate cannot be loaded', () => {
        expect(() => SSLApp({ cert_file_name: '.tmp/missing.pem', key_file_name: '.tmp/missing.pem' })).toThrow('App construction failed');
    });

    it('throws when a route handler is not a function', () => {
        expect(() => App().get('/', 5 as never)).toThrow('Passed callback is not a valid function.');
    });

    it('throws when a pattern is not a string or buffer', () => {
        expect(() => App().get(5 as never, () => {})).toThrow('Text and data can only be passed by String, ArrayBuffer or ArrayBufferView.');
    });

    it('throws when ws is called without a behavior', () => {
        expect(() => (App() as unknown as { ws: (pattern: string) => void }).ws('/')).toThrow('Function requires at least 2 arguments.');
    });

    it('rejects invalid WebSocket idleTimeout values without aborting', () => {
        expect(() => App().ws('/', { idleTimeout: 5, open() {}, message() {} })).toThrow('idleTimeout');
        expect(() => App().ws('/', { idleTimeout: 16, open() {}, message() {} })).not.toThrow();
    });

    it('rejects invalid callback and WebSocket behavior arguments without aborting', () => {
        expect(() => App().ws('/', null as never)).toThrowError();
        expect(() => App().ws('/', { open: 'x' } as never)).toThrowError();
        expect(() => App().ws('/', { idleTimeout: Symbol() } as never)).toThrowError();
        expect(() => (App() as unknown as { listen: (host: string, port: number) => void }).listen('127.0.0.1', 0)).toThrowError();
        expect(() => App().missingServerName(5 as never)).toThrowError();
    });

    it('rejects invalid request, response and WebSocket callbacks without aborting', async () => {
        let errors: unknown[] = [],
            app = App();

        app.get('/on-aborted', (res) => {
            try { res.onAborted('x' as never); }
            catch (error) { errors.push(error); }
            res.end();
        });
        app.get('/collect-body', (res) => {
            try { res.collectBody(NaN, () => {}); }
            catch (error) { errors.push(error); }
            res.end();
        });
        app.get('/cork', (res) => {
            try { res.cork(5 as never); }
            catch (error) { errors.push(error); }
            res.end();
        });
        app.get('/for-each', (res, req) => {
            try { req.forEach(1 as never); }
            catch (error) { errors.push(error); }
            res.end();
        });
        app.ws('/ws-cork', {
            open: (ws) => {
                try { ws.cork(1 as never); }
                catch (error) { errors.push(error); }
            }
        });

        let server = listen(app);
        try {
            await request(`${server.url}/on-aborted`);
            await request(`${server.url}/collect-body`);
            await request(`${server.url}/cork`);
            await request(`${server.url}/for-each`);
            let ws = await connect(server.url.replace('http', 'ws') + '/ws-cork');
            ws.terminate();

            expect(errors).toHaveLength(5);
            for (let error of errors) {
                expect(error).toBeInstanceOf(Error);
            }
        }
        finally {
            server.close();
        }
    });

    it('throws when publish is called without a message', () => {
        expect(() => (App() as unknown as { publish: (topic: string) => void }).publish('t')).toThrow('Function requires at least 2 arguments.');
    });

    it('returns empty topic results before a WebSocket route is registered', () => {
        expect(App().publish('t', 'm')).toBe(false);
        expect(App().numSubscribers('t')).toBe(0);
    });

    it('reports the connectable listen token port over IPv4 and IPv6', async () => {
        let verify = async (host: string, urlHost: string): Promise<void> => {
            let token: us_listen_socket | false = false,
                app = App().get('/*', (res) => { res.end('ok'); });

            app.listen(host, 0, (socket) => { token = socket; });
            expect(token, `${host} listen failed`).toBeTruthy();

            let socket = token as us_listen_socket,
                port = us_socket_local_port(socket);
            try {
                expect((await request(`http://${urlHost}:${port}/`)).body.toString()).toBe('ok');
            }
            finally {
                us_listen_socket_close(socket);
            }
        };

        await verify('127.0.0.1', '127.0.0.1');

        let ipv6 = await new Promise<boolean>((resolve) => {
            let probe = createServer();

            probe.once('error', () => resolve(false));
            probe.listen(0, '::1', () => {
                probe.close(() => resolve(true));
            });
        });

        if (ipv6) {
            await verify('::1', '[::1]');
        }
    });
});

describe('DeclarativeResponse', () => {
    it('encodes instructions as opcode, length and bytes', () => {
        let response = new DeclarativeResponse(),
            encoded: Encoded;

        try {
            response.writeStatus('201 Created').writeHeader('a', 'bc').writeQueryValue('q').writeHeaderValue('h').writeParameterValue('p').writeBody().write('mid');
            encoded = encode(response, '!');
        }
        catch (error) {
            encoded = { error: error as Error };
        }

        expect(encoded.error).toBeUndefined();
        expect(encoded.bytes).toEqual([
            7, 11, ...bytes('201 Created'),
            1, 1, 97, 2, 98, 99,
            3, 1, 113,
            4, 1, 104,
            6, 1, 112,
            2,
            5, 3, 0, ...bytes('mid'),
            0, 1, 0, 33
        ]);
    });

    it('encodes multi-byte strings as utf-8', () => {
        expect(encode(new DeclarativeResponse(), 'é').bytes).toEqual([0, 2, 0, 0xc3, 0xa9]);
    });

    it('encodes an empty end', () => {
        expect(encode(new DeclarativeResponse()).bytes).toEqual([0, 0, 0]);
    });

    it('rejects header values longer than 255 bytes', () => {
        let encoded: Encoded;

        try {
            encoded = encode(new DeclarativeResponse().writeHeader('a', 'x'.repeat(256)));
        }
        catch (error) {
            encoded = { error: error as Error };
        }

        expect(encoded.error).toBeInstanceOf(RangeError);
        expect(encoded.error?.message).toBe('uws: data length exceeds 255');
    });

    it('rejects bodies longer than 65535 bytes', () => {
        let encoded = encode(new DeclarativeResponse(), 'x'.repeat(65536));

        expect(encoded.error).toBeInstanceOf(RangeError);
        expect(encoded.error?.message).toBe('uws: data length exceeds 65535');
    });
});

describe('getParts', () => {
    it('parses fields and files', () => {
        let parts = getParts(MULTIPART_BODY, 'multipart/form-data; boundary=xyz');

        expect(parts).toHaveLength(2);
        expect(parts?.[0].name).toBe('field');
        expect(parts?.[0].filename).toBeUndefined();
        expect(parts?.[0].type).toBeUndefined();
        expect(text(parts![0].data)).toBe('value');
        expect(parts?.[1].name).toBe('file');
        expect(parts?.[1].filename).toBe('a.txt');
        expect(parts?.[1].type).toBe('text/plain');
        expect(text(parts![1].data)).toBe('file body');
    });

    it('accepts buffers as body', () => {
        let parts = getParts(Buffer.from(MULTIPART_BODY), 'multipart/form-data; boundary=xyz');

        expect(parts).toHaveLength(2);
        expect(text(parts![1].data)).toBe('file body');
    });

    it('returns undefined for non-multipart content types', () => {
        expect(getParts(MULTIPART_BODY, 'text/plain')).toBeUndefined();
    });

    it('returns undefined when the boundary is missing', () => {
        expect(getParts(MULTIPART_BODY, 'multipart/form-data')).toBeUndefined();
    });

    it('parses the multipart boundary parameter', () => {
        expect(getParts(MULTIPART_BODY, 'multipart/form-data; boundary= xyz ; charset=utf-8')).toHaveLength(2);
        expect(getParts(MULTIPART_BODY, 'multipart/form-data; boundary= "xyz" ')).toHaveLength(2);
    });
});
