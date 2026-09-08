import { connect as netConnect, createConnection } from 'node:net';
import { expect } from 'vitest';
import { createServer, request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { us_listen_socket_close, us_socket_local_port } from '../src/index';
import type { IncomingHttpHeaders } from 'node:http';
import type { RequestOptions } from 'node:https';
import type { ConnectBehavior, ConnectError, TemplatedApp, TemplatedClient, us_listen_socket, WebSocket as UwsWebSocket } from '../src/index';
import type { ClientOptions } from 'ws';

import WebSocket from 'ws';


type Closed = {
    code: number;
    reason: string;
};

type Dialed<UserData = unknown> = {
    /** Resolves when the outbound socket closes (frame-phase close event). */
    closed: Promise<Closed>;
    /** Resolves at most once when the connection fails before open. */
    failed: Promise<ConnectError>;
    /** Messages received on the outbound socket, in arrival order. */
    messages: Message[];
    /** Resolves with the next message (queued or awaited), mirroring message(). */
    next: () => Promise<Message>;
    /** Resolves with the client WebSocket when the outbound handshake completes. */
    opened: Promise<UwsWebSocket<UserData>>;
};

type ConnectOptions = ClientOptions & {
    protocols?: string[];
};

type ConnectProxy = {
    close: () => void;
    seen: { authorization?: string; url?: string };
    url: string;
};

type Handshake = {
    headers: IncomingHttpHeaders;
    ws: WebSocket;
};

type Inbox = {
    queue: Message[];
    waiters: ((message: Message) => void)[];
};

type ListenOptions = {
    host?: string;
    secure?: boolean;
};

type Message = {
    data: Buffer;
    isBinary: boolean;
};

type RawOptions = {
    delay?: number;
    idle?: number;
    timeout?: number;
};

type RawResult = {
    data: Buffer;
    ended: boolean;
};

type RequestInit = {
    body?: Buffer | string;
    headers?: Record<string, string>;
    method?: string;
    servername?: string;
    socketPath?: string;
};

type Response = {
    body: Buffer;
    headers: IncomingHttpHeaders;
    status: number;
};

type Server = {
    app: TemplatedApp;
    close: () => void;
    port: number;
    socket: us_listen_socket;
    url: string;
};


const LOOPBACK = ['127.0.0.1', '0000:0000:0000:0000:0000:ffff:7f00:0001', '0000:0000:0000:0000:0000:0000:0000:0001'];

const inboxes = new WeakMap<WebSocket, Inbox>();


async function boundedMap<T, R>(items: readonly T[], run: (item: T, index: number) => Promise<R>): Promise<R[]> {
    let results: R[] = new Array(items.length),
        next = 0,
        lane = async (): Promise<void> => {
            while (next < items.length) {
                let index = next++;

                results[index] = await run(items[index], index);
            }
        };

    await Promise.all([lane(), lane(), lane(), lane(), lane(), lane(), lane(), lane()]);

    return results;
}

function closed(ws: WebSocket): Promise<Closed> {
    return new Promise((resolve) => {
        ws.once('close', (code, reason) => {
            resolve({ code, reason: reason.toString() });
        });
    });
}

async function connect(url: string, options: ConnectOptions = {}): Promise<WebSocket> {
    return (await handshake(url, options)).ws;
}

function dial<UserData = unknown>(client: TemplatedClient, url: string, behavior: Partial<ConnectBehavior<UserData>> = {}): Dialed<UserData> {
    let messages: Message[] = [],
        waiters: ((message: Message) => void)[] = [],
        resolveClosed!: (closed: Closed) => void,
        resolveFailed!: (error: ConnectError) => void,
        resolveOpened!: (ws: UwsWebSocket<UserData>) => void,
        closedPromise = new Promise<Closed>((resolve) => { resolveClosed = resolve; }),
        failedPromise = new Promise<ConnectError>((resolve) => { resolveFailed = resolve; }),
        openedPromise = new Promise<UwsWebSocket<UserData>>((resolve) => { resolveOpened = resolve; });

    client.connect<UserData>(url, {
        ...behavior,
        close: (ws, code, reason) => {
            resolveClosed({ code, reason: text(reason) });
            behavior.close?.(ws, code, reason);
        },
        failed: (error) => {
            resolveFailed(error);
            behavior.failed?.(error);
        },
        message: (ws, data, isBinary) => {
            let received: Message = { data: Buffer.from(new Uint8Array(data as ArrayBuffer)), isBinary },
                waiter = waiters.shift();

            if (waiter) {
                waiter(received);
            }
            else {
                messages.push(received);
            }

            void behavior.message?.(ws, data, isBinary);
        },
        open: (ws) => {
            resolveOpened(ws);
            void behavior.open?.(ws);
        }
    });

    return {
        closed: closedPromise,
        failed: failedPromise,
        messages,
        next: () => {
            let queued = messages.shift();

            if (queued) {
                return Promise.resolve(queued);
            }

            return new Promise((resolve) => {
                waiters.push(resolve);
            });
        },
        opened: openedPromise
    };
}

function handshake(url: string, options: ConnectOptions = {}): Promise<Handshake> {
    return new Promise((resolve, reject) => {
        let { protocols, ...client } = options,
            headers: IncomingHttpHeaders = {},
            ws = new WebSocket(url, protocols, client);

        inbox(ws);
        ws.once('error', reject);
        ws.once('upgrade', (response) => {
            headers = response.headers;
        });
        ws.once('open', () => {
            ws.off('error', reject);
            ws.on('error', () => {});
            resolve({ headers, ws });
        });
    });
}

function inbox(ws: WebSocket): Inbox {
    let box = inboxes.get(ws);

    if (box) {
        return box;
    }

    let created: Inbox = { queue: [], waiters: [] };

    inboxes.set(ws, created);
    ws.on('message', (data, isBinary) => {
        let received = { data: data as Buffer, isBinary },
            waiter = created.waiters.shift();

        if (waiter) {
            waiter(received);
        }
        else {
            created.queue.push(received);
        }
    });

    return created;
}

function listen(app: TemplatedApp, options: ListenOptions = {}): Server {
    let holder: { socket: us_listen_socket | false } = { socket: false },
        onListen = (token: us_listen_socket | false): void => {
            holder.socket = token;
        };

    if (options.host === undefined) {
        app.listen(0, onListen);
    }
    else {
        app.listen(options.host, 0, onListen);
    }

    expect(holder.socket, 'harness: listen failed').toBeTruthy();

    let socket = holder.socket as us_listen_socket,
        port = us_socket_local_port(socket);

    return {
        app,
        close: () => {
            us_listen_socket_close(socket);
        },
        port,
        socket,
        url: `${options.secure ? 'https' : 'http'}://127.0.0.1:${port}`
    };
}

function loopback(text: string): boolean {
    return LOOPBACK.includes(text);
}

function message(ws: WebSocket): Promise<Message> {
    let box = inbox(ws),
        queued = box.queue.shift();

    if (queued) {
        return Promise.resolve(queued);
    }

    return new Promise((resolve) => {
        box.waiters.push(resolve);
    });
}

function raw(port: number, data: (Buffer | string)[] | Buffer | string, options: RawOptions = {}): Promise<RawResult> {
    let delay = options.delay ?? 50,
        idle = options.idle ?? 250,
        timeout = options.timeout ?? 3000,
        writes = (Array.isArray(data) ? data : [data]).map((chunk) => (typeof chunk === 'string' ? Buffer.from(chunk) : chunk));

    return new Promise((resolve, reject) => {
        let chunks: Buffer[] = [],
            done = false,
            ended = false,
            idleTimer: NodeJS.Timeout | undefined,
            socket = createConnection({ host: '127.0.0.1', port }),
            finish = (): void => {
                if (done) {
                    return;
                }

                done = true;
                clearTimeout(idleTimer);
                clearTimeout(hardTimer);
                socket.destroy();
                resolve({ data: Buffer.concat(chunks), ended });
            },
            hardTimer = setTimeout(finish, timeout),
            write = (index: number): void => {
                if (index >= writes.length || done) {
                    return;
                }

                socket.write(writes[index]);
                setTimeout(() => write(index + 1), delay);
            };

        socket.on('connect', () => {
            write(0);
        });
        socket.on('data', (chunk) => {
            chunks.push(typeof chunk === 'string' ? Buffer.from(chunk) : chunk);
            clearTimeout(idleTimer);
            idleTimer = setTimeout(finish, idle);
        });
        socket.on('end', () => {
            ended = true;
            finish();
        });
        socket.on('close', () => {
            ended = true;
            finish();
        });
        socket.on('error', (error) => {
            if (chunks.length) {
                ended = true;
                finish();
                return;
            }

            done = true;
            clearTimeout(idleTimer);
            clearTimeout(hardTimer);
            reject(error);
        });
    });
}

function request(url: string, options: RequestInit = {}): Promise<Response> {
    let target = new URL(url),
        send = target.protocol === 'https:' ? httpsRequest : httpRequest,
        requestOptions: RequestOptions = {
            agent: false,
            headers: options.headers,
            method: options.method ?? 'GET',
            rejectUnauthorized: false,
            servername: options.servername,
            socketPath: options.socketPath
        };

    return new Promise((resolve, reject) => {
        let req = send(target, requestOptions, (res) => {
            let chunks: Buffer[] = [];

            res.on('data', (chunk) => {
                chunks.push(chunk);
            });
            res.on('end', () => {
                resolve({ body: Buffer.concat(chunks), headers: res.headers, status: res.statusCode ?? 0 });
            });
        });

        req.on('close', () => {
            req.destroy();
        });
        req.on('error', (error) => {
            req.destroy();
            reject(error);
        });
        req.end(options.body);
    });
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => {
        setTimeout(resolve, ms);
    });
}

