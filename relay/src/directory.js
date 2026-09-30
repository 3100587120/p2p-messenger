// Public account directory only. No passwords, private keys or chat contents.
export class UidDirectory {
  constructor(storage, reserved = {}, ownerSecret = '') { this.storage = storage; this.reserved = reserved; this.ownerSecret = ownerSecret; }
  async register(id, code, proof = {}) {
    if (!/^SD1-[A-Za-z0-9_-]{43}$/.test(code)) throw new Error('invalid_key');
    const raw = Uint8Array.from(atob(code.slice(4).replace(/-/g, '+').replace(/_/g, '/') + '='), c => c.charCodeAt(0));
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', raw))].map(v => v.toString(16).padStart(2, '0')).join('');
    if (hash !== id) throw new Error('identity_mismatch');
    let owner = false;
    const timestamp = Number(proof.timestamp);
    if (this.ownerSecret && Number.isSafeInteger(timestamp) && Math.abs(Date.now()/1000 - timestamp) < 60 && /^[0-9a-f]{64}$/.test(proof.mac || '')) {
      const secret = Uint8Array.from(atob(this.ownerSecret), c => c.charCodeAt(0));
      const key = await crypto.subtle.importKey('raw', secret, {name:'HMAC',hash:'SHA-256'}, false, ['verify']);
      const signature = Uint8Array.from(proof.mac.match(/../g), s => parseInt(s,16));
      owner = await crypto.subtle.verify('HMAC', key, signature, new TextEncoder().encode(`SD-OWNER|${id}|${timestamp}`));
    }
    return this.storage.transaction(async tx => {
      const existing = await tx.get('identity:' + id);
      if (existing) return existing;
      let uid = Object.keys(this.reserved).find(n => /^[1-9]$|^10$/.test(n) && this.reserved[n] === id);
      if (!uid && owner) for (let n = 1; n <= 10; n++) {
        if (!this.reserved[String(n)] && !await tx.get('uid:' + n)) { uid = String(n); break; }
      }
      if (uid && await tx.get('uid:' + uid)) throw new Error('reserved_uid_conflict');
      if (!uid) {
        const next = Math.max(10, Number(await tx.get('counter') ?? 10)) + 1;
        if (!Number.isSafeInteger(next)) throw new Error('directory_full');
        uid = String(next); await tx.put('counter', next);
      }
      await tx.put('identity:' + id, uid); await tx.put('uid:' + uid, code);
      return uid;
    });
  }
  async lookup(uid) {
    if (!/^[1-9][0-9]{0,15}$/.test(uid)) return null;
    return await this.storage.get('uid:' + uid) ?? null;
  }
}
