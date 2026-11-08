// WSMT/WSTV containers: round trip, rejection of truncated and inconsistent data, and the
// exported native fixtures (matches.bin, geometry.bin) decode with the contract's counts.
// usage: WEBSPLAT_DATA=... node test_format.mjs
import { readFileSync } from 'node:fs';
import {
  encodeMatches,
  decodeMatches,
  encodeGeometry,
  decodeGeometry,
} from '../../app/src/stages/match/format.js';

let fails = 0;
const check = (ok, what) => {
  if (!ok) {
    fails++;
    console.log('FAIL', what);
  }
};
const throws = (fn) => {
  try {
    fn();
    return false;
  } catch {
    return true;
  }
};

const res = [
  { i: 0, j: 1, status: 0, matches: new Uint32Array([0, 1, 2, 3, 4, 5]) },
  { i: 0, j: 2, status: 1, matches: new Uint32Array(0) },
  { i: 1, j: 2, status: 0, matches: new Uint32Array([7, 8]) },
];
const enc = encodeMatches(res);
const dec = decodeMatches(enc);
check(
  dec.length === 3 &&
    dec[0].matches.join() === '0,1,2,3,4,5' &&
    dec[1].status === 1 &&
    dec[2].matches.join() === '7,8',
  'WSMT round trip',
);
check(decodeMatches(new Uint8Array(enc.buffer)).length === 3, 'decode from bytes');
const odd = new Uint8Array(enc.byteLength + 1);
odd.set(new Uint8Array(enc.buffer), 1);
check(decodeMatches(odd.subarray(1)).length === 3, 'decode from an unaligned view');
// every truncation and one extra word are rejected
let truncOk = true;
for (let w = 0; w < enc.length; w++) truncOk &&= throws(() => decodeMatches(enc.slice(0, w)));
check(truncOk, 'every truncation rejected');
check(
  throws(() => decodeMatches(new Uint32Array([...enc, 0]))),
  'trailing data rejected',
);
check(
  throws(() => decodeMatches(new Uint8Array(enc.buffer).slice(0, enc.byteLength - 2))),
  'partial word rejected',
);
const bad = (f) => {
  const c = enc.slice();
  f(c);
  return throws(() => decodeMatches(c));
};
check(
  bad((c) => {
    c[0] = 1;
  }),
  'bad magic',
);
check(
  bad((c) => {
    c[1] = 2;
  }),
  'bad version',
);
check(
  bad((c) => {
    c[2] = 4;
  }),
  'pair count beyond data',
);
check(
  bad((c) => {
    c[3] = 3;
  }),
  'total not matching',
);
check(
  bad((c) => {
    c[4 + 5 * 2 + 4] = 2;
  }),
  'non-contiguous offset',
);
check(
  bad((c) => {
    c[4 + 5 * 0 + 3] = 2;
    c[4 + 5 * 2 + 4] = 2;
  }),
  'counts not summing to total',
);
check(
  bad((c) => {
    c[4 + 5 * 0 + 2] = 5;
  }),
  'unknown status',
);
check(
  bad((c) => {
    c[4 + 5 * 1 + 2] = 1;
    c[4 + 5 * 1 + 3] = 1;
    c[4 + 5 * 2 + 4] = 4;
    c[4 + 5 * 0 + 3] = 2;
  }),
  'skipped pair with matches',
);
check(
  bad((c) => {
    c[4 + 5 * 0 + 1] = 0;
  }),
  'pair joining a frame to itself',
);
check(
  throws(() => decodeMatches(enc, { counts: [8, 8] })),
  'frame index beyond counts',
);
check(
  throws(() => decodeMatches(enc, { counts: [5, 5, 9] })),
  'feature index beyond counts',
);
check(decodeMatches(enc, { counts: [8, 9, 9] }).length === 3, 'valid indices accepted');

const model = Float64Array.from({ length: 34 }, (_, k) => k + 0.5);
const genc = encodeGeometry([
  { i: 0, j: 1, config: 2, matches: new Uint32Array([1, 2]), model },
  { i: 0, j: 2, config: 0, matches: new Uint32Array(0) },
]);
const gdec = decodeGeometry(genc);
check(
  gdec[0].config === 2 && gdec[0].model[33] === 33.5 && gdec[1].model.every((x) => x === 0),
  'WSTV round trip',
);
let gtrunc = true;
for (let w = 0; w < genc.length; w++) gtrunc &&= throws(() => decodeGeometry(genc.slice(0, w)));
check(gtrunc, 'every WSTV truncation rejected');
check(
  throws(() => decodeMatches(genc)) && throws(() => decodeGeometry(enc)),
  'magic distinguishes WSMT and WSTV',
);
const gbad = genc.slice();
gbad[4 + 2] = 9;
check(
  throws(() => decodeGeometry(gbad)),
  'unknown config rejected',
);

// native fixtures
const dir = `${process.env.WEBSPLAT_DATA}/stage4/native`;
const counts = JSON.parse(readFileSync(`${dir}/index.json`, 'utf8')).counts;
const nm = decodeMatches(readFileSync(`${dir}/matches.bin`), { counts });
const nr = decodeMatches(readFileSync(`${dir}/native_raw.bin`), { counts });
const geo = decodeGeometry(readFileSync(`${dir}/geometry.bin`), { counts });
const by = {};
for (const g of geo) {
  by[g.config] ??= [0, 0];
  by[g.config][0]++;
  by[g.config][1] += g.matches.length / 2;
}
// CONTRACTS.md reference counts: config 0: 473, 2: 1101 (120637), 3: 1016 (19927), 6: 585 (247924)
check(
  JSON.stringify(by) ===
    JSON.stringify({ 0: [473, 0], 2: [1101, 120637], 3: [1016, 19927], 6: [585, 247924] }),
  'geometry.bin config counts ' + JSON.stringify(by),
);
check(nm.length === 3175 && nr.length === 3175 && geo.length === 3175, 'fixture pair counts');
check(
  geo.every((g) => g.config === 0 || g.model.slice(27, 31).some((x) => x !== 0)),
  'verified pairs carry a pose',
);

console.log(
  fails
    ? `FAIL ${fails} format checks`
    : 'PASS containers validated (round trip, truncation, corruption, fixtures)',
);
process.exit(fails ? 1 : 0);
