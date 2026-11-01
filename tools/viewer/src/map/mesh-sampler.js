// Cesium1.146 exposes interpolation but scans every triangle for each query.
// This guarded adapter indexes that pinned quantized layout once per tile.
// Unknown layouts retain the public interpolation path for compatibility.
export function meshSampler(data) {
  const u = data._uValues,
    v = data._vValues,
    h = data._heightValues,
    indices = data._indices;
  const rawBuffers = new Set(
    Object.values(data)
      .filter(ArrayBuffer.isView)
      .map((a) => a.buffer),
  );
  const rawBytes = [...rawBuffers].reduce((sum, b) => sum + b.byteLength, 0);
  const min = data._minimumHeight,
    max = data._maximumHeight;
  if (
    !ArrayBuffer.isView(u) ||
    !ArrayBuffer.isView(v) ||
    !ArrayBuffer.isView(h) ||
    !ArrayBuffer.isView(indices) ||
    u.length !== v.length ||
    u.length !== h.length ||
    !Number.isFinite(min) ||
    !Number.isFinite(max)
  ) {
    return {
      sample: (rectangle, lon, lat) => data.interpolateHeight(rectangle, lon, lat),
      bytes: rawBytes,
    };
  }
  const n = 32,
    short = 32767;
  const cell = (value) => Math.min(n - 1, Math.max(0, Math.floor((value * n) / short)));
  const buckets = Array.from({ length: n * n }, () => []);
  let count = 0;
  for (let i = 0; i < indices.length; i += 3) {
    const a = indices[i],
      b = indices[i + 1],
      c = indices[i + 2];
    const x0 = cell(Math.min(u[a], u[b], u[c])),
      x1 = cell(Math.max(u[a], u[b], u[c]));
    const y0 = cell(Math.min(v[a], v[b], v[c])),
      y1 = cell(Math.max(v[a], v[b], v[c]));
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++) {
        buckets[y * n + x].push(i);
        if (++count > 4_000_000)
          return { sample: (r, lon, lat) => data.interpolateHeight(r, lon, lat), bytes: rawBytes };
      }
  }
  const cells = buckets.map((b) => Uint32Array.from(b));
  const buffers = new Set(
    Object.values(data)
      .filter(ArrayBuffer.isView)
      .map((a) => a.buffer),
  );
  const bytes = [...buffers].reduce((sum, b) => sum + b.byteLength, 0) + count * 4 + n * n * 32;
  return {
    bytes,
    sample(rectangle, lon, lat) {
      const x = Math.min(short, Math.max(0, ((lon - rectangle.west) / rectangle.width) * short));
      const y = Math.min(short, Math.max(0, ((lat - rectangle.south) / rectangle.height) * short));
      for (const i of cells[cell(y) * n + cell(x)]) {
        const a = indices[i],
          b = indices[i + 1],
          c = indices[i + 2];
        const denominator = (v[b] - v[c]) * (u[a] - u[c]) + (u[c] - u[b]) * (v[a] - v[c]);
        if (denominator === 0) continue;
        const w0 = ((v[b] - v[c]) * (x - u[c]) + (u[c] - u[b]) * (y - v[c])) / denominator;
        const w1 = ((v[c] - v[a]) * (x - u[c]) + (u[a] - u[c]) * (y - v[c])) / denominator;
        const w2 = 1 - w0 - w1;
        if (Math.min(w0, w1, w2) >= -1e-12)
          return min + ((w0 * h[a] + w1 * h[b] + w2 * h[c]) * (max - min)) / short;
      }
      return undefined;
    },
  };
}
