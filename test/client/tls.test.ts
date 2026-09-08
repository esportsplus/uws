import { spawnSync } from 'node:child_process';
import { lookup } from 'node:dns/promises';
import { readFileSync } from 'node:fs';
import { afterEach, describe, expect, it } from 'vitest';
import { App, Client, SSLApp, SSLClient } from '../../src/index';
import { dial, listen, text } from '../harness';
import type { TemplatedApp, TemplatedClient } from '../../src/index';
import type { Server } from '../harness';


const TEST_CA = '.tmp/test-ca/ca.pem';
const LOOPBACK_OPTIONS = {
    cert_file_name: '.tmp/test-ca/loopback.pem',
    key_file_name: '.tmp/test-ca/loopback.key'
};
const LOCALHOST_OPTIONS = {
    cert_file_name: '.tmp/test-ca/localhost.pem',
    key_file_name: '.tmp/test-ca/localhost.key'
};
const localhostHasBothFamilies = await lookup('localhost', { all: true, verbatim: true })
    .then((addresses) => addresses.some(({ family }) => family === 4) && addresses.some(({ family }) => family === 6))
    .catch(() => false);

const defaultCaChildScript = `
import { SSLApp, SSLClient, us_listen_socket_close, us_socket_local_port } from './src/index.ts';

let app = SSLApp({ cert_file_name: '.tmp/test-ca/loopback.pem', key_file_name: '.tmp/test-ca/loopback.key' }).ws('/*', {}),
    socket = await new Promise((resolve) => app.listen(0, resolve));

if (!socket) {
    throw new Error('listen failed');
}

let client = SSLClient(),
    outcome = await new Promise((resolve) => {
        let timer = setTimeout(() => resolve('timeout'), 3000);

        client.connect(\`wss://127.0.0.1:\${us_socket_local_port(socket)}/\`, {
            open: () => {
                clearTimeout(timer);
                resolve('open');
            },
            failed: (error) => {
                clearTimeout(timer);
                resolve(\`failed:\${error.code}\`);
            }
        });
    });

client.close();
us_listen_socket_close(socket);
console.log(outcome);
`;


function build(options: typeof LOOPBACK_OPTIONS | typeof LOCALHOST_OPTIONS): TemplatedApp {
    return SSLApp(options).ws('/echo', {
        message: (ws, data, isBinary) => {
            ws.send(data, isBinary);
        }
    });
}

function startTls(options: typeof LOOPBACK_OPTIONS | typeof LOCALHOST_OPTIONS): Server {
    return listen(build(options), { secure: true });
}


