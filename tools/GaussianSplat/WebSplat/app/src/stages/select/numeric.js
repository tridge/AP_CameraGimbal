/** NumPy-exact frame statistics for extract_frames.py, from a raw uint16 histogram.
 * raw_to_celsius is monotonic in the raw value, so order statistics of the Celsius image are the
 * Celsius values of the raw order statistics. Arithmetic follows NumPy 2 promotion rules
 * (float32 arrays with weak Python scalars stay float32; float32 with float64 becomes float64).
 */
const f = Math.fround;
const identity = (r) => r;

export class RawStats {
  constructor(raw) {
    this.n = raw.length;
    const hist = new Uint32Array(65536);
    for (let i = 0; i < raw.length; i++) hist[raw[i]]++;
    // sorted values are runs: value v covers ranks [start[v], start[v]+hist[v])
    this.values = [];
    this.ends = [];
    let rank = 0;
    for (let v = 0; v < 65536; v++)
      if (hist[v]) {
        rank += hist[v];
        this.values.push(v);
        this.ends.push(rank);
      }
  }
  /** k-th smallest raw value (0-based) */
  kth(k) {
    let lo = 0,
      hi = this.ends.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.ends[mid] > k) hi = mid;
      else lo = mid + 1;
    }
    return this.values[lo];
  }
  get min() {
    return this.values[0];
  }
  get max() {
    return this.values.at(-1);
  }
}

/** splat_common.raw_to_celsius: raw.astype(float32) * scale + offset - 273.15 */
export function celsiusFn(meta) {
  const scale = f(meta?.temperature_scale_k ?? 1 / 64);
  const offset = f(meta?.temperature_offset_k ?? 0);
  const k = f(273.15);
  return (r) => f(f(f(r * scale) + offset) - k);
}

function indexes(n, q) {
  const virtual = (n - 1) * q;
  const prev = Math.floor(virtual);
  return { prev, next: Math.min(prev + 1, n - 1), gamma: virtual - prev };
}

/** np.percentile(a, (q1, q2, ...)) with array q (float64 interpolation). value(k) = a's k-th. */
export function percentileArray(stats, qs, value = identity) {
  return qs.map((p) => {
    const { prev, next, gamma } = indexes(stats.n, p / 100);
    const a = value(stats.kth(prev)),
      b = value(stats.kth(next));
    // diff_b_a keeps the input dtype (float32 for Celsius), the products become float64
    const d = value === identity ? b - a : f(b - a);
    return gamma >= 0.5 ? b - d * (1 - gamma) : a + d * gamma;
  });
}

/** np.percentile(a float32, q scalar int): weak q, float32 throughout. */
export function percentileScalarF32(stats, q, value) {
  const { prev, next, gamma } = indexes(stats.n, q / 100);
  const a = value(stats.kth(prev)),
    b = value(stats.kth(next));
  const d = f(b - a);
  return gamma >= 0.5 ? f(b - f(d * f(1 - gamma))) : f(a + f(d * f(gamma)));
}

/** np.median of a float32 array */
export function medianF32(stats, value) {
  const n = stats.n,
    h = n >> 1;
  if (n % 2) return value(stats.kth(h));
  return f(f(value(stats.kth(h - 1)) + value(stats.kth(h))) / 2);
}

/** np.median of float64 values */
export function median(list) {
  const s = Float64Array.from(list).sort();
  const n = s.length,
    h = n >> 1;
  if (!n) return NaN;
  return n % 2 ? s[h] : (s[h - 1] + s[h]) / 2;
}

export { identity as rawValue };
