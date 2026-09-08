import { parentPort, workerData } from 'node:worker_threads';
import { App, Client, SSLApp, us_listen_socket_close, us_socket_local_port } from '../../src/index';


type Scenario = 'resolving' | 'connecting' | 'handshaking' | 'open' | 'cooperative'
    | 'closed' | 'server-listening' | 'server-http-pending' | 'server-ws-open' | 'server-tls-handshaking' | 'server-cooperative';

type WorkerData = {
    scenario: Scenario;
    port?: number;
};


let { scenario, port } = workerData as WorkerData;

if (scenario.startsWith('server-')) {
    let app = scenario === 'server-tls-handshaking'
        ? SSLApp({ cert_file_name: '.tmp/cert.pem', key_file_name: '.tmp/key.pem', passphrase: '1234' })
        : App();

    if (scenario === 'server-http-pending') {
        app.any('/*', (res) => {
            res.onAborted(() => {});
            parentPort?.postMessage('ready');
        });
    }
    else if (scenario === 'server-ws-open') {
        app.ws('/*', {
            open: () => parentPort?.postMessage('ready')
        });
    }

    app.listen(0, (listenSocket) => {
        if (!listenSocket) {
            throw new Error('listen failed');
        }
        parentPort?.postMessage(`listening:${us_socket_local_port(listenSocket)}`);

        if (scenario === 'server-cooperative') {
            us_listen_socket_close(listenSocket);
            app.close();
        }
    });

    /* A cooperative worker has no live handles after closing its listen socket and app. */
    if (scenario !== 'server-cooperative') {
        setInterval(() => {}, 1000);
    }
}
else {
    let client = Client(),
        url: string;

    if (scenario === 'closed') {
        client.close();
        parentPort?.postMessage('closed');
    }

    else {
        switch (scenario) {
        case 'resolving':
            url = 'ws://a-host-that-does-not-resolve.invalid.test/';
            break;
        case 'connecting':
            url = 'ws://192.0.2.1/';
            break;
        case 'handshaking':
        case 'open':
        case 'cooperative':
            if (!port) {
                throw new Error(`${scenario} requires a port`);
            }
            url = `ws://127.0.0.1:${port}/`;
            break;
        default:
            throw new Error(`unexpected client scenario: ${scenario}`);
        }

        client.connect(url, {
            connectTimeout: scenario === 'connecting' ? 5000 : undefined,
            failed: () => {},
            open: () => {
                parentPort?.postMessage('open');
                if (scenario === 'cooperative') {
                    process.exit(0);
                }
            }
        });

        if (scenario === 'resolving' || scenario === 'connecting' || scenario === 'handshaking') {
            parentPort?.postMessage('connecting');
        }
    }

    /* The parent controls teardown except for the cooperative process.exit case. */
    setInterval(() => {}, 1000);
}
