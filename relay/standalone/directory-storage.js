import { readFileSync, existsSync, mkdirSync, writeFileSync, renameSync } from 'node:fs';
import { dirname } from 'node:path';
// Public UID bindings persist independently of online WebSocket connections.
export class FileDirectoryStorage {
  constructor(path) {
    this.path = path; this.data = existsSync(path) ? JSON.parse(readFileSync(path,'utf8')) : {};
    this.queue = Promise.resolve();
  }
  async get(key) { return this.data[key]; }
  transaction(action) {
    const result = this.queue.then(async () => {
      const draft = {...this.data};
      const value = await action({get: async k => draft[k], put: async (k,v) => { draft[k] = v; }});
      mkdirSync(dirname(this.path), {recursive:true});
      writeFileSync(this.path + '.next', JSON.stringify(draft), {mode:0o600});
      renameSync(this.path + '.next',this.path); this.data = draft; return value;
    });
    this.queue = result.catch(() => {}); return result;
  }
}
