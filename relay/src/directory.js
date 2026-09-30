// Public UID directory, authentication hashes and password-encrypted identity backups.
// No plaintext passwords/private keys or chat contents.
export class UidDirectory {
  constructor(storage, reserved = {}, ownerSecret = '') { this.storage = storage; this.reserved = reserved; this.ownerSecret = ownerSecret; }
  async register(id, code, proof = {}, auth = null) {
    if (!/^SD1-[A-Za-z0-9_-]{43}$/.test(code)) throw new Error('invalid_key');
    const raw = Uint8Array.from(atob(code.slice(4).replace(/-/g, '+').replace(/_/g, '/') + '='), c => c.charCodeAt(0));
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', raw))].map(v => v.toString(16).padStart(2, '0')).join('');
    if (hash !== id) throw new Error('identity_mismatch');
    let credentials = null;
    if (auth) {
      if (auth.v !== 1 || auth.iterations !== 600000 || !/^[A-Za-z0-9_-]{22}$/.test(auth.salt || '') ||
          !/^[A-Za-z0-9_-]{43}$/.test(auth.token || '') || typeof auth.backup !== 'string' || auth.backup.length > 8192 || auth.backup.length < 80 || auth.code !== code || typeof auth.name !== 'string' || !auth.name.trim() || auth.name.length > 64) throw new Error('invalid_credentials');
      const token = Uint8Array.from(atob(auth.token.replace(/-/g,'+').replace(/_/g,'/')+'='),c=>c.charCodeAt(0));
      const tokenHash = [...new Uint8Array(await crypto.subtle.digest('SHA-256',token))].map(v=>v.toString(16).padStart(2,'0')).join('');
      credentials = {v:1,salt:auth.salt,iterations:auth.iterations,backup:auth.backup,code,name:auth.name,tokenHash};
    }
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
      if (existing) {
        if (credentials) {
          const old = await tx.get('auth:' + existing);
          if (old && old.tokenHash !== credentials.tokenHash) throw new Error('account_already_registered');
          await tx.put('auth:' + existing,{...credentials,contacts:old?.contacts});
        }
        return existing;
      }
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
      if (credentials) await tx.put('auth:' + uid, credentials);
      return uid;
    });
  }
  async lookup(uid) {
    if (!/^[1-9][0-9]{0,15}$/.test(uid)) return null;
    return await this.storage.get('uid:' + uid) ?? null;
  }
  async profile(uid) { return (await this.storage.get('auth:' + uid))?.name || ''; }
  async loginInfo(uid) {
    if (!/^[1-9][0-9]{0,15}$/.test(uid)) throw new Error('invalid_login');
    const record = await this.storage.get('auth:' + uid);
    if (!record) throw new Error('invalid_login');
    return {salt:record.salt,iterations:record.iterations};
  }
  async login(uid, token) {
    if (!/^[1-9][0-9]{0,15}$/.test(uid) || !/^[A-Za-z0-9_-]{43}$/.test(token || '')) throw new Error('invalid_login');
    const raw = Uint8Array.from(atob(token.replace(/-/g,'+').replace(/_/g,'/')+'='),c=>c.charCodeAt(0));
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256',raw))].map(v=>v.toString(16).padStart(2,'0')).join('');
    return this.storage.transaction(async tx => {
      const now=Date.now(), previous=await tx.get('rate:'+uid), rate=previous && now-previous.start<60000 ? previous : {start:now,count:0};
      rate.count++; await tx.put('rate:'+uid,rate);
      if (rate.count>10) return {error:'login_rate_limited'};
      const record=await tx.get('auth:'+uid);
      let difference=0; const expected=record?.tokenHash || '0'.repeat(64);
      for (let i=0;i<64;i++) difference |= expected.charCodeAt(i)^hash.charCodeAt(i);
      if (!record || difference) return {error:'invalid_login'};
      const {tokenHash,...safe}=record; return {uid,...safe};
    });
  }
  async admin(action, timestamp, nonce, mac, repairs = null) {
    if (!['backup','reset','repair_friends'].includes(action) || !this.ownerSecret || !Number.isSafeInteger(timestamp) || Math.abs(Date.now()/1000-timestamp)>60 || !/^[0-9a-f-]{36}$/.test(nonce || '') || !/^[0-9a-f]{64}$/.test(mac || '')) throw new Error('admin_denied');
    const secret=Uint8Array.from(atob(this.ownerSecret),c=>c.charCodeAt(0));
    const key=await crypto.subtle.importKey('raw',secret,{name:'HMAC',hash:'SHA-256'},false,['verify']);
    const encoded=JSON.stringify(repairs);if(action==='repair_friends' && encoded.length>16000)throw new Error('admin_denied');
    const suffix=action==='repair_friends'?'|'+[...new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(encoded)))].map(v=>v.toString(16).padStart(2,'0')).join(''):'';
    if (!await crypto.subtle.verify('HMAC',key,Uint8Array.from(mac.match(/../g),s=>parseInt(s,16)),new TextEncoder().encode(`SD-ADMIN|${action}|${timestamp}|${nonce}${suffix}`))) throw new Error('admin_denied');
    return this.storage.transaction(async tx => {
      if (await tx.get('admin_used:'+nonce)) throw new Error('admin_replay');
      const rows=await tx.list();
      if (rows.size>=1000) throw new Error('manual_backup_required');
      const accounts=[...rows].filter(([key])=>/^(identity:|uid:|auth:|rate:|friend_repair:|counter$|generation$)/.test(key));
      await tx.put('admin_used:'+nonce,timestamp);
      if(action==='repair_friends'){
        if(!Array.isArray(repairs) || repairs.length!==4 || new Set(repairs.map(r=>r.uid)).size!==4)throw new Error('invalid_repairs');
        for(const r of repairs){if(!['1','11','12','13'].includes(r.uid) || r.code!==await tx.get('uid:'+r.uid) || !/^[0-9a-f-]{36}$/.test(r.id||'') || !/^[0-9a-f]{64}$/.test(r.from||'') || typeof r.envelope!=='string' || r.envelope.length<80 || r.envelope.length>8000)throw new Error('invalid_repairs');}
        for(const r of repairs)await tx.put('friend_repair:'+r.uid,{id:r.id,from:r.from,envelope:r.envelope});return {accounts:accounts.filter(([k])=>k.startsWith('uid:')).length,repaired:4};
      }
      if (action==='backup') return {records:Object.fromEntries(accounts),accounts:accounts.filter(([key])=>key.startsWith('uid:')).length};
      for (const [key] of accounts) await tx.delete(key);
      await tx.put('generation',nonce);
      return {accounts:0,generation:nonce};
    });
  }
  async friendRepair(identity) {const uid=await this.storage.get('identity:'+identity);return uid?await this.storage.get('friend_repair:'+uid)??null:null;}
  async contacts(uid, token, snapshot = undefined) {
    if (!/^[1-9][0-9]{0,15}$/.test(uid) || !/^[A-Za-z0-9_-]{43}$/.test(token || '')) throw new Error('invalid_login');
    if (snapshot !== undefined && (!snapshot || !/^[0-9a-f-]{36}$/.test(snapshot.id || '') || typeof snapshot.envelope !== 'string' || snapshot.envelope.length > 56000 || snapshot.envelope.length < 80)) throw new Error('invalid_backup');
    const raw=Uint8Array.from(atob(token.replace(/-/g,'+').replace(/_/g,'/')+'='),c=>c.charCodeAt(0));
    const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',raw))].map(v=>v.toString(16).padStart(2,'0')).join('');
    return this.storage.transaction(async tx => {
      const record=await tx.get('auth:'+uid);let difference=0;const expected=record?.tokenHash || '0'.repeat(64);
      for(let i=0;i<64;i++)difference|=expected.charCodeAt(i)^hash.charCodeAt(i);
      if(!record || difference)throw new Error('invalid_login');
      if(snapshot !== undefined){record.contacts={id:snapshot.id,envelope:snapshot.envelope};await tx.put('auth:'+uid,record);}
      return record.contacts || null;
    });
  }
}
