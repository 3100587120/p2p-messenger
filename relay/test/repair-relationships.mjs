import WebSocket from '../standalone/node_modules/ws/index.js';
import { HttpsProxyAgent } from 'https-proxy-agent';
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash,createHmac,randomUUID,randomBytes,createPrivateKey,createPublicKey,diffieHellman,createCipheriv } from 'node:crypto';
// Scoped operator repair. Credentials, passwords and account rows never go to logs.
const [endpoint,credentialFile,backupFile,resultFile,proxy]=process.argv.slice(2);
const secret=Buffer.from(readFileSync(credentialFile,'utf8').trim(),'base64');if(secret.length!==32 || new URL(endpoint).protocol!=='wss:')throw new Error('Invalid TLS or operator credential');
const backup=JSON.parse(readFileSync(backupFile,'utf8')),rows=backup.records;if(backup.endpoint!==endpoint)throw new Error('Backup endpoint mismatch');
const seed=randomBytes(32),privateKey=createPrivateKey({key:Buffer.concat([Buffer.from('302e020100300506032b656e04220420','hex'),seed]),format:'der',type:'pkcs8'});seed.fill(0);
const pk=createPublicKey(privateKey).export({format:'der',type:'spki'}).subarray(-32),from=createHash('sha256').update(pk).digest('hex');
const repairs=['1','11','12','13'].map(uid=>{
  const code=rows['uid:'+uid];if(!/^SD1-[A-Za-z0-9_-]{43}$/.test(code||''))throw new Error('All four UIDs must exist');
  const peers=(uid==='1'?['11','12','13']:['1']).map(n=>({uid:n,code:rows['uid:'+n],name:rows['auth:'+n]?.name||'UID '+n}));
  const pub=Buffer.from(code.slice(4),'base64url'),to=createHash('sha256').update(pub).digest('hex'),id=randomUUID();
  const key=diffieHellman({privateKey,publicKey:createPublicKey({key:Buffer.concat([Buffer.from('302a300506032b656e032100','hex'),pub]),format:'der',type:'spki'})});
  const derived=createHash('sha256').update(Buffer.concat([Buffer.from('ShuangDianLiao relay v1\0'),key,pk,pub])).digest();key.fill(0);
  const nonce=randomBytes(12),cipher=createCipheriv('aes-256-gcm',derived,nonce);cipher.setAAD(Buffer.from(`SD1|${id}|${from}|${to}`));
  const ct=Buffer.concat([cipher.update(JSON.stringify({type:'operator_friend_repair',uid,code,peers})),cipher.final(),cipher.getAuthTag()]);derived.fill(0);
  const envelope=Buffer.from(JSON.stringify({v:1,pk:pk.toString('base64url'),nonce:nonce.toString('base64url'),ct:ct.toString('base64url')})).toString('base64url');return {uid,code,id,from,envelope};
});
const timestamp=Math.floor(Date.now()/1000),nonce=randomUUID(),id=randomUUID();
const digest=createHash('sha256').update(JSON.stringify(repairs)).digest('hex'),mac=createHmac('sha256',secret).update(`SD-ADMIN|repair_friends|${timestamp}|${nonce}|${digest}`).digest('hex');secret.fill(0);
const url=new URL('/connect',endpoint);url.searchParams.set('id',from);
const socket=new WebSocket(url,{handshakeTimeout:15000,...(proxy?{agent:new HttpsProxyAgent(proxy)}:{})});let ok=false;
const timeout=setTimeout(()=>socket.terminate(),30000);
socket.on('open',()=>socket.send(JSON.stringify({op:'admin_directory',id,action:'repair_friends',timestamp,nonce,mac,repairs})));
socket.on('message',data=>{const frame=JSON.parse(data);if(frame.id!==id)return;if(frame.op==='admin_directory_result' && frame.repaired===4){writeFileSync(resultFile,JSON.stringify({created:new Date().toISOString(),repairs,accounts:frame.accounts},null,2),{flag:'wx',mode:0o600});ok=true;console.log('SCOPED_UID_1_AND_11_12_13_ENCRYPTED_REPAIR_STORED=PASS');}else console.error('SCOPED_REPAIR_REJECTED');socket.close();});
socket.on('error',()=>console.error('SCOPED_REPAIR_CONNECTION_FAILED'));socket.on('close',()=>{clearTimeout(timeout);if(!ok)process.exitCode=1;});
