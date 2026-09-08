import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { App } from '../../src/index';
import { boundedMap, closed, connect, listen, message, sleep, text } from '../harness';
import type { TemplatedApp, WebSocket as UwsWebSocket } from '../../src/index';
import type { Server } from '../harness';

import WebSocket from 'ws';


type UserData = {
    name: string;
};


const BIG = 'b'.repeat(20000);

const FILL = 'f'.repeat(8 * 1024 * 1024);

const events: string[] = [];

const sockets = new Map<string, UwsWebSocket<UserData>>();


let app: TemplatedApp;

let server: Server;


function build(): TemplatedApp {
    let built = App();

    built.ws<UserData>('/pubsub', {
        close: (ws) => {
            sockets.delete(ws.getUserData().name);
        },
        message: (ws, data) => {
            let [command, topic, payload] = text(data).split(':');

            switch (command) {
                case 'apppub': {
                    reply(ws, app.publish(topic, `m:${payload}`));
                    break;
                }
                case 'appbig': {
                    reply(ws, app.publish(topic, `m:${BIG}`));
                    break;
                }
                case 'binary': {
                    reply(ws, app.publish(topic, new Uint8Array([9, 8, 7]), true, true));
                    break;
                }
                case 'count': {
                    reply(ws, app.numSubscribers(topic));
                    break;
                }
                case 'is': {
                    reply(ws, ws.isSubscribed(topic));
                    break;
                }
                case 'ordered': {
                    app.publish(topic, 'm:first');
                    sockets.get(payload)?.send('r:"second"');
                    reply(ws, true);
                    break;
                }
                case 'boundary': {
                    app.publish(topic, 'm:small-first');
                    app.publish(topic, `m:${BIG}`);
                    app.publish(topic, 'm:small-last');
                    reply(ws, true);
                    break;
                }
                case 'pub': {
                    reply(ws, ws.publish(topic, `m:${payload}`));
                    break;
                }
                case 'sub': {
                    reply(ws, ws.subscribe(topic));
                    break;
                }
                case 'topics': {
                    reply(ws, ws.getTopics());
                    break;
                }
                case 'unsub': {
                    reply(ws, ws.unsubscribe(topic));
                    break;
                }
            }
        },
        open: (ws) => {
            sockets.set(ws.getUserData().name, ws);
        },
        subscription: (ws, topic, newCount, oldCount) => {
            events.push(`${ws.getUserData().name}:${text(topic)}:${newCount}:${oldCount}`);
        },
        upgrade: (res, req, context) => {
            res.upgrade<UserData>(
                { name: req.getQuery('name') ?? 'anonymous' },
                req.getHeader('sec-websocket-key'),
                req.getHeader('sec-websocket-protocol'),
                req.getHeader('sec-websocket-extensions'),
                context
            );
        }
    });

    return built;
}

async function bounded<T>(promise: Promise<T>, label: string, ms = 3000): Promise<T> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
        return await Promise.race([promise, new Promise<never>((_resolve, reject) => {
            timer = setTimeout(() => reject(new Error(`Timed out: ${label}`)), ms);
        })]);
    }
    finally { clearTimeout(timer); }
}

async function command(ws: WebSocket, line: string): Promise<unknown> {
    let reply = message(ws);

    ws.send(line);

    return JSON.parse((await reply).data.toString().slice(2));
}

function join(name: string): Promise<WebSocket> {
    return connect(`${server.url}/pubsub?name=${name}`);
}

function reply(ws: UwsWebSocket<UserData>, value: unknown): void {
    ws.send(`r:${JSON.stringify(value)}`);
}

