// A copy of an APCG thermal MKV with the vehicle telemetry removed from every frame's metadata
// (a camera recorded without its autopilot link): each BlockAddition's apcg.thermal.v1 JSON keeps
// its radiometry, and its "telemetry" member (with one adjoining comma) is overwritten with spaces,
// so the JSON stays valid and every EBML size and cue offset is unchanged.
// usage: node strip_telemetry.mjs IN.mkv OUT.mkv
import { readFileSync, writeFileSync } from 'node:fs';

const [inp, out] = process.argv.slice(2);
if (!out) throw new Error('usage: node strip_telemetry.mjs IN.mkv OUT.mkv');
const b = Buffer.from(readFileSync(inp));
const key = Buffer.from('"telemetry":{');
let at = 0;
let n = 0;
for (;;) {
  const k = b.indexOf(key, at);
  if (k < 0) break;
  // the object's end: balanced braces, skipping strings
  let i = k + key.length - 1;
  let depth = 0;
  let inStr = false;
  for (; i < b.length; i++) {
    const c = b[i];
    if (inStr) {
      if (c === 0x5c) i++;
      else if (c === 0x22) inStr = false;
    } else if (c === 0x22) inStr = true;
    else if (c === 0x7b) depth++;
    else if (c === 0x7d && --depth === 0) break;
  }
  if (depth !== 0) throw new Error(`unbalanced telemetry object at ${k}`);
  let start = k;
  let end = i + 1;
  if (b[end] === 0x2c) end++; // "telemetry":{..}, -> drop the following comma
  else if (b[start - 1] === 0x2c) start--; // the last member: drop the preceding comma
  b.fill(0x20, start, end);
  n++;
  at = end;
}
if (!n) throw new Error('no telemetry members found');
writeFileSync(out, b);
console.log(`removed telemetry from ${n} frame metadata records -> ${out}`);
