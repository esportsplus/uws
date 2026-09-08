import { fork } from 'node:child_process';
import { availableParallelism } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';


type Options = {
    connections: number;
    duration: number;
    json: boolean;
    pipelining: number;
    servers: string[];
    size: number;
    workers: number;
};

type Started = {
    port: number;
    stop: () => void;
};


const DIR = dirname(fileURLToPath(import.meta.url));


function options(argv: string[]): Options {
    let parsed: Options = {
        connections: 256,
        duration: 10,
        json: false,
        pipelining: 8,
        servers: ['uws', 'node'],
        size: 64,
        workers: Math.max(1, Math.min(16, availableParallelism() - 2))
    };

    for (let i = 0, n = argv.length; i < n; i++) {
        let [key, value] = argv[i].replace(/^--/, '').split('=');

        switch (key) {
            case 'connections':
            case 'duration':
            case 'pipelining':
            case 'size':
            case 'workers': {
                parsed[key] = Number(value);
                break;
            }
            case 'json': {
                parsed.json = value !== 'false';
                break;
            }
            case 'servers': {
                parsed.servers = value.split(',');
                break;
            }
        }
    }

    return parsed;
}

function start(server: string): Promise<Started> {
    return new Promise((resolve, reject) => {
        let child = fork(join(DIR, 'servers', `${server}.ts`), ['0'], { stdio: 'inherit' });

        try {
            child.once('error', (error) => {
                child[Symbol.dispose]();
                reject(error);
            });
            child.once('exit', (code) => {
                child[Symbol.dispose]();
                reject(new Error(`bench: ${server} exited with ${code}`));
            });
            child.once('message', (message) => {
                child.removeAllListeners('exit');
                resolve({
                    port: (message as { port: number }).port,
                    stop: () => {
                        child.kill();
                    }
                });
            });
        }
        catch (error) {
            child[Symbol.dispose]();
            reject(error);
        }
    });
}

function table(rows: Record<string, number | string>[]): string {
    let keys = Object.keys(rows[0]),
        lines = [`| ${keys.join(' | ')} |`, `|${keys.map(() => '---').join('|')}|`];

    for (let i = 0, n = rows.length; i < n; i++) {
        lines.push(`| ${keys.map((key) => rows[i][key]).join(' | ')} |`);
    }

    return lines.join('\n');
}


export { options, start, table };
export type { Options, Started };
