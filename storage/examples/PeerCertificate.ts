/* Verify an optional TLS client certificate with Node's built-in crypto APIs. */

import * as crypto from 'node:crypto';
import * as fs from 'node:fs';
import * as https from 'node:https';
import * as path from 'node:path';
import * as uWS from '../../src/index';

const port = 8086;
const certificatePath = path.join(import.meta.dirname, '../../.tmp/cert.pem');
const keyPath = path.join(import.meta.dirname, '../../.tmp/key.pem');
const caCertPem = fs.readFileSync(certificatePath, 'utf8');

const app = uWS.SSLApp({
  cert_file_name: certificatePath,
  key_file_name: keyPath,
  ca_file_name: certificatePath
}).get('/*', (res) => {
  const clientCert = res.getX509Certificate();

  /* A connection is allowed not to send a client certificate. */
  if (!clientCert) {
    res.end('Hello World! no client certificate was sent.');
    return;
  }

  const x509 = new crypto.X509Certificate(clientCert);
  if (x509.verify(crypto.createPublicKey(caCertPem))) {
    res.end('Hello World! your certificate is valid!');
  } else {
    res.end('Hello World! your certificate is invalid.');
  }
}).listen(port, (token) => {
  if (token) {
    console.log('Listening to port ' + port);
    sendClientRequest();
  } else {
    console.log('Failed to listen to port ' + port);
  }
});

function sendClientRequest(): void {
  const req = https.request({
    hostname: 'localhost',
    port,
    path: '/',
    method: 'GET',
    key: fs.readFileSync(keyPath),
    cert: caCertPem,
    ca: caCertPem,
    rejectUnauthorized: false
  }, (res) => {
    let data = '';
    res.on('data', (chunk: Buffer) => { data += chunk; });
    res.on('end', () => { console.log('Response from server:', data); });
  });

  req.on('error', (error) => { console.error('Request failed:', error); });
  req.end();
}
