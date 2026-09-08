import WebSocket from 'ws';
import { App, us_listen_socket_close } from '../src/index';
import type { us_listen_socket, WebSocket as UwsWebSocket } from '../src/index';


const largeBuffer = new ArrayBuffer(5 * 1024 * 1024);

const maxBackpressure = 50 * 1024 * 1024;

const maxPayloadSize = 5 * 1024 * 1024;

const port = 9001;


let closedClientConnections = 0;

let listenSocket: us_listen_socket;

let openedClientConnections = 0;


function accountForConnection(): void {
    if (++openedClientConnections < 1000) {
        establishNewConnection();
    }
    else {
        us_listen_socket_close(listenSocket);
    }
}

function establishNewConnection(): void {
    let ws = new WebSocket('ws://localhost:' + port);

    ws.on('open', () => {
        (ws as any)._opened = true;
        accountForConnection();

        printStatistics();
        performRandomClientAction(ws);
    });

    ws.on('message', () => {
        performRandomClientAction(ws);
    });

    ws.on('close', () => {
        if (!(ws as any)._opened) {
            accountForConnection();
        }

        closedClientConnections++;
        printStatistics();
    });

    ws.on('error', () => {});
}

function getRandomInt(max: number): number {
    return Math.floor(Math.random() * Math.floor(max));
}

function performRandomClientAction(ws: WebSocket): void {
    let action = getRandomInt(3);

    if (getRandomInt(100) < 80) {
        action = 1;
    }

    switch (action) {
        case 0: {
            ws.close();
            break;
        }
        case 1: {
            try {
                ws.send('a test message');
            }
            catch (e) {}

            break;
        }
        case 2: {
            ws.terminate();
            break;
        }
    }
}

function performRandomServerAction(ws: UwsWebSocket<unknown>, uniform: boolean): void {
    let action = getRandomInt(3);

    if (!uniform) {
        if (getRandomInt(100) < 80) {
            action = 1;
        }
    }

    switch (action) {
        case 0: {
            ws.end();
            break;
        }
        case 1: {
            if (ws.getBufferedAmount() > maxBackpressure) {
                performRandomServerAction(ws, false);
            }
            else {
                ws.send(largeBuffer.slice(0, getRandomInt(maxPayloadSize + 1)));
            }

            break;
        }
        case 2: {
            ws.close();
            break;
        }
    }
}

function printStatistics(): void {
    console.log('Opened clients ' + openedClientConnections);
    console.log('Closed clients ' + closedClientConnections + '\n');
}


App({
    cert_file_name: '.tmp/cert.pem',
    key_file_name: '.tmp/key.pem',
    passphrase: '1234'
}).ws('/*', {
    compression: 0,
    idleTimeout: 100,
    maxPayloadLength: maxPayloadSize / 2,
    open: (ws) => {
        performRandomServerAction(ws, false);
    },
    message: (ws) => {
        performRandomServerAction(ws, false);
    },
    drain: (ws) => {
        if (getRandomInt(100) < 5) {
            performRandomServerAction(ws, true);
        }
    },
    close: (ws) => {
        try {
            performRandomServerAction(ws, false);
        }
        catch (e) {
            return;
        }

        console.error('ERROR: Did not throw in close!');
        process.exit(-1);
    }
}).any('/*', (res) => {
    res.end('Nothing to see here!');
}).listen(port, (token) => {
    if (token) {
        console.log('Hammering on port ' + port);

        establishNewConnection();

        listenSocket = token;
    }
});
