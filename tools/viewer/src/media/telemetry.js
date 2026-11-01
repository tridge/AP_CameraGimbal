/** Pure, bounded telemetry decoders. Container clocks are never inferred from UTC. */
const text = new TextDecoder('utf-8', { fatal: true });
const APCG_UUID = Uint8Array.from('8d646b4e556f4a908b7c35e629510321'.match(/../g), (v) =>
  parseInt(v, 16),
);
const UAS_KEY = Uint8Array.from('060e2b34020b01010e01030101000000'.match(/../g), (v) =>
  parseInt(v, 16),
);
const TAU = Math.PI * 2;
const equal = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const wrap = (a) => ((((a + Math.PI) % TAU) + TAU) % TAU) - Math.PI;

export function* packetNals(packet, lengthSize = 0) {
  if (lengthSize) {
    if (!Number.isInteger(lengthSize) || lengthSize < 1 || lengthSize > 4) return;
    let at = 0;
    while (at + lengthSize <= packet.length) {
      let size = 0;
      for (let i = 0; i < lengthSize; i++) size = size * 256 + packet[at++];
      if (!size || at + size > packet.length) return;
      yield packet.subarray(at, at + size);
      at += size;
    }
    return;
  }
  let begin = -1;
  for (let i = 0; i + 2 < packet.length; i++) {
    if (packet[i] || packet[i + 1] || packet[i + 2] !== 1) continue;
    if (begin >= 0) {
      let end = i;
      while (end > begin && packet[end - 1] === 0) end--;
      if (end > begin) yield packet.subarray(begin, end);
    }
    begin = i + 3;
    i += 2;
  }
  if (begin >= 0) {
    let end = packet.length;
    while (end > begin && !packet[end - 1]) end--;
    if (end > begin) yield packet.subarray(begin, end);
  }
}

export function unescapeRBSP(bytes) {
  const result = new Uint8Array(bytes.length);
  let n = 0,
    zeros = 0;
  for (let i = 0; i < bytes.length; i++) {
    const value = bytes[i];
    if (zeros >= 2 && value === 3 && i + 1 < bytes.length && bytes[i + 1] <= 3) {
      zeros = 0;
      continue;
    }
    result[n++] = value;
    zeros = value === 0 ? zeros + 1 : 0;
  }
  return result.subarray(0, n);
}

// JSON numeric provenance may include nanoseconds/uint64 beyond IEEE754 exact range.
// Quote only unsafe integer tokens, never digits inside strings or floating-point values.
function exactJSON(json) {
  let result = '',
    at = 0,
    begin = 0;
  while (at < json.length) {
    if (json[at] === '"') {
      at++;
      while (at < json.length) {
        if (json[at] === '\\') {
          at += 2;
          continue;
        }
        if (json[at++] === '"') break;
      }
      continue;
    }
    if (json[at] === '-' || (json[at] >= '0' && json[at] <= '9')) {
      const match = /^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/.exec(json.slice(at));
      if (match) {
        const token = match[0];
        if (/^-?\d+$/.test(token) && !Number.isSafeInteger(Number(token))) {
          result += json.slice(begin, at) + '"' + token + '"';
          begin = at + token.length;
        }
        at += token.length;
        continue;
      }
    }
    at++;
  }
  return JSON.parse(result + json.slice(begin));
}

function jsonRecord(bytes, schema) {
  try {
    let end = bytes.length;
    while (end && bytes[end - 1] === 0) end--;
    const value = exactJSON(text.decode(bytes.subarray(0, end)));
    return value && !Array.isArray(value) && value.schema === schema ? value : null;
  } catch {
    return null;
  }
}

