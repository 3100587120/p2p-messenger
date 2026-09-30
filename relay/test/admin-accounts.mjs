import WebSocket from '../standalone/node_modules/ws/index.js';
import { HttpsProxyAgent } from 'https-proxy-agent';
import { createHash, createHmac, randomUUID } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname } from 'node:path';

// Explicitly authorized operator action. Never log credentials or account rows.
const [endpoint, credentialFile, backupFile, proxy, mode='backup'] = process.argv.slice(2);
if (!['backup','backup-and-reset'].includes(mode) || !backupFile || existsSync(backupFile)) throw new Error('Choose a new private backup path and an explicit action');
const secret=Buffer.from(readFileSync(credentialFile,'utf8').trim(),'base64');
if (secret.length!==32 || new URL(endpoint).protocol!=='wss:') throw new Error('Invalid credential or TLS endpoint');
const url=new URL('/connect',endpoint); url.searchParams.set('id',createHash('sha256').update(randomUUID()).digest('hex'));
const socket=new WebSocket(url,{handshakeTimeout:15000,...(proxy?{agent:new HttpsProxyAgent(proxy)}:{})});
let request, action='backup', done=false;
const deadline=setTimeout(()=>{socket.terminate(); if(!done){console.error('ACCOUNT_ADMIN_TIMEOUT');process.exitCode=1;}},30000);
function send(next) {
  action=next; request=randomUUID(); const timestamp=Math.floor(Date.now()/1000),nonce=randomUUID();
  const mac=createHmac('sha256',secret).update(`SD-ADMIN|${action}|${timestamp}|${nonce}`).digest('hex');
  socket.send(JSON.stringify({op:'admin_directory',id:request,action,timestamp,nonce,mac}));
}
socket.on('open',()=>send('backup'));
socket.on('message',data=>{
  const frame=JSON.parse(data); if(frame.id!==request)return;
  if(frame.op==='admin_directory_denied'){console.error('ACCOUNT_ADMIN_DENIED');process.exitCode=1;socket.close();return;}
  if(frame.op!=='admin_directory_result')return;
  if(frame.error || !Number.isInteger(frame.accounts)) { console.error('ACCOUNT_ADMIN_REJECTED');process.exitCode=1;socket.close();return; }
  if(action==='backup') {
    if(!frame.records || typeof frame.records!=='object')throw new Error('No valid backup; reset not attempted');
    mkdirSync(dirname(backupFile),{recursive:true});
    writeFileSync(backupFile,JSON.stringify({endpoint,created:new Date().toISOString(),records:frame.records},null,2),{flag:'wx',mode:0o600});
    const verified=JSON.parse(readFileSync(backupFile,'utf8'));
    if(Object.keys(verified.records).length!==Object.keys(frame.records).length)throw new Error('Backup verification failed; reset not attempted');
    console.log('ACCOUNT_BACKUP_VERIFIED count='+frame.accounts);
    if(mode==='backup-and-reset')send('reset'); else {done=true;socket.close();}
  } else { if(frame.accounts!==0)throw new Error('Reset did not complete'); done=true;console.log('ALL_RELAY_ACCOUNTS_RESET=PASS');socket.close(); }
});
socket.on('error',()=>{console.error('ACCOUNT_ADMIN_CONNECTION_FAILED');process.exitCode=1;});
socket.on('close',()=>{clearTimeout(deadline);secret.fill(0);if(!done)process.exitCode=1;});
