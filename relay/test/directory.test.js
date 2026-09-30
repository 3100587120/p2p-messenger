import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { UidDirectory } from '../src/directory.js';
function key() { const raw = randomBytes(32); return { id: createHash('sha256').update(raw).digest('hex'), code: 'SD1-' + raw.toString('base64url') }; }
function storage() {
  const data = new Map(); let queue = Promise.resolve();
  const store = { get: async k => data.get(k), put: async (k,v) => data.set(k,v),
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
