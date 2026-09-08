import WebSocket from 'ws';
import { App, us_socket_local_port } from '../src/index';
import type { Socket } from 'node:net';


const reasons = {
    invalidFrame: 'Received invalid WebSocket frame',
    tcpFinBeforeClose: 'Received TCP FIN before WebSocket close frame',
    timeout: 'WebSocket timed out from inactivity'
};
const app = App();
const clients: WebSocket[] = [];
const serverClosed = new Set<string>();
const clientClosed = new Set<string>();
let finish!: () => void;
let fail!: (error: Error) => void;
const completed = new Promise<void>((resolve, reject) => { finish = resolve; fail = reject; });

function checkCompletion(): void {
    if (serverClosed.size === 3 && clientClosed.size === 3) {
        finish();
    }
}

for (const [name, expected] of Object.entries(reasons)) {
    app.ws(`/${name}`, {
        idleTimeout: 8,
        sendPingsAutomatically: false,
        close: (_ws, code, message) => {
            const reason = Buffer.from(message).toString();

            if (code !== 1006 || reason !== expected || serverClosed.has(name)) {
                fail(new Error(`[uws/smoke] ${name}: unexpected close ${code}: ${reason}`));
                return;
            }

            serverClosed.add(name);
            console.log(`[uws/smoke] Passed: ${name}`);
            checkCompletion();
        }
    });
}

const deadline = setTimeout(() => {
    fail(new Error(`[uws/smoke] Timed out waiting for all three closures (server=${serverClosed.size}, client=${clientClosed.size})`));
}, 16000);

try {
    app.listen('127.0.0.1', 0, (token) => {
        if (!token) {
            fail(new Error('[uws/smoke] Failed to listen'));
            return;
        }

        for (const name of Object.keys(reasons)) {
            const client = new WebSocket(`ws://127.0.0.1:${us_socket_local_port(token)}/${name}`);
            clients.push(client);
            client.on('open', () => {
                const socket = (client as WebSocket & { _socket: Socket })._socket;

                if (name === 'invalidFrame') {
                    socket.write(Buffer.from([0xFF, 0x80, 0x00, 0x00, 0x00, 0x01]));
                }
                else if (name === 'tcpFinBeforeClose') {
                    socket.end();
                }
            });
            client.on('close', (code) => {
                if (code !== 1006) {
                    fail(new Error(`[uws/smoke] ${name}: unexpected client close ${code}`));
                    return;
                }

                clientClosed.add(name);
                checkCompletion();
            });
            client.on('error', (error) => fail(new Error(`[uws/smoke] ${name}: ${error.message}`)));
        }
    });

    await completed;
    console.log('[uws/smoke] All three closure checks passed.');
}
finally {
    clearTimeout(deadline);
    for (const client of clients) {
        client.terminate();
    }
    app.close();
}
