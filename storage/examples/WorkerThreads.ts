/* This example uses SO_REUSEPORT for multi-core load balancing.
 * It requires a platform with SO_REUSEPORT support (such as Linux). Each worker
 * listens on the same port and the kernel distributes incoming connections.
 * Note that, in this example we only create 2 worker threads. Ideally you should create as many as there are CPUs
 * in your system. But by only creating 2 here, it is simple to see the perf. gain on a system of 4 cores, as you can then
 * run the client side on the remaining 2 cores without interfering with the server side. */

import * as uWS from '../../src/index';
const port = 9001;
import { isMainThread, threadId, Worker } from 'node:worker_threads';

if (isMainThread) {

  /* Spawn two workers for a compact demonstration; use one per CPU in production. */
  [0, 1].forEach(() => new Worker(import.meta.filename));

  /* I guess main thread joins by default? */
} else {
  /* Here we are inside a worker thread */
  uWS./*SSL*/App({
    key_file_name: '.tmp/key.pem',
    cert_file_name: '.tmp/cert.pem',
    passphrase: '1234'
  }).get('/*', (res, req) => {
    res.end('Hello Worker!');
  }).listen(4000, (token) => {
	if (token) {
		console.log('Listening to port ' + 4000 + ' from thread ' + threadId);
	  } else {
		console.log('Failed to listen to port ' + 4000 + ' from thread ' + threadId);
	  }
  });

}
