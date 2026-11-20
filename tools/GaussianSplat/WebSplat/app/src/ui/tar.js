// A ustar archive as a Blob of headers and the files themselves: the browser reads the files
// (e.g. from OPFS) only when the archive is downloaded, so nothing is copied into memory.
// entries: [{name (< 100 bytes, ASCII), file (Blob)}]
export function tarBlob(entries, mtime = 0) {
  const parts = [];
  for (const { name, file } of entries) {
    if (name.length >= 100 || /[^\x20-\x7e]/.test(name)) throw new Error(`tar name ${name}`);
    const h = new Uint8Array(512);
    const put = (s, at) => h.set(new TextEncoder().encode(s), at);
    const oct = (v, len) => v.toString(8).padStart(len - 1, '0');
    put(name, 0);
    put(oct(0o644, 8), 100);
    put(oct(0, 8), 108);
    put(oct(0, 8), 116);
    put(oct(file.size, 12), 124);
    put(oct(mtime, 12), 136);
    put('        ', 148); // checksum field counts as spaces
    put('0', 156);
    put('ustar\x0000', 257);
    let sum = 0;
    for (const b of h) sum += b;
    put(`${oct(sum, 7)}\x00`, 148);
    parts.push(h, file);
    const pad = (512 - (file.size % 512)) % 512;
    if (pad) parts.push(new Uint8Array(pad));
  }
  parts.push(new Uint8Array(1024)); // two zero blocks end the archive
  return new Blob(parts, { type: 'application/x-tar' });
}
