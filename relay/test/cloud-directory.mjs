import WebSocket from '../standalone/node_modules/ws/index.js';
import { readFileSync } from 'node:fs';
import { createHmac, createHash, randomUUID } from 'node:crypto';
import { HttpsProxyAgent } from 'https-proxy-agent';
// Register only this PC's existing account, not fabricated public-service users.
const [endpoint, identityFile, credentialFile, proxy] = process.argv.slice(2);
const account = JSON.parse(readFileSync(identityFile,'utf8').replace(/^\uFEFF/,''));
const key = Buffer.from(account.code.slice(4),'base64url');
if (createHash('sha256').update(key).digest('hex') !== account.id) throw new Error('public identity mismatch');
const secret = Buffer.from(readFileSync(credentialFile,'utf8').trim(),'base64');
if (secret.length !== 32) throw new Error('owner credential invalid');
const url = new URL('/connect', endpoint); url.searchParams.set('id',account.id);
const socket = new WebSocket(url,{handshakeTimeout:15000, ...(proxy ? {agent:new HttpsProxyAgent(proxy)} : {})});
const deadline = setTimeout(() => { socket.terminate(); console.error('CLOUD_DIRECTORY_TIMEOUT'); process.exitCode=1; },20000);
let uid, registration, query;
socket.on('error', e => { console.error('CLOUD_DIRECTORY_CONNECTION_FAILED:',e.message); process.exitCode=1; });
socket.on('open', () => {
  const timestamp = Math.floor(Date.now()/1000);
  const mac = createHmac('sha256',secret).update(`SD-OWNER|${account.id}|${timestamp}`).digest('hex');
  registration = randomUUID();
  socket.send(JSON.stringify({op:'register',id:registration,code:account.code,ownerProof:{timestamp,mac}}));
});
socket.on('message', text => {
  const frame = JSON.parse(text);
  if (frame.op === 'directory_error') { console.error('CLOUD_DIRECTORY_REJECTED'); process.exitCode=1; socket.close(); }
  if (frame.op === 'registered' && frame.id === registration) {
    uid = frame.uid; query = randomUUID(); socket.send(JSON.stringify({op:'lookup',id:query,uid}));
  }
  if (frame.op === 'lookup_result' && frame.id === query) {
    if (frame.uid !== uid || frame.code !== account.code || !/^[1-9]$|^10$/.test(uid)) { console.error('CLOUD_DIRECTORY_BINDING_FAILED'); process.exitCode=1; }
    else console.log('CLOUD_OWNER_UID_REGISTRATION_AND_LOOKUP=PASS UID=' + uid + ' path=' + (proxy ? 'explicit-proxy' : 'no-explicit-proxy'));
    socket.close();
  }
});
socket.on('close',() => clearTimeout(deadline));