describe('outbound TLS verification', () => {
    let clients: TemplatedClient[] = [];
    let servers: Server[] = [];

    afterEach(() => {
        for (let client of clients) {
            client.close();
        }
        for (let server of servers) {
            server.close();
        }
        clients = [];
        servers = [];
    });

    it('opens with a CA-verified loopback certificate', async () => {
        let server = startTls(LOOPBACK_OPTIONS),
            client = SSLClient({ ca_file_name: TEST_CA }),
            dialed = dial(client, `wss://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened;

        servers.push(server);
        clients.push(client);
        ws.send('verified');
        expect(text((await dialed.next()).data)).toBe('verified');
    });

    it('accepts the CA PEM inline', async () => {
        let server = startTls(LOOPBACK_OPTIONS),
            client = SSLClient({ ca_pem: readFileSync(TEST_CA) }),
            dialed = dial(client, `wss://127.0.0.1:${server.port}/echo`),
            ws = await dialed.opened;

        servers.push(server);
        clients.push(client);
        ws.send('inline-ca');
        expect(text((await dialed.next()).data)).toBe('inline-ca');
    });

    it('uses NODE_EXTRA_CA_CERTS in the default CA set', () => {
        let run = (extraCa: boolean) => {
                let env: NodeJS.ProcessEnv = { ...process.env };

                if (extraCa) {
                    env.NODE_EXTRA_CA_CERTS = TEST_CA;
                }
                else {
                    delete env.NODE_EXTRA_CA_CERTS;
                }

                return spawnSync(process.execPath, ['--import', 'tsx', '--input-type=module', '--eval', defaultCaChildScript], {
                    cwd: process.cwd(),
                    encoding: 'utf8',
                    env,
                    timeout: 10000
                });
            },
            withExtraCa = run(true),
            withoutExtraCa = run(false);

        expect(withExtraCa.status, withExtraCa.stderr).toBe(0);
        expect(withExtraCa.stdout.trim()).toBe('open');
        expect(withoutExtraCa.status, withoutExtraCa.stderr).toBe(0);
        expect(withoutExtraCa.stdout.trim()).toBe('failed:TLS_VERIFY');
    });

    it('reports TLS_VERIFY when a trusted certificate hostname mismatches', async () => {
        let server = startTls(LOCALHOST_OPTIONS),
            client = SSLClient({ ca_file_name: TEST_CA }),
            dialed = dial(client, `wss://127.0.0.1:${server.port}/`),
            failed = await dialed.failed;

        servers.push(server);
        clients.push(client);
        expect(failed.code).toBe('TLS_VERIFY');
    });

    it('uses servername for SNI and certificate hostname verification', async () => {
        let server = startTls(LOCALHOST_OPTIONS),
            client = SSLClient({ ca_file_name: TEST_CA }),
            dialed = dial(client, `wss://127.0.0.1:${server.port}/echo`, { servername: 'localhost' }),
            ws = await dialed.opened;

        servers.push(server);
        clients.push(client);
        ws.send('servername');
        expect(text((await dialed.next()).data)).toBe('servername');
    });

    it('reports TLS_VERIFY when servername does not match', async () => {
        let server = startTls(LOOPBACK_OPTIONS),
            client = SSLClient({ ca_file_name: TEST_CA }),
            dialed = dial(client, `wss://127.0.0.1:${server.port}/`, { servername: 'nomatch.invalid' }),
            failed = await dialed.failed;

        servers.push(server);
        clients.push(client);
        expect(failed.code).toBe('TLS_VERIFY');
    });

    it.skipIf(process.platform === 'darwin')('binds plaintext connections to sourceHost', async () => {
        let remoteAddress = '',
            app = App().ws('/echo', {
                open: (ws) => {
                    remoteAddress = ws.getRemoteAddressAsText();
                },
                message: (ws, data, isBinary) => {
                    ws.send(data, isBinary);
                }
            }),
            server = listen(app, { host: '127.0.0.1' }),
            client = Client(),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/echo`, { sourceHost: '127.0.0.2' }),
            ws = await dialed.opened;

        servers.push(server);
        clients.push(client);
        ws.send('source-host');
        await dialed.next();
        expect(remoteAddress).toBe('127.0.0.2');
    });

    it('reports EADDRNOTAVAIL for an unavailable sourceHost', async () => {
        let server = listen(App().ws('/*', {}), { host: '127.0.0.1' }),
            client = Client(),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/`, { sourceHost: '203.0.113.7' }),
            failed = await dialed.failed;

        servers.push(server);
        clients.push(client);
        expect(failed.code).toBe('EADDRNOTAVAIL');
    });

    it('does not connect unbound when sourceHost is not an address', async () => {
        let opened = false,
            server = listen(App().ws('/*', {}), { host: '127.0.0.1' }),
            client = Client(),
            dialed = dial(client, `ws://127.0.0.1:${server.port}/`, {
                sourceHost: 'not-an-address',
                open: () => { opened = true; }
            }),
            failed = await dialed.failed;

        servers.push(server);
        clients.push(client);
        expect(failed.code).toBe('EADDRNOTAVAIL');
        expect(opened).toBe(false);
    });

    /* macOS only configures 127.0.0.1 on lo0, so binding 127.0.0.2 fails on both families there. */
    it.skipIf(!localhostHasBothFamilies || process.platform === 'darwin')('falls back to IPv4 when sourceHost does not match IPv6', async () => {
        let server = listen(App().ws('/*', {}), { host: '127.0.0.1' }),
            client = Client(),
            dialed = dial(client, `ws://localhost:${server.port}/`, { sourceHost: '127.0.0.2' }),
            ws = await dialed.opened;

        servers.push(server);
        clients.push(client);
        ws.close();
    });
});
