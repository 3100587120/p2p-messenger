import assert from 'node:assert/strict';
import { createHash, createPrivateKey, createPublicKey, diffieHellman, randomBytes, randomUUID, createCipheriv, createDecipheriv } from 'node:crypto';

const invite = process.env.RECEIVER_INVITE_CODE ?? '';
const endpoint = process.env.RELAY_TEST_URL ?? '';
assert.match(invite, /^SD1-[A-Za-z0-9_-]{43}$/);
assert.match(endpoint, /^wss:\/\//);

const receiverPublic = Buffer.from(invite.slice(4), 'base64url');
assert.equal(receiverPublic.length, 32);
const seed = randomBytes(32);
const privateKey = createPrivateKey({
  key: Buffer.concat([Buffer.from('302e020100300506032b656e04220420', 'hex'), seed]),
  format: 'der', type: 'pkcs8',
});
const senderPublic = createPublicKey(privateKey).export({ format: 'der', type: 'spki' }).subarray(-32);
const peerKey = createPublicKey({
  key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), receiverPublic]),
  format: 'der', type: 'spki',
});
const senderId = createHash('sha256').update(senderPublic).digest('hex');
const receiverId = createHash('sha256').update(receiverPublic).digest('hex');
const shared = diffieHellman({ privateKey, publicKey: peerKey });
const derive = (from, to) => createHash('sha256').update(Buffer.concat([
  Buffer.from('ShuangDianLiao relay v1\0'), shared, from, to,
])).digest();
const aad = (id, from, to) => Buffer.from(`SD1|${id}|${from}|${to}`);
const packetId = randomUUID();
function seal(message, id) {
const nonce = randomBytes(12);
const cipher = createCipheriv('aes-256-gcm', derive(senderPublic, receiverPublic), nonce);
cipher.setAAD(aad(id, senderId, receiverId));
const payload = Buffer.from(JSON.stringify(message));
const ciphertext = Buffer.concat([cipher.update(payload), cipher.final(), cipher.getAuthTag()]);
return Buffer.from(JSON.stringify({
  v: 1,
  pk: senderPublic.toString('base64url'),
  nonce: nonce.toString('base64url'),
  ct: ciphertext.toString('base64url'),
})).toString('base64url');
}
const envelope = seal({ type: 'friend_request', probe: 'cross_wan', name: 'Cloud test' }, packetId);
const controllerMode = process.env.CONTROLLER_TEST === '1';
let requestAck = false, accepted = false, gotText = false, textAck = false;
const textId = randomUUID();

const url = new URL(endpoint);
url.pathname = '/connect';
url.searchParams.set('id', senderId);
const socket = new WebSocket(url);
function send(message, id = randomUUID()) {
  socket.send(JSON.stringify({ op: 'send', id, to: receiverId, envelope: seal(message, id) }));
}
const timeout = setTimeout(() => {
  console.error('Timed out waiting for encrypted acknowledgement from remote client');
  socket.close();
  process.exitCode = 1;
}, 60000);

socket.addEventListener('open', () => {
  socket.send(JSON.stringify({ op: 'send', id: packetId, to: receiverId, envelope }));
});
socket.addEventListener('error', (event) => {
  console.error('WebSocket error:', event.message ?? event.type);
  process.exitCode = 1;
});
socket.addEventListener('message', (event) => {
  try {
    const frame = JSON.parse(event.data);
    if (frame.op === 'relay' && frame.id === packetId) {
      assert.equal(frame.status, 'forwarded', `Relay status: ${frame.status}`);
      console.log('Cloudflare relay forwarded cross-WAN friend request');
      return;
    }
    if (frame.op !== 'packet' || frame.from !== receiverId) return;
    const wrapped = JSON.parse(Buffer.from(frame.envelope, 'base64url').toString());
    assert.equal(wrapped.v, 1);
    assert.deepEqual(Buffer.from(wrapped.pk, 'base64url'), receiverPublic);
    const encrypted = Buffer.from(wrapped.ct, 'base64url');
    const decipher = createDecipheriv('aes-256-gcm',
      derive(receiverPublic, senderPublic), Buffer.from(wrapped.nonce, 'base64url'));
    decipher.setAAD(aad(frame.id, receiverId, senderId));
    decipher.setAuthTag(encrypted.subarray(-16));
    const ack = JSON.parse(Buffer.concat([
      decipher.update(encrypted.subarray(0, -16)), decipher.final(),
    ]).toString());
    if (controllerMode) {
      if (ack.type === 'ack') {
        if (ack.id === packetId) requestAck = true;
        if (ack.id === textId) textAck = true;
      } else {
        send({ type: 'ack', id: frame.id });
        if (ack.type === 'friend_accept') {
          accepted = true;
          send({ type: 'text', body: 'cloud-to-controller' }, textId);
        }
        if (ack.type === 'text') {
          assert.equal(ack.body, 'controller-to-cloud');
          gotText = true;
        }
      }
      if (!(requestAck && accepted && gotText && textAck)) return;
      console.log('PASS: application controller persisted friend request, accepted it, and exchanged messages in both directions');
    } else {
      assert.deepEqual(ack, { type: 'ack', id: packetId });
    }
    console.log('PASS: remote client received request and returned authenticated E2EE acknowledgement');
    clearTimeout(timeout);
    socket.close();
  } catch (error) {
    console.error(error);
    clearTimeout(timeout);
    socket.close();
    process.exitCode = 1;
  }
});
