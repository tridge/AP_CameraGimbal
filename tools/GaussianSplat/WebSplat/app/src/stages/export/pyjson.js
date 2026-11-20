// JSON text as Python's json.dumps writes it (ensure_ascii, float repr), so exported headers and
// meta.json match the native exporters byte for byte. JS has no int/float distinction: numbers at
// a path in `floats` ('a.b', array elements included) are written as Python floats (8 -> 8.0).

// repr(float): shortest round-trip digits, fixed notation for 1e-4 <= |x| < 1e16
export function pyFloat(x) {
  if (Number.isNaN(x)) return 'NaN';
  if (!Number.isFinite(x)) return x > 0 ? 'Infinity' : '-Infinity';
  if (x === 0) return Object.is(x, -0) ? '-0.0' : '0.0';
  const [m, e] = x.toExponential().split('e');
  const exp = parseInt(e, 10);
  const sign = m[0] === '-' ? '-' : '';
  const digits = m.replace('-', '').replace('.', '');
  if (exp >= -4 && exp < 16) {
    if (exp < 0) return `${sign}0.${'0'.repeat(-exp - 1)}${digits}`;
    if (digits.length <= exp + 1) return `${sign}${digits}${'0'.repeat(exp + 1 - digits.length)}.0`;
    return `${sign}${digits.slice(0, exp + 1)}.${digits.slice(exp + 1)}`;
  }
  const mant = digits.length > 1 ? `${digits[0]}.${digits.slice(1)}` : digits;
  return `${sign}${mant}e${exp < 0 ? '-' : '+'}${String(Math.abs(exp)).padStart(2, '0')}`;
}

function pyString(s) {
  return JSON.stringify(s).replace(
    /[^\x20-\x7e]/g,
    (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`,
  );
}

// json.dumps(v, separators=(',', ':')) when indent is null, json.dumps(v, indent=n) otherwise
export function pyJson(value, { floats = [], indent = null } = {}) {
  const floatPaths = new Set(floats);
  const nl = (depth) => (indent === null ? '' : `\n${' '.repeat(indent * depth)}`);
  const item = indent === null ? ',' : ',';
  const kv = indent === null ? ':' : ': ';
  const out = (v, path, depth) => {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number') {
      return floatPaths.has(path) || !Number.isInteger(v) ? pyFloat(v) : String(v);
    }
    if (typeof v === 'string') return pyString(v);
    if (ArrayBuffer.isView(v)) v = Array.from(v);
    if (Array.isArray(v)) {
      if (!v.length) return '[]';
      const parts = v.map((x) => nl(depth + 1) + out(x, path, depth + 1));
      return `[${parts.join(item)}${nl(depth)}]`;
    }
    const keys = Object.keys(v).filter((k) => v[k] !== undefined);
    if (!keys.length) return '{}';
    const parts = keys.map(
      (k) =>
        `${nl(depth + 1)}${pyString(k)}${kv}${out(v[k], path ? `${path}.${k}` : k, depth + 1)}`,
    );
    return `{${parts.join(item)}${nl(depth)}}`;
  };
  return out(value, '', 0);
}
