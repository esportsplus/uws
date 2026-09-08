import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';


type Metric = {
    max: number;
    median: number;
    min: number;
    samples: number[];
};

type Report = {
    config: Record<string, unknown>;
    http: { reqPerSec: Metric; throughputMBps: Metric };
    meta: { reps: number; warmup: number };
    ws: { msgPerSec: Metric };
    wsClient: { msgPerSec: Metric };
};


const DIR = dirname(fileURLToPath(import.meta.url));

const SCRIPTS: Record<string, string> = {
    http: 'http/http.bench.ts',
    ws: 'ws/ws.bench.ts',
    'ws-client': 'ws/ws-client.bench.ts'
};


function metric(samples: number[]): Metric {
    let sorted = [...samples].sort((a, b) => a - b);

    return {
        max: sorted[sorted.length - 1],
        median: sorted[Math.floor((sorted.length - 1) / 2)],
        min: sorted[0],
        samples
    };
}

function once(script: string, passthrough: string[]): Record<string, number> {
    let output = execFileSync('node', ['--import', 'tsx', join(DIR, SCRIPTS[script]), '--json', '--servers=uws', ...passthrough], { encoding: 'utf8' }),
        parsed = JSON.parse(output.trim().split('\n').pop() as string);

    return parsed.results[0];
}

function repeat(script: string, reps: number, warmup: number, passthrough: string[]): Record<string, number>[] {
    let results: Record<string, number>[] = [];

    for (let i = 0, n = reps + warmup; i < n; i++) {
        let result = once(script, passthrough);

        process.stderr.write(`  ${script} rep ${i + 1}/${n}${i < warmup ? ' (warmup, discarded)' : ''}: ${JSON.stringify(result)}\n`);

        if (i >= warmup) {
            results.push(result);
        }
    }

    return results;
}

function series(results: Record<string, number>[], key: string): number[] {
    return results.map((result) => result[key]);
}


let argv = process.argv.slice(2),
    reps = 5,
    warmup = 1,
    passthrough: string[] = [];

for (let i = 0, n = argv.length; i < n; i++) {
    let [key, value] = argv[i].replace(/^--/, '').split('=');

    if (key === 'reps') {
        reps = Number(value);
    }
    else if (key === 'warmup') {
        warmup = Number(value);
    }
    else if (key !== 'json' && key !== 'servers') {
        passthrough.push(argv[i]);
    }
}

process.stderr.write(`bench: ${reps} reps (+${warmup} warmup) per suite\n`);

let http = repeat('http', reps, warmup, passthrough),
    ws = repeat('ws', reps, warmup, passthrough),
    wsClient = repeat('ws-client', reps, warmup, passthrough);

let report: Report = {
    config: { argv: passthrough },
    http: { reqPerSec: metric(series(http, 'req/s')), throughputMBps: metric(series(http, 'throughput (MB/s)')) },
    meta: { reps, warmup },
    ws: { msgPerSec: metric(series(ws, 'msg/s')) },
    wsClient: { msgPerSec: metric(series(wsClient, 'msg/s')) }
};

process.stdout.write(JSON.stringify(report) + '\n');