export function parseSEI(packet, codec = 'avc', lengthSize = 0) {
  const records = [];
  const hevc = ['hevc', 'h265', 'hev1', 'hvc1'].includes(codec);
  for (const nal of packetNals(packet, lengthSize)) {
    const header = hevc ? 2 : 1;
    const kind = hevc ? (nal[0] >> 1) & 63 : nal[0] & 31;
    if (nal.length <= header || !(hevc ? [39, 40] : [6]).includes(kind)) continue;
    const rbsp = unescapeRBSP(nal.subarray(header));
    let at = 0;
    while (at < rbsp.length && !(rbsp[at] === 128 && rbsp.subarray(at + 1).every((v) => v === 0))) {
      const fields = [];
      for (let f = 0; f < 2; f++) {
        let n = 0;
        while (at < rbsp.length && rbsp[at] === 255) {
          n += 255;
          at++;
        }
        if (at >= rbsp.length) {
          at = rbsp.length;
          break;
        }
        fields.push(n + rbsp[at++]);
      }
      if (fields.length !== 2) break;
      const [type, size] = fields;
      if (at + size > rbsp.length) break;
      const payload = rbsp.subarray(at, at + size);
      at += size;
      if (type === 5 && equal(payload.subarray(0, 16), APCG_UUID)) {
        const value = jsonRecord(payload.subarray(16), 'apcg.telemetry.v1');
        if (value) records.push(value);
      }
    }
  }
  return records;
}

function ber(bytes, at, oid = false) {
  if (at >= bytes.length) throw new Error('Truncated BER');
  let value = bytes[at++];
  if (oid) {
    let result = value & 127;
    for (let count = 0; count < 5; count++) {
      if (!(value & 128)) return [result, at];
      if (at >= bytes.length || count === 4) break;
      value = bytes[at++];
      result = result * 128 + (value & 127);
    }
    throw new Error('Invalid BER-OID');
  }
  if (!(value & 128)) return [value, at];
  const count = value & 127;
  if (!count || count > 4 || at + count > bytes.length) throw new Error('Invalid BER length');
  value = 0;
  for (let i = 0; i < count; i++) value = value * 256 + bytes[at++];
  return [value, at];
}