function buildDrainApp(dropped: (ws: UwsWebSocket<unknown>, subscribers: UwsWebSocket<unknown>[]) => void, payload = 'd'.repeat(1024), count = 200, events: string[] = []): TemplatedApp {
    let built = App(),
        subscribers: UwsWebSocket<unknown>[] = [];

    built.get('/flood', (res) => {
        for (let i = 0; i < count; i++) {
            built.publish('drain', payload);
        }
        res.end();
    });

    built.ws('/drain', {
        dropped: (ws) => {
            dropped(ws, subscribers);
        },
        // The readable sibling must hold the entire batch even with a small OS send buffer.
        maxBackpressure: 1024 * 1024,
        message: (ws, data) => {
            if (text(data) === 'subscribe') {
                ws.subscribe('drain');
                ws.send('subscribed');
            }
            else if (text(data) === 'fill') {
                // Winsock can accept one large write into its kernel buffer in full.
                for (let i = 0; i < 4 && ws.getBufferedAmount() <= 1024 * 1024; i++) {
                    ws.send(FILL);
                }
            }
        },
        open: (ws) => {
            subscribers.push(ws);
        },
        subscription: (_ws, topic, newCount, oldCount) => {
            events.push(`${text(topic)}:${newCount}:${oldCount}`);
        }
    });

    return built;
}

async function prime(url: string, subscribe = true): Promise<WebSocket> {
    let ws = await connect(`${url.replace('http', 'ws')}/drain`);

    if (subscribe) {
        ws.send('subscribe');
        await message(ws);
    }

    return ws;
}

async function backpressure(ws: WebSocket): Promise<void> {
    socketOf(ws).pause();
    ws.send('fill');
    await sleep(100);
}

function socketOf(ws: WebSocket): { pause: () => void; resume: () => void } {
    return (ws as unknown as { _socket: { pause: () => void; resume: () => void } })._socket;
}

async function receive(ws: WebSocket, count: number): Promise<string[]> {
    let received: string[] = [];

    for (let i = 0; i < count; i++) {
        received.push((await message(ws)).data.toString());
    }

    return received;
}


beforeAll(() => {
    app = build();
    server = listen(app);
});

afterAll(() => {
    server.close();
});


describe('subscriptions', () => {
    it('does not leak a subscriber when subscribe runs inside a close-time subscription event', async () => {
        let lateApp = App();

        lateApp.ws('/late', {
            message: (ws) => {
                ws.subscribe('late-a');
                ws.send('ok');
            },
            subscription: (ws, _topic, newCount, oldCount) => {
                if (newCount < oldCount) {
                    try {
                        ws.subscribe('late-b');
                    }
                    catch {
                        // Wrapper already invalidated on JS-initiated paths: expected.
                    }
                }
            }
        });

        let lateServer = listen(lateApp),
            ws: WebSocket | undefined;

        try {
            ws = await connect(`${lateServer.url.replace('http', 'ws')}/late`, { handshakeTimeout: 3000 });

            let done = closed(ws),
                reply = message(ws);

            ws.send('go');
            expect((await bounded(reply, 'late subscription acknowledged')).data.toString()).toBe('ok');
            expect(lateApp.numSubscribers('late-a')).toBe(1);
            ws.close();
            await bounded(done, 'late subscriber close');
            await sleep(50);
            expect(lateApp.numSubscribers('late-a')).toBe(0);
            expect(lateApp.numSubscribers('late-b')).toBe(0);
        }
        finally {
            ws?.terminate();
            lateServer.close();
        }
    }, 10000);

    it('reports subscribe and unsubscribe results', async () => {
        let ws = await join('a');

        expect(await command(ws, 'sub:t1')).toBe(true);
        expect(await command(ws, 'sub:t1')).toBe(true);
        expect(await command(ws, 'is:t1')).toBe(true);
        expect(await command(ws, 'unsub:t1')).toBe(true);
        expect(await command(ws, 'unsub:t1')).toBe(false);
        expect(await command(ws, 'is:t1')).toBe(false);
        ws.close();
    });

    it('lists topics and counts subscribers', async () => {
        let a = await join('a'),
            b = await join('b');

        await command(a, 'sub:t2');
        await command(a, 'sub:t3');
        await command(b, 'sub:t2');

        expect((await command(a, 'topics') as string[]).sort()).toEqual(['t2', 't3']);
        expect(await command(b, 'topics')).toEqual(['t2']);
        expect(await command(a, 'count:t2')).toBe(2);
        expect(await command(a, 'count:t3')).toBe(1);
        expect(await command(a, 'count:none')).toBe(0);
        a.close();
        b.close();
    });

    it('emits subscription events with new and old counts', async () => {
        let a = await join('ea'),
            b = await join('eb'),
            done = closed(a);

        events.length = 0;
        await command(a, 'sub:et');
        await command(b, 'sub:et');
        await command(b, 'unsub:et');
        a.close();
        await done;
        await sleep(50);

        expect(events).toEqual(['ea:et:1:0', 'eb:et:2:1', 'eb:et:1:2', 'ea:et:0:1']);
        b.close();
    });
});

