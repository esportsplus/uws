import { createConnection, createServer, type Socket } from 'node:net';
import { Worker } from 'node:worker_threads';
import { WebSocket, WebSocketServer } from 'ws';


type Scenario = 'resolving' | 'connecting' | 'handshaking' | 'open' | 'cooperative'
    | 'server-listening' | 'server-http-pending' | 'server-ws-open' | 'server-tls-handshaking' | 'server-cooperative';


let scenario = process.argv[2] as Scenario;

if (!['resolving', 'connecting', 'handshaking', 'open', 'cooperative', 'server-listening', 'server-http-pending', 'server-ws-open', 'server-tls-handshaking', 'server-cooperative'].includes(scenario)) {
    throw new Error('expected a teardown scenario');
}

function waitForMessage(worker: Worker): Promise<string> {
    return new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
        worker.once('exit', (code) => reject(new Error(`worker exited before ready: ${code}`)));
    });
}

function waitForExit(worker: Worker): Promise<number> {
    return new Promise((resolve, reject) => {
        worker.once('exit', resolve);
        worker.once('error', reject);
    });
}

async function main(): Promise<void> {
    let rawServer: ReturnType<typeof createServer> | undefined,
        wsServer: WebSocketServer | undefined,
        webSocket: WebSocket | undefined,
        sockets: Socket[] = [],
        port: number | undefined;

    if (scenario === 'handshaking') {
        rawServer = createServer((socket) => {
            sockets.push(socket);
            socket.resume();
            /* Accept the TCP connection but never answer the WebSocket handshake. */
        });
        await new Promise<void>((resolve) => rawServer!.listen(0, '127.0.0.1', resolve));
        port = (rawServer.address() as import('node:net').AddressInfo).port;
    }
    else if (scenario === 'open' || scenario === 'cooperative') {
        wsServer = new WebSocketServer({ port: 0, host: '127.0.0.1' });
        wsServer.on('connection', (socket) => {
            socket.on('message', (message, binary) => socket.send(message, { binary }));
        });
        await new Promise<void>((resolve) => wsServer!.once('listening', resolve));
        port = (wsServer.address() as import('node:net').AddressInfo).port;
    }

    let worker = new Worker(new URL('./teardown-worker.ts', import.meta.url), {
        workerData: { scenario, port },
        execArgv: ['--import', 'tsx']
    }), exited = waitForExit(worker);

    try {
        let message = await waitForMessage(worker);
        if (scenario.startsWith('server-')) {
            port = Number(message.slice('listening:'.length));
            if (!Number.isInteger(port)) {
                throw new Error('server worker did not report a port');
            }

            if (scenario === 'server-http-pending') {
                let ready = waitForMessage(worker);
                sockets.push(createConnection({ host: '127.0.0.1', port }));
                sockets[0].write('GET / HTTP/1.1\r\nHost: localhost\r\n\r\n');
                await ready;
            }
            else if (scenario === 'server-ws-open') {
                webSocket = new WebSocket(`ws://127.0.0.1:${port}/`);
                let ready = waitForMessage(worker),
                    opened = new Promise<void>((resolve, reject) => {
                    webSocket!.once('open', resolve);
                    webSocket!.once('error', reject);
                });
                await Promise.all([ready, opened]);
            }
            else if (scenario === 'server-tls-handshaking') {
                /* More than one prefix exhausts the low-priority budget, leaving at least one TLS
                 * handshake parked outside context->head_sockets when the worker is terminated. */
                for (let i = 0; i < 6; i++) {
                    let socket = createConnection({ host: '127.0.0.1', port });
                    sockets[sockets.length] = socket;
                    socket.write(Buffer.from([0x16, 0x03, 0x01, 0x00]));
                }
                await new Promise((resolve) => setTimeout(resolve, 50));
            }

            if (scenario === 'server-cooperative') {
                if (await exited !== 0) {
                    process.exitCode = 1;
                }
            }
            else {
                await worker.terminate();
            }
        }
        else if (scenario === 'cooperative') {
            if (await exited !== 0) {
                process.exitCode = 1;
            }
        }
        else {
            await worker.terminate();
        }
    }
    finally {
        await worker.terminate();
        for (let socket of sockets) {
            socket.destroy();
        }
        webSocket?.close();
        await new Promise<void>((resolve) => rawServer ? rawServer.close(() => resolve()) : resolve());
        await new Promise<void>((resolve) => wsServer ? wsServer.close(() => resolve()) : resolve());
    }
}

await main();
