/** Node stand-ins for the browser decoder and OPFS store (same interfaces). */
import fs from 'node:fs';
import path from 'node:path';

/** Serves native_dump.py's raw frames as a stage 2 decoder. */
export class RawDirDecoder {
  constructor(ingest, dir) {
    this.ingest = ingest;
    this.dir = dir;
    this.decoded = 0;
  }
  async *decode(wanted) {
    for (const index of [...new Set(wanted)].sort((a, b) => a - b)) {
      const b = fs.readFileSync(path.join(this.dir, String(index).padStart(5, '0') + '.u16'));
      const data = new Uint16Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
      this.decoded++;
      yield { index, meta: this.ingest.frames[index].meta, data };
    }
  }
}

export class FsStore {
  constructor(root) {
    this.root = root;
    this.bytesWritten = 0;
    fs.rmSync(root, { recursive: true, force: true });
    fs.mkdirSync(root, { recursive: true });
  }
  _path(name) {
    const p = path.join(this.root, name);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    return p;
  }
  async open(name) {
    const fd = fs.openSync(this._path(name), 'w+');
    const store = this;
    return {
      write(bytes, at) {
        fs.writeSync(fd, bytes, 0, bytes.byteLength, at);
        store.bytesWritten += bytes.byteLength;
      },
      read(length, at) {
        const out = new Uint8Array(length);
        if (fs.readSync(fd, out, 0, length, at) !== length) throw new Error('short read');
        return out;
      },
      truncate: (size) => fs.ftruncateSync(fd, size),
      size: () => fs.fstatSync(fd).size,
      close: () => fs.closeSync(fd),
    };
  }
  async write(name, data) {
    fs.writeFileSync(this._path(name), data);
  }
  async read(name) {
    return new Uint8Array(fs.readFileSync(path.join(this.root, name)));
  }
  async remove(name) {
    fs.rmSync(path.join(this.root, name), { force: true });
  }
}