describe('publishing', () => {
    it('publishes to other subscribers but not the sender', async () => {
        let a = await join('pa'),
            b = await join('pb'),
            c = await join('pc');

        await command(a, 'sub:pt');
        await command(b, 'sub:pt');

        let received = message(b);

        expect(await command(a, 'pub:pt:hello')).toBe(true);
        expect((await received).data.toString()).toBe('m:hello');
        expect(await command(c, 'pub:pt:nope')).toBe(false);

        let none = await Promise.race([message(a).then(() => 'received'), sleep(200).then(() => 'silent')]);

        expect(none).toBe('silent');
        a.close();
        b.close();
        c.close();
    });

    it('publishes from the app to every subscriber', async () => {
        let a = await join('aa'),
            b = await join('ab'),
            c = await join('ac');

        await command(a, 'sub:at');
        await command(b, 'sub:at');

        expect(await command(c, 'apppub:at:broadcast')).toBe(true);
        expect((await message(a)).data.toString()).toBe('m:broadcast');
        expect((await message(b)).data.toString()).toBe('m:broadcast');
        expect(await command(c, 'apppub:empty:x')).toBe(false);
        a.close();
        b.close();
        c.close();
    });

    it('delivers a publish to the sender before the reply to its own command', async () => {
        let a = await join('sa');

        await command(a, 'sub:st');
        a.send('apppub:st:self');

        expect((await message(a)).data.toString()).toBe('m:self');
        expect((await message(a)).data.toString()).toBe('r:true');
        a.close();
    });

    it('publishes big messages that bypass the cork buffer', async () => {
        let a = await join('ba'),
            b = await join('bb');

        await command(b, 'sub:bt');

        let received = message(b);

        expect(await command(a, 'appbig:bt')).toBe(true);
        expect((await received).data.toString()).toBe(`m:${BIG}`);
        a.close();
        b.close();
    });

    it('preserves order across the publishBig boundary', async () => {
        let sender = await join('boundary-sender'),
            subscriber = await join('boundary-subscriber');

        await command(subscriber, 'sub:boundary');
        expect(await command(sender, 'boundary:boundary')).toBe(true);
        expect(await receive(subscriber, 3)).toEqual(['m:small-first', `m:${BIG}`, 'm:small-last']);
        sender.close();
        subscriber.close();
    });

    it('publishes binary messages', async () => {
        let a = await join('ia'),
            b = await join('ib');

        await command(b, 'sub:it');

        let received = message(b);

        expect(await command(a, 'binary:it')).toBe(true);

        let data = await received;

        expect(data.isBinary).toBe(true);
        expect(Array.from(data.data)).toEqual([9, 8, 7]);
        a.close();
        b.close();
    });

    it('keeps published messages ordered before direct sends', async () => {
        let a = await join('oa'),
            b = await join('ob');

        await command(a, 'sub:ot');

        let first = message(a);

        await command(b, 'ordered:ot:oa');

        expect((await first).data.toString()).toBe('m:first');
        expect((await message(a)).data.toString()).toBe('r:"second"');
        a.close();
        b.close();
    });

    it('matches topics exactly without wildcards', async () => {
        let a = await join('wa'),
            b = await join('wb');

        await command(a, 'sub:w/#');

        expect(await command(b, 'apppub:w/x:nope')).toBe(false);
        expect(await command(b, 'apppub:w/#:yes')).toBe(true);
        expect((await message(a)).data.toString()).toBe('m:yes');
        a.close();
        b.close();
    });

    it('returns false when a websocket publishes to itself as the only subscriber', async () => {
        let ws = await join('only-subscriber');

        await command(ws, 'sub:alone');
        expect(await command(ws, 'pub:alone:ignored')).toBe(false);
        ws.close();
    });

    it('accepts binary, long and empty topic names', async () => {
        let subscriber = await join('topic-subscriber'),
            publisher = await join('topic-publisher'),
            nul = 'nul\0topic',
            long = 'l'.repeat(4096);

        expect(await command(subscriber, `sub:${nul}`)).toBe(true);
        expect(await command(subscriber, `sub:${long}`)).toBe(true);
        expect(await command(subscriber, 'sub:')).toBe(true);
        expect(await command(subscriber, 'count:')).toBe(1);
        expect(await command(publisher, `apppub:${nul}:nul`)).toBe(true);
        expect(await command(publisher, `apppub:${long}:long`)).toBe(true);
        expect(await command(publisher, 'apppub::empty')).toBe(true);
        expect(await receive(subscriber, 3)).toEqual(['m:nul', 'm:long', 'm:empty']);
        subscriber.close();
        publisher.close();
    });

    it('allows publish and subscription changes from live handlers and rejects ws access from close', async () => {
        let handlerApp = App(),
            closeAccessRejected = '',
            messageChangedSubscriptions = false,
            sawDrain = false,
            fill = 'f'.repeat(1024 * 1024);

        handlerApp.ws('/handlers', {
            close: (ws) => {
                // The ws object is invalidated before the close handler runs, so any
                // subscription change on it throws; app-level publish still works.
                try {
                    ws.subscribe('close-topic');
                }
                catch (error) {
                    closeAccessRejected = (error as Error).message;
                }
                handlerApp.publish('events', 'close');
            },
            drain: () => {
                sawDrain = true;
                handlerApp.publish('events', 'drain');
            },
            maxBackpressure: 32 * 1024 * 1024,
            message: (ws, data) => {
                switch (text(data)) {
                    case 'events':
                        ws.subscribe('events');
                        ws.send('ready');
                        break;
                    case 'trigger':
                        ws.subscribe('trigger');
                        ws.send('triggered');
                        break;
                    case 'fill':
                        for (let i = 0; i < 40; i++) {
                            ws.send(fill);
                        }
                        break;
                    case 'mutate':
                        messageChangedSubscriptions = ws.subscribe('pending-drain') && ws.unsubscribe('pending-drain');
                        handlerApp.publish('events', 'mutated');
                        break;
                }
            },
            subscription: (_ws, topic) => {
                if (text(topic) === 'trigger') {
                    handlerApp.publish('events', 'subscription');
                }
            }
        });

        let handlerServer = listen(handlerApp),
            observer = await connect(`${handlerServer.url.replace('http', 'ws')}/handlers`),
            subject = await connect(`${handlerServer.url.replace('http', 'ws')}/handlers`),
            subjectClosed = closed(subject);

        // Writable callbacks can publish multiple drain events, including before close.
        // Require delivery of every handler's publication without pinning their interleaving.
        let receivedEvents = new Set<string>();

        async function waitForEvent(event: string): Promise<void> {
            while (!receivedEvents.has(event)) {
                receivedEvents.add((await message(observer)).data.toString());
            }
        }

        observer.send('events');
        expect((await message(observer)).data.toString()).toBe('ready');
        subject.send('trigger');
        await message(subject);
        await waitForEvent('subscription');

        socketOf(subject).pause();
        subject.send('fill');
        subject.send('mutate');
        await waitForEvent('mutated');
        socketOf(subject).resume();
        expect(messageChangedSubscriptions).toBe(true);

        let drain = await Promise.race([waitForEvent('drain').then(() => 'drain'), sleep(3000).then(() => 'timeout')]);

        expect(sawDrain).toBe(true);
        expect(drain).toBe('drain');

        subject.close();
        await subjectClosed;
        await waitForEvent('close');
        expect(closeAccessRejected).toBe('Invalid access of closed uWS.WebSocket/SSLWebSocket.');

        let fresh = await connect(`${handlerServer.url.replace('http', 'ws')}/handlers`);

        fresh.send('events');
        expect((await message(fresh)).data.toString()).toBe('ready');
        expect(handlerApp.publish('events', 'final')).toBe(true);
        expect((await message(fresh)).data.toString()).toBe('final');
        await waitForEvent('final');
        expect(receivedEvents).toEqual(new Set(['subscription', 'mutated', 'drain', 'close', 'final']));
        fresh.close();
        observer.close();
        handlerServer.close();
    }, 10000);

    it('leaves unsubscribed subscriber objects safe through publish and close churn', async () => {
        let churnApp = App();

        churnApp.ws<{ index: number }>('/churn', {
            message: (ws, data) => {
                if (text(data) !== 'churn') {
                    return;
                }

                let offset = ws.getUserData().index;

                for (let i = 0; i < 100; i++) {
                    ws.subscribe(`topic-${(i * 37 + offset) % 100}`);
                }
                for (let i = 99; i >= 0; i--) {
                    ws.unsubscribe(`topic-${(i * 37 + offset) % 100}`);
                }
                churnApp.publish('topic-0', 'after-unsubscribe-all');
                ws.send('done');
            },
            upgrade: (res, req, context) => {
                res.upgrade(
                    { index: Number(req.getQuery('index')) },
                    req.getHeader('sec-websocket-key'),
                    req.getHeader('sec-websocket-protocol'),
                    req.getHeader('sec-websocket-extensions'),
                    context
                );
            }
        });

        let churnServer = listen(churnApp),
            clients = await boundedMap(Array.from({ length: 200 }, (_, index) => index), (index) => connect(`${churnServer.url.replace('http', 'ws')}/churn?index=${index}`));

        clients.forEach((ws) => ws.send('churn'));
        expect(await boundedMap(clients, (ws) => message(ws).then((received) => received.data.toString()))).toEqual(Array(200).fill('done'));

        let closingOrder = Array.from({ length: 200 }, (_, index) => index).sort((a, b) => ((a * 73) % 199) - ((b * 73) % 199)),
            closeEvents = closingOrder.map((index) => closed(clients[index]));

        closingOrder.forEach((index) => clients[index].close());
        await boundedMap(closeEvents, (event) => event);
        churnServer.close();
    }, 30000);

    it.skipIf(!process.env.UWS_SLOW_TESTS)('survives a SlowReceiver-style immediate publish loop when a subscriber closes mid-drain', async () => {
        let slowApp = App(),
            running = true,
            payload = 's'.repeat(64 * 1024),
            publish = (): void => {
                if (running) {
                    slowApp.publish('clock_feed', payload);
                    setImmediate(publish);
                }
            };

        slowApp.ws('/slow', {
            maxBackpressure: 32 * 1024 * 1024,
            open: (ws) => {
                ws.subscribe('clock_feed');
            }
        });

        let slowServer = listen(slowApp),
            slow = await connect(`${slowServer.url.replace('http', 'ws')}/slow`),
            slowClosed = closed(slow);

        socketOf(slow).pause();
        publish();
        await sleep(250);
        slow.terminate();
        await slowClosed;
        await sleep(1750);
        running = false;

        let fresh = await connect(`${slowServer.url.replace('http', 'ws')}/slow`);

        expect(slowApp.publish('clock_feed', 'final')).toBe(true);
        expect((await message(fresh)).data.toString()).toBe('final');
        fresh.close();
        slowServer.close();
    }, 10000);

    it('survives a dropped handler ending its socket during the 32-message overflow drain', async () => {
        let didDrop = false,
            drainApp = buildDrainApp((ws) => {
                didDrop = true;
                ws.end(1013);
            }, 'd'.repeat(15000), 500),
            drainServer = listen(drainApp),
            paused = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        paused.send('subscribe');
        await message(paused);
        socketOf(paused).pause();
        expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
        expect(didDrop).toBe(true);
        let fresh = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        fresh.send('subscribe');
        await message(fresh);
        drainApp.publish('drain', 'final');
        expect(await receive(fresh, 1)).toEqual(['final']);
        socketOf(paused).resume();
        paused.close();
        fresh.close();
        drainServer.close();
    }, 15000);

    it('survives a dropped handler closing a sibling subscriber in the topic mid-publish', async () => {
        let didDrop = false,
            victimClosed = false,
            drainApp = buildDrainApp((_, subscribers) => {
                didDrop = true;
                if (!victimClosed) {
                    victimClosed = true;
                    subscribers[1].close();
                }
            }, 'd'.repeat(15000), 500),
            drainServer = listen(drainApp),
            paused = await connect(`${drainServer.url.replace('http', 'ws')}/drain`),
            victim = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        victim.send('subscribe');
        paused.send('subscribe');
        await message(victim);
        await message(paused);
        socketOf(paused).pause();
        expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
        expect(didDrop).toBe(true);
        let fresh = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        fresh.send('subscribe');
        await message(fresh);
        drainApp.publish('drain', 'final');
        expect(await receive(fresh, 1)).toEqual(['final']);
        socketOf(paused).resume();
        paused.close();
        victim.close();
        fresh.close();
        drainServer.close();
    }, 15000);

    it('survives a dropped handler ending its socket while publishing big messages', async () => {
        let didDrop = false,
            drainApp = buildDrainApp((ws) => {
                didDrop = true;
                ws.end(1013);
            }, 'b'.repeat(20000)),
            drainServer = listen(drainApp),
            paused = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        paused.send('subscribe');
        await message(paused);
        socketOf(paused).pause();
        expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
        expect(didDrop).toBe(true);
        let fresh = await connect(`${drainServer.url.replace('http', 'ws')}/drain`);

        fresh.send('subscribe');
        await message(fresh);
        drainApp.publish('drain', 'final');
        expect(await receive(fresh, 1)).toEqual(['final']);
        socketOf(paused).resume();
        paused.close();
        fresh.close();
        drainServer.close();
    }, 15000);

    it('survives a dropped handler unsubscribing the last subscriber from the published topic', async () => {
        let didDrop = false,
            events: string[] = [],
            drainApp = buildDrainApp((ws) => {
                if (!didDrop) {
                    didDrop = true;
                    ws.unsubscribe('drain');
                }
            }, 'd'.repeat(15000), 40, events),
            drainServer = listen(drainApp),
            clients: WebSocket[] = [];

        try {
            let paused = await prime(drainServer.url);

            clients.push(paused);
            await backpressure(paused);
            expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
            expect(didDrop).toBe(true);
            expect(drainApp.numSubscribers('drain')).toBe(0);
            expect(events).toEqual(['drain:1:0', 'drain:0:1']);

            let fresh = await prime(drainServer.url);

            clients.push(fresh);
            expect(drainApp.publish('drain', 'final')).toBe(true);
            expect(await receive(fresh, 1)).toEqual(['final']);
        }
        finally {
            clients.forEach((ws) => {
                socketOf(ws).resume();
                ws.terminate();
            });
            drainServer.close();
        }
    }, 15000);

    it('survives a dropped handler unsubscribing itself while a sibling stays subscribed', async () => {
        let didDrop = false,
            payload = 'd'.repeat(15000),
            events: string[] = [],
            drainApp = buildDrainApp((ws) => {
                if (!didDrop) {
                    didDrop = true;
                    ws.unsubscribe('drain');
                }
            }, payload, 40, events),
            drainServer = listen(drainApp),
            clients: WebSocket[] = [];

        try {
            let sibling = await prime(drainServer.url);

            clients.push(sibling);
            let paused = await prime(drainServer.url);

            clients.push(paused);
            await backpressure(paused);
            expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
            expect(didDrop).toBe(true);
            expect(drainApp.numSubscribers('drain')).toBe(1);
            expect(events).toEqual(['drain:1:0', 'drain:2:1', 'drain:1:2']);
            expect(await receive(sibling, 40)).toEqual(Array(40).fill(payload));
        }
        finally {
            clients.forEach((ws) => {
                socketOf(ws).resume();
                ws.terminate();
            });
            drainServer.close();
        }
    }, 15000);

    it('survives a dropped handler unsubscribing during a big publish', async () => {
        let didDrop = false,
            events: string[] = [],
            drainApp = buildDrainApp((ws) => {
                if (!didDrop) {
                    didDrop = true;
                    ws.unsubscribe('drain');
                }
            }, 'b'.repeat(20000), 5, events),
            drainServer = listen(drainApp),
            clients: WebSocket[] = [];

        try {
            let paused = await prime(drainServer.url);

            clients.push(paused);
            await backpressure(paused);
            expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
            expect(didDrop).toBe(true);
            expect(drainApp.numSubscribers('drain')).toBe(0);
            expect(events).toEqual(['drain:1:0', 'drain:0:1']);
        }
        finally {
            clients.forEach((ws) => {
                socketOf(ws).resume();
                ws.terminate();
            });
            drainServer.close();
        }
    }, 15000);

    it('orders a publish issued from dropped inside the overflow drain after the in-flight message', async () => {
        let didDrop = false,
            payload = 'd'.repeat(15000),
            drainApp = buildDrainApp(() => {
                if (!didDrop) {
                    didDrop = true;
                    drainApp.publish('drain', 'lag');
                }
            }, payload, 40),
            drainServer = listen(drainApp),
            clients: WebSocket[] = [];

        try {
            let sibling = await prime(drainServer.url);

            clients.push(sibling);
            let paused = await prime(drainServer.url);

            clients.push(paused);
            await backpressure(paused);
            expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
            expect(didDrop).toBe(true);
            // The deferred 'lag' is delivered exactly once, after the in-flight batch, with no
            // loss or duplication of the 40 flood messages (exact interleave index is timing-dependent).
            let received = await receive(sibling, 41);

            expect(received.filter((m) => m === 'lag')).toEqual(['lag']);
            expect(received.filter((m) => m === payload)).toHaveLength(40);
            expect(received.indexOf('lag')).toBeGreaterThan(0);
        }
        finally {
            clients.forEach((ws) => {
                socketOf(ws).resume();
                ws.terminate();
            });
            drainServer.close();
        }
    }, 15000);

    for (let variant of ['app', 'ws']) {
        it(`delivers a ${variant} publish issued from dropped inside the loop drain on the next drain`, async () => {
            let didDrop = false,
                payload = 'd'.repeat(1024),
                drainApp = buildDrainApp((ws) => {
                    if (!didDrop) {
                        didDrop = true;
                        if (variant === 'app') {
                            drainApp.publish('drain', 'lag');
                        }
                        else {
                            ws.publish('drain', 'lag');
                        }
                    }
                }, payload, 20),
                drainServer = listen(drainApp),
                clients: WebSocket[] = [];

            try {
                let sibling = await prime(drainServer.url);

                clients.push(sibling);
                let paused = await prime(drainServer.url);

                clients.push(paused);
                await backpressure(paused);
                expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
                expect(await receive(sibling, 20)).toEqual(Array(20).fill(payload));
                expect(didDrop).toBe(true);
                expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
                expect(await receive(sibling, 21)).toEqual(['lag', ...Array(20).fill(payload)]);
            }
            finally {
                clients.forEach((ws) => {
                    socketOf(ws).resume();
                    ws.terminate();
                });
                drainServer.close();
            }
        }, 20000);
    }

    it('applies a subscribe issued from dropped once the publish completes', async () => {
        let didDrop = false,
            events: string[] = [],
            drainApp = buildDrainApp((_, subscribers) => {
                if (!didDrop) {
                    didDrop = true;
                    subscribers[1].subscribe('drain');
                }
            }, 'd'.repeat(15000), 40, events),
            drainServer = listen(drainApp),
            clients: WebSocket[] = [];

        try {
            let paused = await prime(drainServer.url);

            clients.push(paused);
            let newcomer = await prime(drainServer.url, false);

            clients.push(newcomer);
            await backpressure(paused);
            expect((await fetch(`${drainServer.url}/flood`)).status).toBe(200);
            expect(didDrop).toBe(true);
            expect(drainApp.numSubscribers('drain')).toBe(2);
            expect(events).toEqual(['drain:1:0', 'drain:2:1']);
            expect(drainApp.publish('drain', 'final')).toBe(true);

            // The newcomer can receive the remaining flood after the deferred subscribe.
            let received = '';

            for (let i = 0; i <= 40 && received !== 'final'; i++) {
                received = (await message(newcomer)).data.toString();
                expect(['d'.repeat(15000), 'final']).toContain(received);
            }
            expect(received).toBe('final');
        }
        finally {
            clients.forEach((ws) => {
                socketOf(ws).resume();
                ws.terminate();
            });
            drainServer.close();
        }
    }, 15000);

    for (let how of ['close', 'terminate'] as const) {
        it(`survives a subscription handler unsubscribing during ${how}`, async () => {
            let closeApp = App(),
                events: string[] = [];

            closeApp.ws('/subscriptions', {
                message: (ws) => {
                    ws.subscribe('t1');
                    ws.subscribe('t2');
                    ws.subscribe('t3');
                    ws.send('subscribed');
                },
                subscription: (ws, topic, newCount, oldCount) => {
                    if (newCount < oldCount) {
                        events.push(text(topic));
                        ws.unsubscribe(topic);
                    }
                }
            });

            let closeServer = listen(closeApp),
                clients: WebSocket[] = [];

            try {
                let client = await connect(`${closeServer.url.replace('http', 'ws')}/subscriptions`);

                clients.push(client);
                client.send('subscribe');
                expect((await message(client)).data.toString()).toBe('subscribed');
                expect(closeApp.numSubscribers('t1')).toBe(1);

                let done = closed(client);

                client[how]();
                await done;
                await sleep(100);
                // Each topic fires twice: once for the close-time unsubscribe, once for the handler's
                // own explicit ws.unsubscribe of the same topic (pre-existing subscription-event behavior).
                expect(events.sort()).toEqual(['t1', 't1', 't2', 't2', 't3', 't3']);
                expect(closeApp.numSubscribers('t1')).toBe(0);
                expect(closeApp.numSubscribers('t2')).toBe(0);
                expect(closeApp.numSubscribers('t3')).toBe(0);

                let fresh = await connect(`${closeServer.url.replace('http', 'ws')}/subscriptions`);

                clients.push(fresh);
                fresh.send('subscribe');
                expect((await message(fresh)).data.toString()).toBe('subscribed');
            }
            finally {
                clients.forEach((ws) => ws.terminate());
                closeServer.close();
            }
        }, 15000);
    }

    it('delivers large publish batches in order', async () => {
        let drainApp = App();

        drainApp.get('/publish', (res) => {
            for (let i = 0; i < 70000; i++) {
                drainApp.publish('ordered', `${i}`);
            }
            res.end();
        });
        drainApp.ws('/ordered', {
            maxBackpressure: 0,
            message: (ws, data) => {
                if (text(data) === 'subscribe') {
                    ws.subscribe('ordered');
                    ws.send('subscribed');
                }
            }
        });

        let drainServer = listen(drainApp),
            first = await connect(`${drainServer.url.replace('http', 'ws')}/ordered`),
            second = await connect(`${drainServer.url.replace('http', 'ws')}/ordered`);

        first.send('subscribe');
        second.send('subscribe');
        await message(first);
        await message(second);
        await fetch(`${drainServer.url}/publish`);
        expect(await receive(first, 70000)).toEqual(Array.from({ length: 70000 }, (_, i) => `${i}`));
        expect(await receive(second, 70000)).toEqual(Array.from({ length: 70000 }, (_, i) => `${i}`));
        first.close();
        second.close();
        drainServer.close();
    }, 30000);
});
