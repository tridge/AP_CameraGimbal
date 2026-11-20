/** Stage output store in the origin private file system, rooted at a directory path.
 * Interface (also implemented for Node in test/stage2/node-io.mjs):
 *   write(name, bytes|string), read(name) -> Uint8Array, remove(name),
 *   open(name) -> {write(bytes, at), read(length, at) -> Uint8Array, truncate(size), size(), close()}
 * open() uses sync access handles, so it needs a dedicated worker.
 */
import { dir as appDir } from '../../storage/opfs.js';

export class OpfsStore {
  /** dir: path below the app's OPFS root (storage/opfs.js), e.g. a stage's msg.outDir */
  /** faultAfterBytes: tests only, fail writes past this many bytes as a full quota would */
  static async open(dir, { clear = false, faultAfterBytes = null } = {}) {
    let handle = await appDir('');
    const parts = dir.split('/').filter(Boolean);
    for (const [k, part] of parts.entries()) {
      if (clear && k === parts.length - 1)
        await handle.removeEntry(part, { recursive: true }).catch(() => {});
      handle = await handle.getDirectoryHandle(part, { create: true });
    }
    const store = new OpfsStore(handle);
    store.faultAfterBytes = faultAfterBytes;
    return store;
  }
  constructor(root) {
    this.root = root;
    this.faultAfterBytes = null;
    this.dirs = new Map();
    this.bytesWritten = 0;
  }
  async _parent(name, create) {
    const parts = name.split('/');
    const file = parts.pop();
    let dir = this.root;
    let key = '';
    for (const part of parts) {
      key += part + '/';
      if (!this.dirs.has(key)) this.dirs.set(key, await dir.getDirectoryHandle(part, { create }));
      dir = this.dirs.get(key);
    }
    return [dir, file];
  }
  async open(name) {
    const [dir, file] = await this._parent(name, true);
    const fh = await dir.getFileHandle(file, { create: true });
    if (typeof fh.createSyncAccessHandle !== 'function')
      throw new Error('random-access OPFS files need a dedicated worker');
    const access = await fh.createSyncAccessHandle();
    const store = this;
    return {
      write(bytes, at) {
        store._fault(bytes.byteLength);
        if (access.write(bytes, { at }) !== bytes.byteLength)
          throw new Error(`short write to ${name}`);
        store.bytesWritten += bytes.byteLength;
      },
      read(length, at) {
        const out = new Uint8Array(length);
        if (access.read(out, { at }) !== length) throw new Error(`short read from ${name}`);
        return out;
      },
      truncate: (size) => access.truncate(size),
      size: () => access.getSize(),
      close() {
        try {
          access.flush();
        } finally {
          access.close();
        }
      },
    };
  }
  _fault(n) {
    if (this.faultAfterBytes != null && this.bytesWritten + n > this.faultAfterBytes)
      throw new DOMException('storage quota exceeded (test fault)', 'QuotaExceededError');
  }
  async write(name, data) {
    const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data;
    this._fault(bytes.byteLength);
    const [dir, file] = await this._parent(name, true);
    const fh = await dir.getFileHandle(file, { create: true });
    const w = await fh.createWritable();
    try {
      await w.write(bytes);
      await w.close();
    } catch (e) {
      await w.abort().catch(() => {});
      throw e;
    }
    this.bytesWritten += bytes.byteLength;
  }
  async read(name) {
    const [dir, file] = await this._parent(name, false);
    const blob = await (await dir.getFileHandle(file)).getFile();
    return new Uint8Array(await blob.arrayBuffer());
  }
  async remove(name) {
    const [dir, file] = await this._parent(name, false);
    await dir.removeEntry(file).catch((e) => {
      if (e.name !== 'NotFoundError') throw e;
    });
  }
}
