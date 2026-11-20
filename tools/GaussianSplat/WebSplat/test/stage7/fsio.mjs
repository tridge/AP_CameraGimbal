// Node files for exportStream (the worker uses OPFS sync access handles the same way)
import {
  closeSync,
  fstatSync,
  ftruncateSync,
  fsyncSync,
  openSync,
  readSync,
  writeSync,
} from 'node:fs';

export function fileIn(path) {
  const fd = openSync(path, 'r');
  return {
    size: fstatSync(fd).size,
    read(offset, length) {
      const u = new Uint8Array(length);
      for (let got = 0; got < length; ) {
        const n = readSync(fd, u, got, length - got, offset + got);
        if (!n) throw new Error(`${path}: short read`);
        got += n;
      }
      return u;
    },
    close: () => closeSync(fd),
  };
}

export function fileSink(path) {
  const fd = openSync(path, 'w+');
  return {
    write(bytes, offset) {
      for (let done = 0; done < bytes.length; )
        done += writeSync(fd, bytes, done, bytes.length - done, offset + done);
    },
    read(offset, length) {
      const u = new Uint8Array(length);
      for (let got = 0; got < length; ) {
        const n = readSync(fd, u, got, length - got, offset + got);
        if (!n) throw new Error(`${path}: short read`);
        got += n;
      }
      return u;
    },
    truncate: (n) => ftruncateSync(fd, n),
    flush: () => fsyncSync(fd),
    close: () => closeSync(fd),
  };
}
