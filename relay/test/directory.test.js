import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { UidDirectory } from '../src/directory.js';
function key() { const raw = randomBytes(32); return { id: createHash('sha256').update(raw).digest('hex'), code: 'SD1-' + raw.toString('base64url') }; }
function storage() {
  const data = new Map(); let queue = Promise.resolve();
  const store = { get: async k => data.get(k), put: async (k,v) => data.set(k,v),
    list: async()=>new Map(data), delete:async k=>data.delete(k),
    transaction: action => { const p = queue.then(() => action(store)); queue = p.catch(() => {}); return p; } };
  return store;
}
test('sequential UID starts at 11, reserved 1-10 cannot be self-selected', async () => {
  const store = storage(), owner = key(), a = key(), b = key();
  const d = new UidDirectory(store, { '1': owner.id });
  assert.deepEqual(await Promise.all([d.register(a.id,a.code),d.register(b.id,b.code)]), ['11','12']);
  assert.equal(await d.register(a.id,a.code), '11');
  assert.equal(await d.register(owner.id,owner.code), '1');
  assert.equal(await d.lookup('1'),owner.code);
  assert.equal(await d.lookup('2'),null);
  const reopened = new UidDirectory(store);
  assert.equal(await reopened.register(b.id,b.code), '12');
  assert.equal(await reopened.lookup('11'),a.code);
});

test('password account login is stable, wrong tokens rejected, no raw token stored', async () => {
  const store=storage(), d=new UidDirectory(store), a=key(), token=randomBytes(32).toString('base64url');
  const auth={v:1,salt:randomBytes(16).toString('base64url'),iterations:600000,token,backup:'a'.repeat(120),name:'昵称',code:a.code};
  const uid=await d.register(a.id,a.code,{},auth); assert.equal(uid,'11');
  assert.equal(await d.profile(uid),'昵称'); assert.equal((await d.loginInfo(uid)).salt,auth.salt);
  assert.equal((await d.login(uid,token)).code,a.code);
  assert.equal((await d.login(uid,randomBytes(32).toString('base64url'))).error,'invalid_login');
  const saved=await store.get('auth:'+uid); assert.equal(saved.token,undefined); assert.equal(saved.tokenHash.length,64);
  await assert.rejects(d.register(a.id,a.code,{}, {...auth,token:randomBytes(32).toString('base64url')}),/already_registered/);
  for (let n=0;n<9;n++) await d.login(uid,token);
  assert.equal((await d.login(uid,token)).error,'login_rate_limited');
});
test('admin reset requires operation-specific proof and cannot be replayed', async () => {
  const store=storage(), secret=randomBytes(32), d=new UidDirectory(store,{},secret.toString('base64')), a=key();
  await d.register(a.id,a.code); const timestamp=Math.floor(Date.now()/1000), nonce='11111111-1111-1111-1111-111111111111';
  const mac=createHmac('sha256',secret).update(`SD-ADMIN|backup|${timestamp}|${nonce}`).digest('hex');
  assert.equal((await d.admin('backup',timestamp,nonce,mac)).accounts,1);
  await assert.rejects(d.admin('reset',timestamp,nonce,mac),/denied/);
  await assert.rejects(d.admin('backup',timestamp,nonce,mac),/replay/);
  const resetNonce='22222222-2222-2222-2222-222222222222', resetMac=createHmac('sha256',secret).update(`SD-ADMIN|reset|${timestamp}|${resetNonce}`).digest('hex');
  assert.equal((await d.admin('reset',timestamp,resetNonce,resetMac)).accounts,0);
  assert.equal(await d.lookup('11'),null); assert.equal(await d.register(key().id,a.code).catch(()=>null),null);
  const b=key(); assert.equal(await d.register(b.id,b.code),'11');
});
test('wrong public key cannot replace UID binding', async () => {
  const d = new UidDirectory(storage()), a = key(), b = key();
  await assert.rejects(d.register(a.id,b.code), /identity_mismatch/);
  assert.equal(await d.register(a.id,a.code),'11');
  assert.equal(await d.lookup('01'),null);
});
test('owner device gets 1-10 then ordinary UID; forged owner cannot reserve', async () => {
  const secret = randomBytes(32), d = new UidDirectory(storage(), {}, secret.toString('base64'));
  const fake = key();
  assert.equal(await d.register(fake.id,fake.code,{timestamp:Math.floor(Date.now()/1000),mac:'0'.repeat(64)}),'11');
  for (let n = 1; n <= 11; n++) {
    const a = key(), timestamp = Math.floor(Date.now()/1000);
    const mac = createHmac('sha256',secret).update(`SD-OWNER|${a.id}|${timestamp}`).digest('hex');
    assert.equal(await d.register(a.id,a.code,{timestamp,mac}),String(n <= 10 ? n : 12));
  }
});
