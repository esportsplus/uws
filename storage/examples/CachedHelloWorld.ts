/* Minimal SSL/non-SSL example setting a five-second HTTP cache policy. */

import * as uWS from '../../src/index';
const port = 9001;

const app = uWS./*SSL*/App({
  key_file_name: '.tmp/key.pem',
  cert_file_name: '.tmp/cert.pem',
  passphrase: '1234'
}).get('/*', (res, req) => {
  res.writeHeader('Cache-Control', 'public, max-age=5').end('Hello World!');
}).listen(port, (token) => {
  if (token) {
    console.log('Listening to port ' + port);
  } else {
    console.log('Failed to listen to port ' + port);
  }
});
