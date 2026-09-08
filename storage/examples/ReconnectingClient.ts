/* The engine deliberately has no built-in reconnect; this is the JavaScript pattern. */

import { Client, type ConnectBehavior } from '../../src/index';

const url = 'ws://127.0.0.1:9001';
const baseDelay = 250;
const maxDelay = 30_000;
const client = Client();
let attempts = 0;

function scheduleReconnect(reason: string): void {
    let delay = Math.min(maxDelay, baseDelay * 2 ** attempts);
    let jitter = Math.floor(Math.random() * 250);

    attempts++;
    console.log(`Reconnecting after ${reason} in ${delay + jitter}ms`);
    setTimeout(connect, delay + jitter);
}

function connect(): void {
    let behavior: ConnectBehavior<undefined> = {
        open: (_ws) => {
            attempts = 0;
            console.log('Connected');
        },
        message: (_ws, _message, _isBinary) => {
            // Handle messages here.
        },
        failed: (error) => {
            console.error(`Connection failed: ${error.code}: ${error.message}`);
            scheduleReconnect(`failure (${error.code})`);
        },
        close: (_ws, code, _message) => {
            console.log(`Connection closed: ${code}`);
            scheduleReconnect(`close (${code})`);
        }
    };

    client.connect<undefined>(url, behavior);
}

connect();
