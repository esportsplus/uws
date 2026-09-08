import { describe, expect, it } from 'vitest';
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { Agent, request as httpsRequest } from 'node:https';
import { connect as tlsConnect } from 'node:tls';
import { SSLApp } from '../../src/index';
import { connect, listen, loopback, message, raw, request } from '../harness';


const OPTIONS = {
    cert_file_name: '.tmp/cert.pem',
    key_file_name: '.tmp/key.pem',
    passphrase: '1234'
};
const TEST_CA = '.tmp/test-ca/ca.pem';
const UNTRUSTED_CLIENT = {
    cert: '.tmp/cert.pem',
    key: '.tmp/key.pem',
    passphrase: '1234'
};
type ClientCertificate = Partial<typeof UNTRUSTED_CLIENT>;

const missingServerNameThrowChildScript = `
import { request } from 'node:https';
import { SSLApp, us_listen_socket_close, us_socket_local_port } from './src/index.ts';

let app = SSLApp({ cert_file_name: '.tmp/cert.pem', key_file_name: '.tmp/key.pem', passphrase: '1234' }),
    token = await new Promise((resolve) => app.listen(0, resolve)),
    threw = false;

if (!token) throw new Error('listen failed');
app.domain('').get('/*', (res) => res.end('default'));
app.missingServerName(() => { throw new Error('missing server name'); });
process.once('uncaughtException', (error) => {
    if (error.message !== 'missing server name') throw error;
    threw = true;
});
await new Promise((resolve) => {
    let req = request({ host: '127.0.0.1', port: us_socket_local_port(token), path: '/', servername: 'throws.test', rejectUnauthorized: false }, (res) => {
        res.resume();
        res.once('end', resolve);
    });
    req.once('error', resolve);
    req.end();
});
us_listen_socket_close(token);
console.log(JSON.stringify({ threw }));
`;


function requestWithCertificate(url: string, certificate: ClientCertificate = {}): Promise<string> {
    let timer: NodeJS.Timeout | undefined;
    return new Promise<string>((resolve, reject) => {
        let req = httpsRequest(url, {
            ...certificate,
            cert: certificate.cert ? readFileSync(certificate.cert) : undefined,
            key: certificate.key ? readFileSync(certificate.key) : undefined,
            agent: false,
            rejectUnauthorized: false
        }, (res) => {
            let body = '';

            res.setEncoding('utf8');
            res.on('data', (chunk) => body += chunk);
            res.on('end', () => resolve(body));
            res.on('error', reject);
            res.on('aborted', () => reject(new Error('TLS response aborted')));
        });

        // Bound the entire handshake and response, including peers that never send an event.
        timer = setTimeout(() => {
            let error = new Error('TLS request timed out');
            reject(error);
            req.destroy(error);
        }, 2000);
        req.on('error', reject);
        req.end();
    }).finally(() => clearTimeout(timer));
}

function rejectedTls11(port: number): Promise<Error> {
    return new Promise((resolve, reject) => {
        let socket = tlsConnect({ host: '127.0.0.1', port, rejectUnauthorized: false, secureProtocol: 'TLSv1_1_method' });

        socket.once('secureConnect', () => {
            socket.destroy();
            reject(new Error('TLS 1.1 unexpectedly connected'));
        });
        socket.once('error', (error) => resolve(error));
    });
}


