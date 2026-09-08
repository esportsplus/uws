import { createServer } from 'node:http';
import type { AddressInfo } from 'node:net';


const server = createServer((_req, res) => {
    res.end('Hello World!');
});


server.listen(Number(process.argv[2] ?? 0), () => {
    process.send?.({ port: (server.address() as AddressInfo).port });
});
