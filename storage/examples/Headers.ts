/* Example of looping over all headers (not recommended as production solution,
 * do NOT use this to "solve" your problems with headers, use ONLY for debugging) */

import * as uWS from '../../src/index';
const port = 9001;

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;'
  })[character]!);
}

const app = uWS./*SSL*/App({
  key_file_name: '.tmp/key.pem',
  cert_file_name: '.tmp/cert.pem',
  passphrase: '1234'
}).get('/*', (res, req) => {

  res.write('<h2>Hello, your headers are:</h2><ul>');

  req.forEach((k, v) => {
    res.write('<li>');
    res.write(escapeHtml(k));
    res.write(' = ');
    res.write(escapeHtml(v));
    res.write('</li>');
  });

  res.end('</ul>');

}).listen(port, (token) => {
  if (token) {
    console.log('Listening to port ' + port);
  } else {
    console.log('Failed to listen to port ' + port);
  }
});