function unsigned(bytes) {
  let n = 0;
  for (const b of bytes) n = n * 256 + b;
  return n;
}
function rotation(roll, pitch, yaw) {
  const cr = Math.cos(roll),
    sr = Math.sin(roll),
    cp = Math.cos(pitch),
    sp = Math.sin(pitch),
    cy = Math.cos(yaw),
    sy = Math.sin(yaw);
  return [
    [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
    [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
    [-sp, cp * sr, cp * cr],
  ];
}

function decodeLocalSet(packet, valueAt, preserveRawTags = false) {
  const n = packet.length;
  if (n < valueAt + 4 || packet[n - 4] !== 1 || packet[n - 3] !== 2)
    throw new Error('Missing ST0601 checksum');
  let checksum = 0;
  for (let i = 0; i < n - 2; i++) checksum = (checksum + packet[i] * (i % 2 ? 1 : 256)) & 65535;
  if (checksum !== unsigned(packet.subarray(n - 2))) throw new Error('Invalid ST0601 checksum');
  const items = new Map(),
    order = [];
  let at = valueAt;
  while (at < n) {
    let tag, size;
    [tag, at] = ber(packet, at, true);
    [size, at] = ber(packet, at);
    if (items.has(tag) || at + size > n) throw new Error('Invalid ST0601 local set');
    items.set(tag, packet.subarray(at, at + size));
    order.push(tag);
    at += size;
  }
  if (order[0] !== 2 || items.get(2)?.length !== 8 || items.get(65)?.length !== 1)
    throw new Error('Missing ST0601 timestamp/version');
  const u = (tag, size, scale, bias = 0) =>
    items.get(tag)?.length === size
      ? (unsigned(items.get(tag)) * scale) / (2 ** (8 * size) - 1) + bias
      : null;
  const s = (tag, size, scale) => {
    const bytes = items.get(tag);
    if (bytes?.length !== size) return null;
    let v = unsigned(bytes),
      max = 2 ** (8 * size - 1) - 1;
    if (v > max) v -= 2 ** (8 * size);
    return v === -max - 1 ? null : (v * scale) / max;
  };
  let utc = 0n;
  for (const byte of items.get(2)) utc = utc * 256n + BigInt(byte);
  const record = {
    schema: 'misb.st0601',
    utc_us: utc <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(utc) : utc.toString(),
    position: null,
    vehicle_attitude: null,
    gimbal_attitude: null,
    heading_rad: null,
    zoom: null,
    hfov_deg: u(16, 2, 180),
    misb_version: unsigned(items.get(65)),
  };
  const known = new Set([1, 2, 5, 6, 7, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 65, 90, 91]);
  const retained = [...items].filter(([tag]) => preserveRawTags || !known.has(tag));
  if (retained.length)
    record.raw_tags = Object.fromEntries(retained.map(([tag, bytes]) => [tag, bytes.slice()]));
  const lat = s(13, 4, 90),
    lon = s(14, 4, 180),
    alt = u(15, 2, 19900, -900);
  if ([lat, lon, alt].every((v) => v !== null))
    record.position = {
      lat_e7: Math.round(lat * 1e7),
      lon_e7: Math.round(lon * 1e7),
      alt_amsl_m: alt,
      age_ms: 0,
    };
  let yaw = u(5, 2, 360),
    pitch = items.has(90) ? s(90, 4, 90) : s(6, 2, 20),
    roll = items.has(91) ? s(91, 4, 90) : s(7, 2, 50);
  if ([roll, pitch, yaw].every((v) => v !== null)) {
    [roll, pitch, yaw] = [roll, pitch, yaw].map((v) => (v * Math.PI) / 180);
    record.vehicle_attitude = { roll_rad: roll, pitch_rad: pitch, yaw_rad: yaw, age_ms: 0 };
    record.heading_rad = yaw;
    const az = u(18, 4, 360),
      el = s(19, 4, 180),
      rr = u(20, 4, 360);
    if ([az, el, rr].every((v) => v !== null)) {
      const a = rotation(roll, pitch, yaw),
        b = rotation((rr * Math.PI) / 180, (el * Math.PI) / 180, (az * Math.PI) / 180);
      const earth = a.map((row) =>
        b[0].map((_, j) => row.reduce((sum, v, k) => sum + v * b[k][j], 0)),
      );
      const horizontal = Math.hypot(earth[0][0], earth[1][0]);
      const cameraYaw =
        horizontal > 1e-8
          ? Math.atan2(earth[1][0], earth[0][0])
          : Math.atan2(-earth[0][1], earth[1][1]);
      record.gimbal_attitude = {
        roll_rad: horizontal > 1e-8 ? Math.atan2(earth[2][1], earth[2][2]) : 0,
        pitch_rad: Math.atan2(-earth[2][0], horizontal),
        yaw_rad: wrap(cameraYaw - yaw),
        age_ms: 0,
      };
    }
  }
  return record;
}

/** Invalid sets are rejected individually; errors are available without fabricating pose. */
export function parseKLV(data, { errors = [], preserveRawTags = false } = {}) {
  const records = [];
  let at = 0;
  while (at + 17 <= data.length) {
    const start = at;
    try {
      let size, value;
      [size, value] = ber(data, start + 16);
      const end = value + size;
      if (end > data.length) {
        errors.push('Truncated KLV local set');
        break;
      }
      at = end;
      if (equal(data.subarray(start, start + 16), UAS_KEY))
        records.push(decodeLocalSet(data.subarray(start, end), value - start, preserveRawTags));
    } catch (error) {
      errors.push(error.message);
      if (at === start) break;
    }
  }
  return records;
}

/** Strip synchronous ST1402 access-unit cells; asynchronous packets are already KLV. */
export function parseMetadataPES(data, options) {
  if (equal(data.subarray(0, 16), UAS_KEY)) return parseKLV(data, options);
  const result = [];
  let at = 0;
  while (at + 5 <= data.length) {
    const flags = data[at + 2],
      size = data[at + 3] * 256 + data[at + 4];
    if (at + 5 + size > data.length || (flags & 0xc0) !== 0xc0) break;
    // Fragmented AU cells need reassembly; never pass a partial cell as a local set.
    if (!(flags & 0x20)) result.push(...parseKLV(data.subarray(at + 5, at + 5 + size), options));
    at += 5 + size;
  }
  return result;
}

export function parseThermalMetadata(bytes) {
  return jsonRecord(bytes, 'apcg.thermal.v1');
}

/** Keep APCG's established field names and validity, and retain source schema/record. */
export function normalizeTelemetry(record, provenance = {}) {
  if (!record || !['apcg.telemetry.v1', 'misb.st0601'].includes(record.schema)) return null;
  return {
    ...record,
    schema: 'apcg.telemetry.v1',
    sourceSchema: record.schema,
    provenance: { ...record.provenance, ...provenance },
  };
}
