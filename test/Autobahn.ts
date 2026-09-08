import { App, DEDICATED_COMPRESSOR, DISABLED, SHARED_COMPRESSOR, SSLApp, us_listen_socket_close } from '../src/index';
import type { CompressOptions, TemplatedApp, us_listen_socket } from '../src/index';


type Settings = {
    compression: CompressOptions;
    port: number;
    pubsub: boolean;
    ssl: boolean;
};


const apps: { app: TemplatedApp; listenSocket: us_listen_socket }[] = [];


let closing = false;


function listenWithSettings(settings: Settings): void {
    let sslOptions = {
            cert_file_name: '.tmp/cert.pem',
            key_file_name: '.tmp/key.pem',
            passphrase: '1234'
        },
        app = settings.ssl ? SSLApp(sslOptions) : App(sslOptions);

    app.ws('/*', {
        compression: settings.compression,
        idleTimeout: 60,
        maxBackpressure: 16 * 1024 * 1204,
        maxPayloadLength: 16 * 1024 * 1024,
        open: (ws) => {
            if (settings.pubsub) {
                ws.subscribe('broadcast');
            }
        },
        message: (ws, message, isBinary) => {
            if (settings.pubsub) {
                ws.publish('broadcast', message, isBinary);
            }
            else {
                ws.send(message, isBinary, true);
            }
        }
    }).any('/exit', (res) => {
        if (!closing) {
            apps.forEach((a) => {
                us_listen_socket_close(a.listenSocket);
            });
            closing = true;
        }

        res.close();
    }).listen(settings.port, (listenSocket) => {
        if (listenSocket) {
            apps.push({ app, listenSocket });
            console.log('Up and running: ' + JSON.stringify(settings));
        }
        else {
            console.log('Failed to listen, closing everything now');
            process.exit(0);
        }
    });
}


listenWithSettings({ compression: DISABLED, port: 9001, pubsub: false, ssl: false });
listenWithSettings({ compression: SHARED_COMPRESSOR, port: 9002, pubsub: false, ssl: true });
listenWithSettings({ compression: DEDICATED_COMPRESSOR, port: 9003, pubsub: false, ssl: false });
listenWithSettings({ compression: DISABLED, port: 9004, pubsub: true, ssl: false });
listenWithSettings({ compression: DISABLED, port: 9005, pubsub: true, ssl: true });
