import { beforeAll, describe, expect, it } from 'vitest';
import { lookup } from 'node:dns/promises';
import { createServer } from 'node:net';
import { App, Client } from '../../src/index';
import { dial, listen, startConnectProxy, text } from '../harness';


async function probeIpv6(): Promise<boolean> {
    return new Promise((resolve) => {
        let server = createServer(),
            done = false,
            finish = (available: boolean): void => {
                if (!done) {
                    done = true;
                    resolve(available);
                }
            };

        server.once('error', () => { finish(false); });
        server.listen(0, '::1', () => {
            server.close(() => { finish(true); });
        });
    });
}

async function probeLocalhostFamilies(): Promise<boolean> {
    try {
        let addresses = await lookup('localhost', { all: true, order: 'verbatim' });

        return addresses[0]?.family === 6 && addresses.some(({ family }) => family === 4);
    }
    catch {
        return false;
    }
}

async function unusedPort(): Promise<number> {
    return new Promise((resolve, reject) => {
        let server = createServer();

        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            let address = server.address();

            if (!address || typeof address === 'string') {
                reject(new Error('could not allocate a TCP port'));
                return;
            }

            server.close((error) => {
                if (error) {
                    reject(error);
                }
                else {
                    resolve(address.port);
                }
            });
        });
    });
}

function build() {
    return App().ws('/echo', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    });
}


/* skipIf is evaluated during test collection, before beforeAll hooks run. Probe here
 * as well so the collection-time guards reflect the host capabilities; beforeAll
 * repeats both probes as the suite's runtime guard. */
let ipv6Available = await probeIpv6();
let localhostHasBothFamilies = await probeLocalhostFamilies();

beforeAll(async () => {
    ipv6Available = await probeIpv6();
    localhostHasBothFamilies = await probeLocalhostFamilies();

    if (process.env.CI && process.platform === 'linux') {
        expect(localhostHasBothFamilies).toBe(true);
    }
});

describe('outbound address fallback and IPv6', () => {
    it.skipIf(!ipv6Available)('opens and echoes over IPv6 loopback', async () => {
        let server = listen(build(), { host: '::1' }),
            client = Client();

        try {
            let dialed = dial(client, `ws://[::1]:${server.port}/echo`),
                ws = await dialed.opened;

            ws.send('ipv6');
            expect(text((await dialed.next()).data)).toBe('ipv6');
            ws.close();
            await dialed.closed;
        }
        finally {
            client.close();
            server.close();
        }
    });

    it.skipIf(!ipv6Available)('parses a bracketed IPv6 host with the default port', async () => {
        let client = Client();

        try {
            let error = await dial(client, 'ws://[::1]/', { connectTimeout: 1000 }).failed;

            // Hosted Windows runners may have an HTTP service listening on the default port.
            expect(['ECONNREFUSED', 'HTTP_STATUS']).toContain(error.code);
        }
        finally {
            client.close();
        }
    });

    it.skipIf(!localhostHasBothFamilies)('falls back from refused IPv6 localhost to IPv4', async () => {
        let server = listen(build(), { host: '127.0.0.1' }),
            client = Client();

        try {
            let dialed = dial(client, `ws://localhost:${server.port}/echo`, {
                open: (ws) => {
                    expect(ws.getRemoteAddressAsText()).toBe('127.0.0.1');
                }
            }),
                ws = await dialed.opened;

            ws.send('fallback');
            expect(text((await dialed.next()).data)).toBe('fallback');
            ws.close();
            await dialed.closed;
        }
        finally {
            client.close();
            server.close();
        }
    });

    it.skipIf(!localhostHasBothFamilies)('reports the final refused address when fallback is exhausted', async () => {
        let client = Client(),
            port = await unusedPort();

        try {
            let error = await dial(client, `ws://localhost:${port}/`, { connectTimeout: 1000 }).failed;

            expect(error.code).toBe('ECONNREFUSED');
        }
        finally {
            client.close();
        }
    });

    it.skipIf(!ipv6Available)('tunnels to an IPv6 destination through an HTTP CONNECT proxy', async () => {
        let server = listen(build(), { host: '::1' }),
            proxy = await startConnectProxy(),
            client = Client({ proxy: proxy.url });

        try {
            let dialed = dial(client, `ws://[::1]:${server.port}/echo`),
                ws = await dialed.opened;

            ws.send('proxy-ipv6');
            expect(text((await dialed.next()).data)).toBe('proxy-ipv6');
            expect(proxy.seen.url).toBe(`[::1]:${server.port}`);
            ws.close();
            await dialed.closed;
        }
        finally {
            client.close();
            proxy.close();
            server.close();
        }
    });
});
