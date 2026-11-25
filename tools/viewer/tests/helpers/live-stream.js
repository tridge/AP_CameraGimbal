// A small APCG live thermal stream: prefix plus one Cluster per frame, as the camera sends it.
const encode = (s) => new TextEncoder().encode(s);
export const join = (...parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};
const hex = (h) => Uint8Array.from(h.match(/../g), (v) => parseInt(v, 16));
function vint(n) {
  let length = 1;
  while (n >= 2 ** (7 * length) - 1) length++;
  const out = new Uint8Array(length);
  for (let i = length - 1; i >= 0; i--) {
    out[i] = n % 256;
    n = Math.floor(n / 256);
  }
  out[0] |= 0x80 >> (length - 1);
  return out;
}
function uint(n) {
  const out = [];
  do {
    out.unshift(n % 256);
    n = Math.floor(n / 256);
  } while (n);
  return Uint8Array.from(out);
}
export const eb = (id, data) => join(hex(id), vint(data.length), data);
const eu = (id, n) => eb(id, uint(n));

export function liveHead({
  codecPrivate = Uint8Array.of(7, 8, 9),
  width = 640,
  height = 512,
} = {}) {
  const track = eb(
    'ae',
    join(
      eu('d7', 1),
      eu('83', 1),
      eb('86', encode('V_FFV1')),
      eb('63a2', codecPrivate),
      eu('23e383', 200000000),
      eb('e0', join(eu('b0', width), eu('ba', height))),
      eb('41e4', join(eu('41f0', 2), eu('41e7', 0x41504347))),
    ),
  );
  return {
    ebml: eb('1a45dfa3', eb('4282', encode('matroska'))),
    segment: hex('1853806701ffffffffffffff'),
    // a float Duration, as a converted recording carries
    info: eb('1549a966', join(eu('2ad7b1', 1000000), eb('4489', hex('4110d83400000000')))),
    tracks: eb('1654ae6b', track),
  };
}
export const headBytes = (head = liveHead()) =>
  join(head.ebml, head.segment, head.info, head.tracks);

/** payload defaults to bytes derived from the frame number. */
export function liveCluster(
  ticks,
  n = 0,
  {
    payload = null,
    latE7 = -353219444 + n * 2000,
    lonE7 = 1488380556,
    utcUs = 1787882912000000 + ticks * 1000,
  } = {},
) {
  const meta = {
    schema: 'apcg.thermal.v1',
    frame_id: n,
    width: 640,
    height: 512,
    pixel_format: 'gray16le',
    bits_per_sample: 16,
    rotation_deg: 0,
    hfov_deg: 24.2,
    temperature_calibration: 'sensor_kelvin_64',
    temperature_scale_k: 1 / 64,
    temperature_offset_k: 0,
    minimum_c: 1,
    maximum_c: 20 + (n % 10),
    telemetry: {
      schema: 'apcg.telemetry.v1',
      utc_us: utcUs,
      position: {
        lat_e7: latE7,
        lon_e7: lonE7,
        alt_amsl_m: 1400,
        alt_relative_m: 600,
        age_ms: 0,
      },
      vehicle_attitude: { roll_rad: 0, pitch_rad: 0, yaw_rad: 0, age_ms: 0 },
      gimbal_attitude: { roll_rad: 0, pitch_rad: -Math.PI / 2, yaw_rad: 0, age_ms: 0 },
      hfov_deg: 60,
      heading_rad: 0,
    },
  };
  const block = eb(
    'a1',
    join(Uint8Array.of(0x81, 0, 0, 0), payload ?? Uint8Array.of(n & 255, (n >> 8) & 255, 33)),
  );
  const group = eb(
    'a0',
    join(block, eb('75a1', eb('a6', join(eu('ee', 2), eb('a5', encode(JSON.stringify(meta))))))),
  );
  return eb('1f43b675', join(eu('e7', ticks), group));
}
