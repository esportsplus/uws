import { isMainThread, parentPort, Worker, workerData } from 'node:worker_threads';
import { options, start, table } from '../harness';
import type { Options } from '../harness';

import WebSocket from 'ws';


type Result = {
    connections: number;
    'msg/s': number;
    server: string;
    size: number;
};

type WorkerData = {
    connections: number;
    duration: number;
    pipelining: number;
    port: number;
    size: number;
};


function client(data: WorkerData): void {
    let count = 0,
        payload = Buffer.alloc(data.size, 1),
        sockets: WebSocket[] = [];

    for (let i = 0; i < data.connections; i++) {
        let ws = new WebSocket(`ws://127.0.0.1:${data.port}/`);

        ws.on('error', () => {});
        ws.on('message', () => {
            count++;
            ws.send(payload);
        });
        ws.on('open', () => {
            for (let j = 0; j < data.pipelining; j++) {
                ws.send(payload);
            }
        });
        sockets.push(ws);
    }

    setTimeout(() => {
        parentPort?.postMessage(count);

        for (let i = 0, n = sockets.length; i < n; i++) {
            sockets[i].terminate();
        }
    }, data.duration * 1000);
}

async function run(server: string, config: Options): Promise<Result> {
    let started = await start(server),
        perWorker = Math.max(1, Math.floor(config.connections / config.workers)),
        counts = await Promise.all(Array.from({ length: config.workers }, () => spawn({
            connections: perWorker,
            duration: config.duration,
            pipelining: config.pipelining,
            port: started.port,
            size: config.size
        }))),
        total = counts.reduce((sum, count) => sum + count, 0);

    started.stop();

    return {
        connections: perWorker * config.workers,
        'msg/s': Math.round(total / config.duration),
        server,
        size: config.size
    };
}

function spawn(data: WorkerData): Promise<number> {
    return new Promise((resolve, reject) => {
        let worker = new Worker(import.meta.filename, { workerData: data });

        worker.once('error', (error) => {
            void worker.terminate();
            reject(error);
        });
        worker.once('exit', () => {
            void worker.terminate();
        });
        worker.once('message', (count: number) => {
            resolve(count);
        });
    });
}


if (isMainThread) {
    let config = options(process.argv.slice(2)),
        results: Result[] = [];

    for (let i = 0, n = config.servers.length; i < n; i++) {
        if (config.servers[i] === 'uws') {
            results.push(await run(config.servers[i], config));
        }
    }

    if (config.json) {
        console.log(JSON.stringify({ config, results }));
    }
    else {
        console.log(`WebSocket echo: ${config.workers} workers, pipelining ${config.pipelining}, ${config.duration}s\n`);
        console.log(table(results));
    }
}
else {
    client(workerData as WorkerData);
}