function startConnectProxy(options: { auth?: string; respond?: string } = {}): Promise<ConnectProxy> {
    let seen: { authorization?: string; url?: string } = {},
        proxy = createServer();

    proxy.on('connect', (req, clientSocket, head) => {
        seen.url = req.url;
        seen.authorization = req.headers['proxy-authorization'];

        expect(head.length).toBe(0);

        if (options.respond) {
            clientSocket.write(`HTTP/1.1 ${options.respond}\r\n\r\n`);
            clientSocket.destroy();
            return;
        }

        if (options.auth && seen.authorization !== `Basic ${Buffer.from(options.auth).toString('base64')}`) {
            clientSocket.write('HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="t"\r\n\r\n');
            clientSocket.destroy();
            return;
        }

        let target = new URL(`http://${req.url}`),
            upstream = netConnect({ host: target.hostname.replace(/^\[|\]$/g, ''), port: Number(target.port) });

        upstream.on('connect', () => {
            clientSocket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
            clientSocket.pipe(upstream).pipe(clientSocket);
        });
        upstream.on('error', () => {
            upstream.destroy();
            clientSocket.destroy();
        });
        clientSocket.on('close', () => {
            upstream.destroy();
        });
    });

    return new Promise((resolve) => {
        proxy.listen(0, '127.0.0.1', () => {
            let port = (proxy.address() as import('node:net').AddressInfo).port;

            resolve({
                close: () => { proxy.close(); },
                seen,
                url: `http://127.0.0.1:${port}`
            });
        });
    });
}

function text(data: ArrayBuffer | Buffer): string {
    return Buffer.from(data as ArrayBuffer).toString();
}


export { boundedMap, closed, connect, dial, handshake, listen, loopback, message, raw, request, sleep, startConnectProxy, text };
export type { Closed, ConnectOptions, ConnectProxy, Dialed, Handshake, Message, RawResult, Response, Server };
