// Content hashes as CONTRACTS.md: "sha256:" up to 64 MiB, else "sha256c:" over 4 MiB chunk digests.
export const CHUNK = 4 << 20;
export const WHOLE_LIMIT = 64 << 20;

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');

export async function sha256(data) {
  return hex(await crypto.subtle.digest('SHA-256', data));
}

export async function hashBytes(data) {
  const u8 = data instanceof Uint8Array ? data : new Uint8Array(data.buffer ?? data);
  if (u8.byteLength <= WHOLE_LIMIT) return 'sha256:' + (await sha256(u8));
  return 'sha256c:' + (await chunked((o, e) => u8.subarray(o, e), u8.byteLength));
}

// blob: File or Blob; force: use the chunked form whatever the size (the source MKV)
export async function hashBlob(blob, { chunkedAlways = false } = {}) {
  if (!chunkedAlways && blob.size <= WHOLE_LIMIT) {
    return 'sha256:' + (await sha256(await blob.arrayBuffer()));
  }
  return 'sha256c:' + (await chunked(async (o, e) => blob.slice(o, e).arrayBuffer(), blob.size));
}

async function chunked(slice, size) {
  const n = Math.max(1, Math.ceil(size / CHUNK));
  const digests = new Uint8Array(n * 32);
  for (let k = 0; k < n; k++) {
    const o = k * CHUNK;
    const part = await slice(o, Math.min(size, o + CHUNK));
    digests.set(new Uint8Array(await crypto.subtle.digest('SHA-256', part)), k * 32);
  }
  return sha256(digests);
}

export async function hashJson(value) {
  return 'sha256:' + (await sha256(new TextEncoder().encode(stableStringify(value))));
}

// JSON with sorted object keys, so equal values hash equally
export function stableStringify(v) {
  if (v === undefined) return 'null';
  if (v === null || typeof v !== 'object') return JSON.stringify(v);
  if (Array.isArray(v)) return '[' + v.map(stableStringify).join(',') + ']';
  return (
    '{' +
    Object.keys(v)
      .filter((k) => v[k] !== undefined)
      .sort()
      .map((k) => JSON.stringify(k) + ':' + stableStringify(v[k]))
      .join(',') +
    '}'
  );
}
