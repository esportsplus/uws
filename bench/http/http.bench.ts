import { options, start, table } from '../harness';
import type { Options } from '../harness';

import autocannon from 'autocannon';


type Result = {
    'latency p50 (ms)': number;
    'req/s': number;
    server: string;
    'throughput (MB/s)': number;
};


async function run(server: string, config: Options): Promise<Result> {
    let started = await start(server),
        result = await autocannon({
            connections: config.connections,
            duration: config.duration,
            pipelining: config.pipelining,
            url: `http://127.0.0.1:${started.port}/`,
            workers: config.workers
        });

    started.stop();

    return {
        'latency p50 (ms)': result.latency.p50,
        'req/s': Math.round(result.requests.average),
        server,
        'throughput (MB/s)': Math.round(result.throughput.average / 1024 / 1024)
    };
}


const config = options(process.argv.slice(2));

const results: Result[] = [];

for (let i = 0, n = config.servers.length; i < n; i++) {
    results.push(await run(config.servers[i], config));
}

if (config.json) {
    console.log(JSON.stringify({ config, results }));
}
else {
    console.log(`HTTP: ${config.connections} connections, pipelining ${config.pipelining}, ${config.workers} workers, ${config.duration}s\n`);
    console.log(table(results));
}
