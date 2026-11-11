// A splat file's own metadata (no DOM): the apcg_splat JSON of a PLY header or an SPZ gzip header.
import { spzMeta } from '../spz-meta.mjs';

// metadata written by train.py, train_rgb.py and export_web.py: "comment apcg_splat {json}"
export function plyMeta(bytes) {
  const head = new TextDecoder('latin1').decode(bytes.subarray(0, Math.min(bytes.length, 1 << 20)));
  const end = head.search(/\nend_header\r?\n/); // the line, not the text inside a comment
  if (!head.startsWith('ply') || end < 0) return null;
  for (const line of head.slice(0, end).split('\n')) {
    if (line.startsWith('comment apcg_splat ')) {
      try {
        return JSON.parse(line.slice('comment apcg_splat '.length));
      } catch {
        return null;
      }
    }
  }
  return null;
}

// the file's own metadata: a PLY header line or an SPZ's gzip header (spz-meta.mjs)
export function embeddedMeta(bytes) {
  return plyMeta(bytes) ?? spzMeta(bytes);
}
