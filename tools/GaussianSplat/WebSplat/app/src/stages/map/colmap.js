// Thin wrapper over the WebSplat colmap-wasm fork (colmap-wasm-patches/, built into wasm/ by
// build-colmap.sh): loading, heap transfers, progress. One instance per worker; calls are
// synchronous, so cancellation happens between calls (or by terminating the worker).

// licences and notices shipped with the wasm (build-colmap.sh copies them; referenced here so
// the app build emits them next to websplat-colmap.wasm, for an about/licences page)
export const COLMAP_NOTICES = {
  thirdParty: new URL('./wasm/THIRD_PARTY_NOTICES.md', import.meta.url).href,
  colmapWasm: new URL('./wasm/LICENSE-colmap-wasm.txt', import.meta.url).href,
  colmap: new URL('./wasm/LICENSE-COLMAP.txt', import.meta.url).href,
};

export const PHASE = { 1: 'verify', 2: 'triangulate', 3: 'ba' };

// opts: factory (the emscripten module factory), wasmBinary (Node), locateFile, onLog
export async function loadColmap(opts = {}) {
  const factory = opts.factory ?? (await import('./wasm/websplat-colmap.mjs')).default;
  const log = opts.onLog ?? (() => {});
  const M = await factory({
    wasmBinary: opts.wasmBinary,
    locateFile: opts.locateFile,
    print: (t) => log(t, 'info'),
    printErr: (t) => log(t, 'warn'),
  });
  return new Colmap(M);
}

export class Colmap {
  constructor(M) {
    this.M = M;
    this.onProgress = null; // (phase name, done, total) => truthy to stop
    M.wsProgress = (phase, done, total) =>
      this.onProgress ? this.onProgress(PHASE[phase], done, total) : 0;
  }

  get heapBytes() {
    return this.M.HEAPU8.length;
  }

  check(rc, what) {
    if (rc < 0) throw new Error(`${what}: ${this.M.UTF8ToString(this.M._ws_error())}`);
    return rc;
  }

  // copies a typed array into a fresh heap allocation; returns the pointer (free it)
  put(arr) {
    const bytes = new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength);
    const p = this.M._malloc(Math.max(1, bytes.length));
    if (!p) throw new Error(`out of wasm memory (${bytes.length} bytes)`);
    this.M.HEAPU8.set(bytes, p);
    return p;
  }

  // runs fn with the arrays on the heap (and strings as UTF-8), freeing everything allocated so
  // far on any failure
  with(arrays, fn) {
    const ptrs = [];
    try {
      for (const a of arrays) {
        if (typeof a === 'string') {
          const p = this.M.stringToNewUTF8(a);
          if (!p) throw new Error('out of wasm memory (string)');
          ptrs.push(p);
        } else ptrs.push(this.put(a));
      }
      return fn(...ptrs);
    } finally {
      for (const p of ptrs) this.M._free(p);
    }
  }

  u32(ptr, n) {
    return new Uint32Array(this.M.HEAPU8.buffer, ptr, n).slice();
  }

  f64(ptr, n) {
    return new Float64Array(this.M.HEAPU8.buffer, ptr, n).slice();
  }

  bytes(ptr, n) {
    return new Uint8Array(this.M.HEAPU8.buffer, ptr, n).slice();
  }
}
