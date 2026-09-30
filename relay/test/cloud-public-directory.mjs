// Disposable QA registration. No owner credentials, app vaults, or messages are read.
import WebSocket from '../standalone/node_modules/ws/index.js';
import { HttpsProxyAgent } from 'https-proxy-agent';
import { generateKeyPairSync, createHash, randomUUID } from 'node:crypto';
const [endpoint, proxy] = process.argv.slice(2);
const raw = generateKeyPairSync('x25519').publicKey.export({format:'der',type:'spki'}).subarray(-32);
const code = 'SD1-' + raw.toString('base64url'), identity = createHash('sha256').update(raw).digest('hex');
const url = new URL('/connect',endpoint); url.searchParams.set('id',identity);
const socket = new WebSocket(url,{handshakeTimeout:15000, ...(proxy ? {agent:new HttpsProxyAgent(proxy)} : {})});
const request = randomUUID(), query = randomUUID(); let uid, passed = false;
const deadline = setTimeout(() => { console.error('PUBLIC_UID_DIRECTORY_TIMEOUT'); process.exitCode=1; socket.terminate(); },20000);
socket.on('open', () => socket.send(JSON.stringify({op:'register',id:request,code})));
socket.on('message', text => {
  const frame = JSON.parse(text);
  if (frame.op === 'registered' && frame.id === request) { uid = frame.uid; socket.send(JSON.stringify({op:'lookup',id:query,uid})); }
  if (frame.op === 'lookup_result' && frame.id === query) {
    passed = frame.uid === uid && frame.code === code && Number(uid)>=11;
    console.log('PUBLIC_UID_REGISTRATION_AND_LOOKUP=' + (passed ? 'PASS' : 'FAIL') + ' UID=' + uid + ' path=' + (proxy ? 'explicit-proxy' : 'no-explicit-proxy'));
    socket.close();
  }
  if (frame.op === 'directory_error') { console.error('PUBLIC_UID_DIRECTORY_REJECTED'); socket.close(); }
});
socket.on('error',error => { console.error('PUBLIC_UID_CONNECTION_FAILED:',error.message); process.exitCode=1; });
socket.on('close',() => { clearTimeout(deadline); if (!passed) process.exitCode=1; });
