import { readFileSync } from 'node:fs';


type Metric = {
    max: number;
    median: number;
    min: number;
    samples: number[];
};

type Report = {
    http: { reqPerSec: Metric; throughputMBps: Metric };
    ws: { msgPerSec: Metric };
    wsClient: { msgPerSec: Metric };
};

type Row = {
    'A (median)': number;
    'B (median)': number;
    'delta %': string;
    metric: string;
    verdict: string;
};


/* Deltas within this band are treated as noise, not signal. */
const NOISE = 3;

const METRICS: { key: string; path: (r: Report) => Metric }[] = [
    { key: 'http req/s', path: (r) => r.http.reqPerSec },
    { key: 'http throughput MB/s', path: (r) => r.http.throughputMBps },
    { key: 'ws msg/s', path: (r) => r.ws.msgPerSec },
    { key: 'ws-client msg/s', path: (r) => r.wsClient.msgPerSec }
];


function row(key: string, a: Metric, b: Metric): Row {
    let delta = a.median === 0 ? 0 : ((b.median - a.median) / a.median) * 100;

    return {
        'A (median)': a.median,
        'B (median)': b.median,
        'delta %': `${delta >= 0 ? '+' : ''}${delta.toFixed(1)}`,
        metric: key,
        verdict: Math.abs(delta) < NOISE ? 'noise' : delta > 0 ? 'faster' : 'SLOWER'
    };
}

function table(rows: Row[]): string {
    let keys = Object.keys(rows[0]),
        lines = [`| ${keys.join(' | ')} |`, `|${keys.map(() => '---').join('|')}|`];

    for (let i = 0, n = rows.length; i < n; i++) {
        lines.push(`| ${keys.map((key) => rows[i][key as keyof Row]).join(' | ')} |`);
    }

    return lines.join('\n');
}


let [pathA, pathB] = process.argv.slice(2);

if (!pathA || !pathB) {
    process.stderr.write('Usage: compare.ts <A.json> <B.json>\n');
    process.exit(1);
}

let a = JSON.parse(readFileSync(pathA, 'utf8')) as Report,
    b = JSON.parse(readFileSync(pathB, 'utf8')) as Report,
    rows = METRICS.map(({ key, path }) => row(key, path(a), path(b)));

process.stdout.write(`A = ${pathA}\nB = ${pathB}\n\n${table(rows)}\n`);
