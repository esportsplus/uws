import { App, SHARED_COMPRESSOR } from '../../src/index';


const PORT = Number(process.env.PORT ?? 9001);


App()
    .ws('/*', {
        compression: SHARED_COMPRESSOR,
        idleTimeout: 0,
        maxBackpressure: 256 * 1024 * 1024,
        maxPayloadLength: 100 * 1024 * 1024,
        message: (ws, message, isBinary) => {
            ws.send(message, isBinary);
        }
    })
    .listen(PORT, (token) => {
        if (!token) {
            console.error(`autobahn-echo: failed to listen on ${PORT}`);
            process.exit(1);
        }

        console.log(`autobahn-echo: listening on ${PORT}`);
    });
