import { App, us_socket_local_port } from '../../src/index';


App()
    .get('/*', (res) => {
        res.end('Hello World!');
    })
    .ws('/*', {
        compression: 0,
        idleTimeout: 0,
        maxBackpressure: 16 * 1024 * 1024,
        maxPayloadLength: 1024 * 1024,
        message: (ws, message, isBinary) => {
            ws.send(message, isBinary);
        }
    })
    .listen(Number(process.argv[2] ?? 0), (token) => {
        if (!token) {
            process.exit(1);
        }

        process.send?.({ port: us_socket_local_port(token) });
    });
