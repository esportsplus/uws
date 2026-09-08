/* Same as HelloWorld, but with automatic port selection. */

import * as uWS from '../../src/index';
let port = 0; 

const app = uWS./*SSL*/App({
  key_file_name: '.tmp/key.pem',
  cert_file_name: '.tmp/cert.pem',
  passphrase: '1234'
}).get('/*', (res, req) => {
  res.end('Hello World!');
}).listen(port, (token) => {
  if (token) {
    // retrieve listening port
    port = uWS.us_socket_local_port(token);
    console.log('Listening to port ' + port);
  } else {
    console.log('Failed finding available port');
  }
});
