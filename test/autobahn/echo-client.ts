import { Client, SHARED_COMPRESSOR, type TemplatedClient } from '../../src/index';


const AGENT = 'uws-client';
const BASE = 'ws://127.0.0.1:9001';


function getCaseCount(client: TemplatedClient): Promise<number> {
    return new Promise((resolve, reject) => {
        client.connect(`${BASE}/getCaseCount`, {
            compression: SHARED_COMPRESSOR,
            idleTimeout: 0,
            maxBackpressure: 256 * 1024 * 1024,
            maxPayloadLength: 100 * 1024 * 1024,
            failed: reject,
            message: (_ws, message) => {
                resolve(Number(Buffer.from(new Uint8Array(message)).toString()));
            }
        });
    });
}


function runCase(client: TemplatedClient, index: number): Promise<void> {
    return new Promise((resolve) => {
        client.connect(`${BASE}/runCase?case=${index}&agent=${AGENT}`, {
            compression: SHARED_COMPRESSOR,
            idleTimeout: 0,
            maxBackpressure: 256 * 1024 * 1024,
            maxPayloadLength: 100 * 1024 * 1024,
            close: () => {
                resolve();
            },
            failed: (error) => {
                /* Do not abort the run: continue so updateReports still writes the report and the
                 * failed case is recorded there. */
                console.error(`autobahn-client: case ${index} failed`, error);
                resolve();
            },
            message: (ws, message, isBinary) => {
                ws.send(message, isBinary);
            }
        });
    });
}


function updateReports(client: TemplatedClient): Promise<void> {
    return new Promise((resolve, reject) => {
        client.connect(`${BASE}/updateReports?agent=${AGENT}`, {
            compression: SHARED_COMPRESSOR,
            idleTimeout: 0,
            maxBackpressure: 256 * 1024 * 1024,
            maxPayloadLength: 100 * 1024 * 1024,
            close: () => {
                resolve();
            },
            failed: reject
        });
    });
}


async function main(): Promise<void> {
    const client = Client();
    const count = await getCaseCount(client);

    console.log(`autobahn-client: ${count} cases`);

    for (let index = 1; index <= count; index++) {
        await runCase(client, index);
    }

    await updateReports(client);
    client.close();
    console.log('autobahn-client: done');
}


try {
    await main();
}
catch (error) {
    console.error('autobahn-client: failed', error);
    process.exit(1);
}
