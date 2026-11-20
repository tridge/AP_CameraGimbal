// Telemetry pair selection: a port of sfm.py select_pairs() (and view_geometry()).
// Same defaults as sfm.py's command line.
//
// Tie policy: view neighbours are ordered by descending cosine, then ascending frame index.
// numpy's argsort is unstable, so where a decision is within TIE_EPS of a tie (the
// neighbour cut-off falls inside a run of equal cosines, or a cosine or range ratio sits on
// its threshold) the native result is not predictable; those decisions are returned in
// `ambiguous` and parity with sfm.py holds only when it is empty.

export const PAIR_DEFAULTS = { maxAngle: 25.0, maxScale: 2.2, neighbours: 30, sequential: 5 };
export const TIE_EPS = 1e-12; // far above float64 differences between numpy's BLAS and JS

// frames: [{position: [e, n, u]}] in frame order; target: [e, n, u].
// Returns {pairs: Uint32Array of (i, j) with i < j, sorted, ambiguous: [{frame, kind, ...}]}.
export function selectPairs(frames, target, opts = {}) {
  const { maxAngle, maxScale, neighbours, sequential } = { ...PAIR_DEFAULTS, ...opts };
  const n = frames.length;
  const dx = new Float64Array(n),
    dy = new Float64Array(n),
    dz = new Float64Array(n);
  const ranges = new Float64Array(n);
  for (let k = 0; k < n; k++) {
    const p = frames[k].position;
    const x = target[0] - p[0],
      y = target[1] - p[1],
      z = target[2] - p[2];
    const r = Math.sqrt(x * x + y * y + z * z);
    ranges[k] = r;
    dx[k] = x / r;
    dy[k] = y / r;
    dz[k] = z / r;
  }
  // math.radians(x) is x * (pi / 180)
  const cosMax = Math.cos(maxAngle * (Math.PI / 180));
  const keys = new Set();
  const ambiguous = [];
  const cos = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const ok = [];
    for (let j = 0; j < n; j++) {
      if (j === i) continue;
      cos[j] = dx[j] * dx[i] + dy[j] * dy[i] + dz[j] * dz[i];
      const scale = Math.max(ranges[j] / ranges[i], ranges[i] / ranges[j]);
      if (Math.abs(cos[j] - cosMax) <= TIE_EPS)
        ambiguous.push({ frame: i, kind: 'angle-threshold', other: j });
      if (Math.abs(scale - maxScale) <= TIE_EPS * maxScale)
        ambiguous.push({ frame: i, kind: 'scale-threshold', other: j });
      if (cos[j] >= cosMax && scale <= maxScale) ok.push(j);
    }
    ok.sort((a, b) => cos[b] - cos[a] || a - b);
    if (ok.length > neighbours && cos[ok[neighbours - 1]] - cos[ok[neighbours]] <= TIE_EPS) {
      const v = cos[ok[neighbours - 1]];
      ambiguous.push({
        frame: i,
        kind: 'neighbour-cutoff',
        tied: ok.filter((j) => Math.abs(cos[j] - v) <= TIE_EPS).length,
      });
    }
    for (const j of ok.slice(0, neighbours)) keys.add(Math.min(i, j) * n + Math.max(i, j));
    for (let j = i + 1; j < Math.min(n, i + 1 + sequential); j++) keys.add(i * n + j);
  }
  const sorted = [...keys].sort((a, b) => a - b);
  const pairs = new Uint32Array(sorted.length * 2);
  sorted.forEach((k, p) => {
    pairs[2 * p] = Math.floor(k / n);
    pairs[2 * p + 1] = k % n;
  });
  return { pairs, ambiguous };
}