describe('https', () => {
    it('serves requests over tls', async () => {
        let server = listen(SSLApp(OPTIONS).get('/*', (res) => {
            res.end('secure');
        }), { secure: true });

        let response = await request(`${server.url}/`);

        expect(response.status).toBe(200);
        expect(response.body.toString()).toBe('secure');
        server.close();
    });

    it('accepts the low memory option', async () => {
        let server = listen(SSLApp({ ...OPTIONS, ssl_prefer_low_memory_usage: true }).get('/*', (res) => {
            res.end('low');
        }), { secure: true });

        expect((await request(`${server.url}/`)).body.toString()).toBe('low');
        server.close();
    });

    it('reports the remote address and an empty client certificate', async () => {
        let server = listen(SSLApp(OPTIONS).get('/*', (res) => {
            res.end(JSON.stringify({ cert: res.getX509Certificate(), text: res.getRemoteAddressAsText() }));
        }), { secure: true });

        let parsed = JSON.parse((await request(`${server.url}/`)).body.toString()) as { cert: string; text: string };

        expect(parsed.cert).toBe('');
        expect(loopback(parsed.text)).toBe(true);
        server.close();
    });

    it('requests and verifies optional client certificates', async () => {
        let server = listen(SSLApp({ ...OPTIONS, ca_file_name: TEST_CA }).get('/*', (res) => {
            res.end(res.getX509Certificate());
        }), { secure: true });

        try {
            // ca_file_name enables SSL_VERIFY_PEER, not SSL_VERIFY_FAIL_IF_NO_PEER_CERT.
            expect(await requestWithCertificate(`${server.url}/`)).toBe('');
            // Only a TLS rejection/reset counts; a deadline or local PEM parsing error must fail.
            await expect(requestWithCertificate(`${server.url}/`, UNTRUSTED_CLIENT)).rejects.toMatchObject({
                code: expect.stringMatching(/^(ERR_SSL_|EPROTO$|ECONNRESET$)/)
            });
            // A fresh connection must still reach the handler after the rejected peer.
            expect(await requestWithCertificate(`${server.url}/`)).toBe('');
        }
        finally {
            server.close();
        }
    });

    // scripts/gen-test-certs.sh gives localhost/loopback only serverAuth, not clientAuth.
    // Skip the non-empty getX509Certificate assertion until a clientAuth fixture exists;
    // generating one here would require an external OpenSSL CLI unavailable on some hosts.
    it.skip('returns a non-empty certificate for a valid optional client certificate', () => {});

    it('accepts an unused passphrase for an unencrypted private key', async () => {
        // .tmp/key.pem is BEGIN PRIVATE KEY, not ENCRYPTED PRIVATE KEY: the password
        // callback installed by create_ssl_context_from_options is never needed.
        let server = listen(SSLApp({ ...OPTIONS, passphrase: 'wrong' }).get('/*', (res) => {
            res.end('secure');
        }), { secure: true });

        try {
            expect(await requestWithCertificate(`${server.url}/`)).toBe('secure');
        }
        finally {
            server.close();
        }
    });

    it('rejects invalid TLS construction options', () => {
        // create_ssl_context_from_options returns NULL on fopen or cipher-list failure;
        // AppWrapper translates constructorFailed() into this exception.
        expect(() => SSLApp({ ...OPTIONS, dh_params_file_name: '.tmp/missing-dh.pem' })).toThrow('App construction failed');
        expect(() => SSLApp({ ...OPTIONS, ssl_ciphers: 'NONSENSE' })).toThrow('App construction failed');
    });

    describe('throwing option getters', () => {
        it('throws when the SSLApp key_file_name getter throws', () => {
            let app: ReturnType<typeof SSLApp> | undefined;

            try {
                expect(() => {
                    app = SSLApp({
                        ...OPTIONS,
                        get key_file_name(): never { throw new Error('boom'); }
                    });
                }).toThrow('boom');
            }
            finally {
                app?.close();
            }
        });

        it('throws when the SSLApp reject_unauthorized getter throws', () => {
            let app: ReturnType<typeof SSLApp> | undefined;

            try {
                expect(() => {
                    app = SSLApp({
                        ...OPTIONS,
                        get reject_unauthorized(): never { throw new Error('boom'); }
                    });
                }).toThrow('boom');
            }
            finally {
                app?.close();
            }
        });
    });

    it('closes plaintext and TLS 1.1 connections while continuing to serve TLS', async () => {
        let server = listen(SSLApp(OPTIONS).get('/*', (res) => {
            res.end('secure');
        }), { secure: true });

        try {
            expect((await raw(server.port, 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')).ended).toBe(true);
            // OpenSSL rejects the legacy ClientHello; the exact alert varies by OpenSSL version.
            expect(await rejectedTls11(server.port)).toBeInstanceOf(Error);
            expect((await request(`${server.url}/`)).body.toString()).toBe('secure');
        }
        finally {
            server.close();
        }
    });

    it('listens without a certificate but rejects every TLS connection', async () => {
        let server = listen(SSLApp({}).get('/*', (res) => {
            res.end('unexpected');
        }), { secure: true });

        try {
            // Current OpenSSL context creation permits this, but no TLS handshake can complete.
            await expect(request(`${server.url}/`)).rejects.toBeInstanceOf(Error);
        }
        finally {
            server.close();
        }
    });

    it('keeps a large TLS upload intact when onData writes synchronously', async () => {
        const uploadSize = 8 * 1024 * 1024;
        let received = 0;
        let server = listen(SSLApp(OPTIONS).post('/*', (res) => {
            res.onData((chunk, isLast) => {
                received += chunk.byteLength;
                res.cork(() => res.write('x'.repeat(64 * 1024)));

                if (isLast) {
                    res.end('total:' + received);
                }
            });
        }), { secure: true });

        let response = await new Promise<string>((resolve, reject) => {
            let req = httpsRequest(server.url, {
                method: 'POST',
                rejectUnauthorized: false,
                headers: { 'content-length': uploadSize }
            }, (res) => {
                let body = '';
                res.setEncoding('utf8');
                res.on('data', (chunk) => body += chunk);
                res.on('end', () => resolve(body));
                res.on('error', (error) => {
                    req.destroy();
                    reject(error);
                });
            });

            req.on('close', () => {
                req.destroy();
            });
            req.on('error', (error) => {
                req.destroy();
                reject(error);
            });
            let upload = Buffer.alloc(uploadSize, 'u');
            for (let offset = 0; offset < upload.length; offset += 512 * 1024) {
                req.write(upload.subarray(offset, offset + 512 * 1024));
            }
            req.end();
        });

        expect(received).toBe(uploadSize);
        expect(response.endsWith('total:8388608')).toBe(true);
        server.close();
    });

    it('echoes websocket messages over tls', async () => {
        let server = listen(SSLApp(OPTIONS).ws('/*', {
            message: (ws, data, isBinary) => {
                ws.send(data, isBinary);
            }
        }), { secure: true });

        let ws = await connect(`wss://127.0.0.1:${server.port}/`, { rejectUnauthorized: false }),
            reply = message(ws);

        ws.send('wss');

        expect((await reply).data.toString()).toBe('wss');
        ws.close();
        server.close();
    });
});

describe('server names', () => {
    it('throws when adding a server name with invalid TLS options', () => {
        let app = SSLApp(OPTIONS);

        expect(() => app.addServerName('x.test', {
            cert_file_name: '.tmp/missing.pem',
            key_file_name: '.tmp/missing.pem'
        })).toThrow('App: addServerName failed');
    });

    it('throws when adding an existing server name', () => {
        let app = SSLApp(OPTIONS);

        app.addServerName('example.test', OPTIONS);
        expect(() => app.addServerName('example.test', OPTIONS)).toThrow('App: addServerName failed');
    });

    it('throws when adding a server name with more than 10 labels', () => {
        let app = SSLApp(OPTIONS);

        expect(() => app.addServerName('a.b.c.d.e.f.g.h.i.j.k', OPTIONS)).toThrow();
        expect(() => app.addServerName('example.test', OPTIONS)).not.toThrow();
    });

    it('routes requests by server name and falls back to the default router', async () => {
        let app = SSLApp(OPTIONS),
            missing: string[] = [];

        app.addServerName('example.test', OPTIONS);
        app.domain('example.test').get('/*', (res) => {
            res.end('domain');
        });
        app.domain('').get('/*', (res) => {
            res.end('default');
        });
        app.missingServerName((hostname) => {
            missing.push(hostname);
        });

        let server = listen(app, { secure: true });

        expect((await request(`${server.url}/`, { servername: 'example.test' })).body.toString()).toBe('domain');
        expect((await request(`${server.url}/`)).body.toString()).toBe('default');
        expect((await request(`${server.url}/`, { servername: 'other.test' })).body.toString()).toBe('default');
        expect(missing).toEqual(['other.test']);

        app.removeServerName('example.test');

        expect((await request(`${server.url}/`, { servername: 'example.test' })).body.toString()).toBe('default');
        expect(missing).toEqual(['other.test', 'example.test']);
        server.close();
    });

    it('lets missingServerName add the name synchronously', async () => {
        let app = SSLApp(OPTIONS),
            seen: string[] = [];

        app.missingServerName((hostname) => {
            seen.push(hostname);
            app.addServerName(hostname, OPTIONS);
            app.domain(hostname).get('/*', (res) => {
                res.end(`late ${hostname}`);
            });
        });
        app.domain('').get('/*', (res) => {
            res.end('default');
        });

        let server = listen(app, { secure: true });

        expect((await request(`${server.url}/`, { servername: 'late.test' })).body.toString()).toBe('late late.test');
        expect((await request(`${server.url}/`, { servername: 'late.test' })).body.toString()).toBe('late late.test');
        expect(seen).toEqual(['late.test']);
        server.close();
    });

    it('pins a thrown missingServerName callback without crashing the process', () => {
        let child = spawnSync(process.execPath, ['--import', 'tsx', '--input-type=module', '--eval', missingServerNameThrowChildScript], {
            cwd: process.cwd(),
            encoding: 'utf8',
            timeout: 10000
        });

        expect(child.status, child.stderr).toBe(0);
        expect(JSON.parse(child.stdout) as { threw: boolean }).toEqual({ threw: true });
    });

    it('survives a missingServerName callback that closes the app', async () => {
        let app = SSLApp(OPTIONS);

        app.domain('').get('/*', (res) => {
            res.end('default');
        });
        app.missingServerName(() => {
            app.close();
        });

        let server = listen(app, { secure: true });
        try {
            // Closing from the SNI callback aborts the in-flight handshake rather than crashing.
            await expect(request(`${server.url}/`, { servername: 'close.test' })).rejects.toBeInstanceOf(Error);
            await expect(request(`${server.url}/`, { servername: 'close.test' })).rejects.toBeInstanceOf(Error);
        }
        finally {
            // app.close() invalidates the token; its close helper is intentionally idempotent.
            server.close();
        }
    });

    it('frees an established peer eagerly when a missingServerName callback closes the app', async () => {
        let app = SSLApp(OPTIONS),
            closing = false;

        app.domain('').get('/*', (res) => {
            res.end('default');
        });
        app.missingServerName(() => {
            if (closing) {
                return;
            }

            closing = true;
            app.close();
        });

        let server = listen(app, { secure: true });
        try {
            let established = await request(`${server.url}/`);

            expect(established.body.toString()).toBe('default');
            await expect(request(`${server.url}/`, { servername: 'close.test' })).rejects.toBeInstanceOf(Error);
            await expect(request(`${server.url}/`, { servername: 'close.test' })).rejects.toBeInstanceOf(Error);
        }
        finally {
            server.close();
        }
    });

    it('falls back to the default router on a keep-alive connection after removing its server name', async () => {
        let app = SSLApp(OPTIONS),
            agent = new Agent({ keepAlive: true });

        app.addServerName('example.test', OPTIONS);
        app.domain('example.test').get('/*', (res) => {
            res.end('domain');
        });
        app.domain('').get('/*', (res) => {
            res.end('default');
        });

        let server = listen(app, { secure: true });
        let requestWithAgent = () => new Promise<string>((resolve, reject) => {
            let req = httpsRequest(server.url, {
                agent,
                rejectUnauthorized: false,
                servername: 'example.test'
            }, (res) => {
                let body = '';
                res.setEncoding('utf8');
                res.on('data', (chunk) => body += chunk);
                res.on('end', () => resolve(body));
            });
            req.on('close', () => {
                req.destroy();
            });
            req.on('error', (error) => {
                req.destroy();
                reject(error);
            });
            req.end();
        });

        expect(await requestWithAgent()).toBe('domain');
        app.removeServerName('example.test');
        expect(await requestWithAgent()).toBe('default');

        agent.destroy();
        server.close();
    });
});
