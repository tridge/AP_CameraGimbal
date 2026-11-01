import test from 'node:test';
import assert from 'node:assert/strict';
import {
  parseFlightLog,
  resolveMissionAltitude,
  missionSegments,
  selectLogAt,
  homeAt,
} from '../src/domain/flight-log.js';

const concat = (arrays) => {
  const b = new Uint8Array(arrays.reduce((n, a) => n + a.length, 0));
  let o = 0;
  for (const a of arrays) {
    b.set(a, o);
    o += a.length;
  }
  return b;
};
const utf = new TextEncoder();
function putString(b, offset, value, size) {
  b.set(utf.encode(value).subarray(0, size), offset);
}
function fmt(type, name, format, columns) {
  const sizes = { Q: 8, B: 1, H: 2, f: 4, L: 4, Z: 64, e: 4, I: 4, c: 2 };
  const length = 3 + [...format].reduce((s, c) => s + sizes[c], 0);
  const b = new Uint8Array(89);
  b.set([0xa3, 0x95, 128, type, length]);
  putString(b, 5, name, 4);
  putString(b, 9, format, 16);
  putString(b, 25, columns, 64);
  return { bytes: b, type, length, format, columns: columns.split(',') };
}
function record(f, values) {
  const b = new Uint8Array(f.length);
  b.set([0xa3, 0x95, f.type]);
  const d = new DataView(b.buffer);
  let o = 3;
  for (let i = 0; i < f.format.length; i++) {
    const v = values[f.columns[i]] ?? 0;
    switch (f.format[i]) {
      case 'Q':
        d.setBigUint64(o, BigInt(v), true);
        o += 8;
        break;
      case 'B':
        d.setUint8(o, v);
        o++;
        break;
      case 'H':
        d.setUint16(o, v, true);
        o += 2;
        break;
      case 'I':
        d.setUint32(o, v, true);
        o += 4;
        break;
      case 'f':
        d.setFloat32(o, v, true);
        o += 4;
        break;
      case 'L':
        d.setInt32(o, Math.round(v * 1e7), true);
        o += 4;
        break;
      case 'e':
        d.setInt32(o, Math.round(v * 100), true);
        o += 4;
        break;
      case 'c':
        d.setInt16(o, Math.round(v * 100), true);
        o += 2;
        break;
      case 'Z':
        putString(b, o, v, 64);
        o += 64;
        break;
      default:
        throw new Error('fixture field');
    }
  }
  return b;
}
const CMD = fmt(
  129,
  'CMD',
  'QHHHffffLLfB',
  'TimeUS,CTot,CNum,CId,Prm1,Prm2,Prm3,Prm4,Lat,Lng,Alt,Frame',
);
const MSG = fmt(130, 'MSG', 'QZ', 'TimeUS,Message');
const FNCE = fmt(131, 'FNCE', 'QBBBLLBf', 'TimeUS,Tot,Seq,Type,Lat,Lng,Count,Radius');
const POS = fmt(132, 'POS', 'QLLfff', 'TimeUS,Lat,Lng,Alt,RelHomeAlt,RelOriginAlt');
const ORGN = fmt(133, 'ORGN', 'QBLLe', 'TimeUS,Type,Lat,Lng,Alt');
const GPS = fmt(134, 'GPS', 'QBBIHBLLe', 'TimeUS,I,Status,GMS,GWk,NSats,Lat,Lng,Alt');
function binBlob(records) {
  return new Blob([
    concat([CMD.bytes, MSG.bytes, FNCE.bytes, POS.bytes, ORGN.bytes, GPS.bytes, ...records]),
  ]);
}
const cmd = (seq, total, frame = 3, more = {}) =>
  record(CMD, {
    TimeUS: 1000000 + seq,
    CTot: total,
    CNum: seq,
    CId: 16,
    Lat: -35,
    Lng: 149,
    Alt: 50,
    Frame: frame,
    ...more,
  });
const newMission = (t) => record(MSG, { TimeUS: t, Message: 'New mission' });

test('BIN latest complete snapshots, no repeated-upload union; independent home datum', async () => {
  const b = binBlob([
    record(ORGN, { TimeUS: 1, Type: 0, Lat: -35, Lng: 149, Alt: 1000 }),
    record(ORGN, { TimeUS: 2, Type: 1, Lat: -35, Lng: 149, Alt: 700 }),
    newMission(10),
    cmd(0, 2, 3),
    cmd(1, 2, 10),
    newMission(2000000),
    cmd(0, 2, 0, { TimeUS: 2000001, Alt: 99 }),
    record(POS, { TimeUS: 3000000, Lat: -35, Lng: 149, Alt: 710, RelHomeAlt: 10 }),
  ]);
  const result = await parseFlightLog(b, { chunkSize: 512 });
  assert.equal(result.missionVersions.length, 2);
  assert.equal(result.missionVersions[1].complete, false);
  assert.equal(result.mission.complete, true);
  assert.deepEqual(
    result.mission.items.map((i) => i.frame),
    [3, 10],
  );
  assert.equal(result.home.altMsl, 700);
  assert.equal(result.track[0].altMsl, 710);
  assert.equal(resolveMissionAltitude(result.mission.items[0], { home: result.home }).altMsl, 750);
  assert.equal(resolveMissionAltitude(result.mission.items[1]).resolved, false);
  assert.equal(resolveMissionAltitude(result.mission.items[1], { terrainMsl: 800 }).altMsl, 850);
});

test('BIN equal-size neighboring polygons stay separate, internal exclusion types preserved', async () => {
  const records = [record(MSG, { TimeUS: 1, Message: 'New fence' })];
  for (let seq = 0; seq < 9; seq++)
    records.push(
      record(FNCE, {
        TimeUS: 10 + seq,
        Tot: 11,
        Seq: seq,
        Type: seq < 6 ? 98 : 97,
        Count: 3,
        Lat: -35 + seq * 0.001,
        Lng: 149 + (seq % 2) * 0.001,
      }),
    );
  records.push(
    record(FNCE, { TimeUS: 19, Tot: 11, Seq: 9, Type: 92, Lat: -35, Lng: 149, Radius: 50 }),
  );
  records.push(
    record(FNCE, { TimeUS: 20, Tot: 11, Seq: 10, Type: 93, Lat: -35, Lng: 149, Radius: 15 }),
  );
  const result = await parseFlightLog(binBlob(records));
  assert.equal(result.fenceVersions[0].complete, true);
  assert.deepEqual(
    result.fences.map((f) => [f.kind, f.exclusion]),
    [
      ['polygon', false],
      ['polygon', false],
      ['polygon', true],
      ['circle', false],
      ['circle', true],
    ],
  );
  assert.equal(result.fences[3].radius, 50);
});

const CRC = {
  2: 137,
  24: 24,
  33: 104,
  38: 9,
  39: 254,
  44: 221,
  45: 232,
  47: 153,
  73: 38,
  242: 104,
};
function mav(
  id,
  payload,
  { src = [255, 190], v2 = true, signed = false, utc = 1787886000000000, corrupt = false } = {},
) {
  const h = v2 ? 10 : 6,
    n = payload.length;
  const b = new Uint8Array(h + n + 2 + (signed ? 13 : 0));
  if (v2)
    b.set([0xfd, n, signed ? 1 : 0, 0, 0, ...src, id & 255, (id >> 8) & 255, (id >> 16) & 255]);
  else b.set([0xfe, n, 0, ...src, id]);
  b.set(payload, h);
  let crc = 65535;
  const add = (x) => {
    let t = x ^ (crc & 255);
    t ^= (t << 4) & 255;
    crc = ((crc >>> 8) ^ (t << 8) ^ (t << 3) ^ (t >>> 4)) & 65535;
  };
  for (let i = 1; i < h + n; i++) add(b[i]);
  add(CRC[id]);
  b[h + n] = crc & 255;
  b[h + n + 1] = crc >> 8;
  if (corrupt) b[h + n] ^= 1;
  const stamp = new Uint8Array(8);
  new DataView(stamp.buffer).setBigUint64(0, BigInt(utc), false);
  return concat([stamp, b]);
}
function count(n, type = 0, { target = [1, 0], ...opts } = {}) {
  const p = new Uint8Array(5);
  new DataView(p.buffer).setUint16(0, n, true);
  p.set([...target, type], 2);
  return mav(44, p, opts);
}
function item(
  seq,
  {
    frame = 3,
    type = 0,
    command = 16,
    int = true,
    x = -35,
    y = 149,
    z = 50,
    param1 = 0,
    target = [1, 1],
    ...opts
  } = {},
) {
  const p = new Uint8Array(38);
  const d = new DataView(p.buffer);
  d.setFloat32(0, param1, true);
  if (int) {
    d.setInt32(16, Math.round(x * 1e7), true);
    d.setInt32(20, Math.round(y * 1e7), true);
  } else {
    d.setFloat32(16, x, true);
    d.setFloat32(20, y, true);
  }
  d.setFloat32(24, z, true);
  d.setUint16(28, seq, true);
  d.setUint16(30, command, true);
  p.set([...target, frame, 0, 1, type], 32);
  return mav(int ? 73 : 39, p, opts);
}
function ack(type = 0, result = 0, opts = {}) {
  return mav(47, new Uint8Array([255, 190, result, type]), { src: [1, 1], ...opts });
}
function position(boot = 1000) {
  const p = new Uint8Array(28),
    d = new DataView(p.buffer);
  d.setUint32(0, boot, true);
  d.setInt32(4, -350000000, true);
  d.setInt32(8, 1490000000, true);
  d.setInt32(12, 710000, true);
  d.setInt32(16, 10000, true);
  return mav(33, p, { src: [1, 1] });
}
const tlog = (packets) => new Blob([concat(packets)]);

test('accepted ALL-type clear removes both plans only after ACK; rejected and ambiguous clears preserve plans', async () => {
  const utc = 1787886000000000;
  for (const outcome of ['accepted', 'rejected', 'superseded']) {
    const packets = [
      position(),
      count(1),
      item(0),
      ack(),
      count(1, 1),
      item(0, { type: 1, command: 5004, param1: 35 }),
      ack(1),
      mav(45, Uint8Array.of(1, 0, 255), { utc: utc + 1e6 }),
    ];
    if (outcome === 'superseded') packets.push(count(1, 0, { utc: utc + 1.5e6 }));
    packets.push(ack(255, outcome === 'rejected' ? 1 : 0, { utc: utc + 2e6 }));
    if (outcome === 'accepted') packets.push(ack(255, 0, { utc: utc + 3e6 })); // duplicate must not move completion time
    const log = await parseFlightLog(tlog(packets));
    const before = selectLogAt(log, utc + 1.5e6);
    assert.equal(before.mission.items.length, 1);
    assert.equal(before.fences.length, 1);
    const after = selectLogAt(log, utc + 2.5e6);
    assert.equal(after.mission.items.length, outcome === 'accepted' ? 0 : 1);
    assert.equal(after.fences.length, outcome === 'accepted' ? 0 : 1);
    assert.equal(log.mission.items.length, outcome === 'accepted' ? 0 : 1);
  }
});

test('tlog float/int frames, component-zero endpoints, out-of-order items and accepted upload', async () => {
  const result = await parseFlightLog(
    tlog([
      count(6),
      item(2, { frame: 6 }),
      item(0, { frame: 0, int: false }),
      item(1, { frame: 3, int: false }),
      item(3, { frame: 10 }),
      item(4, { frame: 11 }),
      item(5, { frame: 5 }),
      ack(),
      position(),
    ]),
    { chunkSize: 512 },
  );
  assert.equal(result.mission.complete, true);
  assert.equal(result.mission.confirmed, true);
  assert.deepEqual(
    result.mission.items.map((i) => i.position.altitude.reference),
    ['amsl', 'home', 'home', 'terrain', 'terrain', 'amsl'],
  );
  assert.deepEqual(
    result.mission.items.map((i) => i.position.lat),
    Array(6).fill(-35),
  );
  assert.equal(result.track.length, 1);
  assert.equal(result.track[0].utcUs, 1787886000000000);
});

test('tlog incomplete/rejected uploads do not replace complete plan, mission/fence isolated', async () => {
  const result = await parseFlightLog(
    tlog([
      count(1),
      item(0, { z: 11 }),
      ack(),
      count(2),
      item(0, { z: 22 }),
      count(1, 1),
      item(0, { type: 1, command: 5004, param1: 35 }),
      ack(1),
      count(1),
      item(0, { z: 33 }),
      ack(0, 1),
    ]),
  );
  assert.equal(result.mission.items[0].position.altitude.metres, 11);
  assert.equal(result.missionVersions.length, 3);
  assert.equal(result.missionVersions[2].rejected, true);
  assert.equal(result.fences[0].exclusion, true);
  assert.equal(result.fences[0].radius, 35);
});

test('tlog accepted empty plan, CRC failure, signed trailers and bounded reads', async () => {
  const packets = [
    count(1),
    item(0, { signed: true }),
    ack(),
    ...Array.from({ length: 25 }, () => position()),
    count(0),
    ack(),
    item(0, { corrupt: true }),
  ];
  const blob = tlog(packets);
  let largest = 0;
  let whole = false;
  const file = {
    size: blob.size,
    name: 'test.tlog',
    arrayBuffer() {
      whole = true;
      throw new Error('whole read');
    },
    slice(a, b) {
      largest = Math.max(largest, b - a);
      return blob.slice(a, b);
    },
  };
  const result = await parseFlightLog(file, { chunkSize: 512, maxTrackPoints: 4 });
  assert.equal(result.mission.complete, true);
  assert.equal(result.mission.items.length, 0);
  assert.equal(result.diagnosticCounts.mavlink_crc_or_length_error, 1);
  assert.equal(result.diagnosticCounts.mavlink_signature_not_authenticated, 1);
  assert.equal(whole, false);
  assert.ok(largest <= 512);
  assert.ok(result.track.length <= 5);
});

test('partial mission route does not span missing seq or treat DO commands/0,0 as waypoints', async () => {
  const result = await parseFlightLog(
    tlog([count(5), item(0), item(2), item(3, { command: 177 }), item(4, { x: 0, y: 0 })]),
  );
  assert.equal(result.mission.complete, false);
  assert.equal(result.mission.items[2].spatial, false);
  assert.equal(result.mission.items[3].spatial, false);
  assert.deepEqual(
    missionSegments(result.mission.items).map((s) => s.map((i) => i.seq)),
    [[0], [2]],
  );
});

test('BIN GPS anchors keep real AMSL, valid equator coordinates and cancellation', async () => {
  const blob = binBlob([
    record(GPS, {
      TimeUS: 1000000,
      I: 0,
      Status: 3,
      GMS: 100000,
      GWk: 2433,
      NSats: 10,
      Lat: 0,
      Lng: 20,
      Alt: 801.25,
    }),
    record(POS, { TimeUS: 2000000, Lat: 0, Lng: 20, Alt: 802.25 }),
  ]);
  const result = await parseFlightLog(blob);
  assert.equal(result.track[0].lat, 0);
  assert.equal(result.track[0].altMsl, 802.25);
  assert.ok(result.track[0].utcUs > 1700000000000000);
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(() => parseFlightLog(blob, { signal: controller.signal }), {
    name: 'AbortError',
  });
});

test('interleaved vehicle uploads and conflicting retries cannot manufacture a complete plan', async () => {
  const result = await parseFlightLog(
    tlog([
      count(1),
      item(0, { z: 12 }),
      ack(),
      count(2, 0, { target: [2, 0] }),
      item(0, { target: [2, 1], z: 22 }),
      count(2),
      item(0, { z: 33 }),
      item(0, { z: 44 }),
      item(1),
      ack(),
      position(),
    ]),
  );
  assert.equal(result.mission.items[0].position.altitude.metres, 12);
  assert.equal(result.missionVersions.length, 3);
  assert.equal(result.missionVersions[1].items.length, 1);
  assert.equal(result.missionVersions[2].complete, false);
  assert.ok(result.missionVersions[2].diagnostics.some((d) => d.includes('conflicting duplicate')));
});

test('tlog receipt UTC stays distinct from derived autopilot UTC and short clock reorder', async () => {
  const p = new Uint8Array(12),
    d = new DataView(p.buffer);
  d.setBigUint64(0, 1787886009000000n, true);
  d.setUint32(8, 10000, true);
  const result = await parseFlightLog(
    tlog([mav(2, p, { src: [1, 1] }), position(12000), position(10500)]),
  );
  assert.equal(result.track[0].utcUs, 1787886000000000);
  assert.equal(result.track[0].bootUtcUs, 1787886011000000);
  assert.equal(result.diagnosticCounts.boot_clock_reset, undefined);
  assert.equal(result.diagnosticCounts.boot_time_reordered, 1);
});

test('frame-time selection uses completed plan and applicable arming home, never a future upload', async () => {
  const utc = 1787886000000000;
  const home = (alt, at, src = [1, 1]) => {
    const p = new Uint8Array(52),
      d = new DataView(p.buffer);
    d.setInt32(0, -350000000, true);
    d.setInt32(4, 1490000000, true);
    d.setInt32(8, alt * 1000, true);
    return mav(242, p, { src, utc: at });
  };
  const log = await parseFlightLog(
    tlog([
      home(700, utc - 5e6),
      count(2, 0, { utc }),
      item(0, { utc: utc + 1e6 }),
      item(1, { utc: utc + 2e6 }),
      ack(0, 0, { utc: utc + 3e6 }),
      home(800, utc + 20e6),
      count(1, 0, { utc: utc + 30e6 }),
      item(0, { z: 80, utc: utc + 31e6 }),
      ack(0, 1, { utc: utc + 35e6 }),
      count(1, 1, { utc: utc + 40e6 }),
      item(0, { type: 1, command: 5004, param1: 35, utc: utc + 41e6 }),
      ack(1, 0, { utc: utc + 42e6 }),
      home(999, utc + 10e6, [2, 1]),
      position(),
    ]),
  );
  assert.equal(selectLogAt(log), log);
  assert.equal(selectLogAt(log, utc + 1e6).mission.items.length, 0);
  const beforeAck = selectLogAt(log, utc + 2.5e6);
  assert.equal(beforeAck.mission.complete, true);
  assert.equal(beforeAck.mission.confirmed, false);
  assert.equal(selectLogAt(log, utc + 4e6).mission.confirmed, true);
  assert.equal(homeAt(log, utc + 4e6).altMsl, 700);
  assert.equal(homeAt(log, utc + 25e6).altMsl, 800);
  assert.equal(
    resolveMissionAltitude(selectLogAt(log, utc + 4e6).mission.items[0], {
      home: homeAt(log, utc + 4e6),
    }).altMsl,
    750,
  );
  assert.equal(
    resolveMissionAltitude(selectLogAt(log, utc + 25e6).mission.items[0], {
      home: homeAt(log, utc + 25e6),
    }).altMsl,
    850,
  );
  assert.equal(selectLogAt(log, utc + 32e6).mission.items[0].position.altitude.metres, 80);
  assert.equal(selectLogAt(log, utc + 36e6).mission.items[0].position.altitude.metres, 50);
  assert.equal(selectLogAt(log, utc + 39e6).fences.length, 0);
  assert.equal(selectLogAt(log, utc + 43e6).fences.length, 1);
  log.homes.push({
    lat: -35,
    lon: 149,
    altMsl: 1500,
    sourceRef: { vehicleId: '1:1', utcUs: null },
  });
  log.missionVersions.push({
    ...log.missionVersions[0],
    id: 'unknown-clock',
    completedAt: { utcUs: null },
    endedAt: { utcUs: null },
  });
  const uncertain = selectLogAt(log, utc + 4e6);
  assert.equal(uncertain.home.altMsl, 700);
  assert.notEqual(uncertain.mission.id, 'unknown-clock');
  assert.ok(uncertain.selection.uncertain.includes('home missing UTC'));
});

test('bad supported/unknown/truncated MAVLink headers rescan without swallowing following valid packet', async () => {
  const valid = position();
  const fake = (id) => {
    const b = new Uint8Array(8 + 10 + 8);
    new DataView(b.buffer).setBigUint64(0, 1787886000000000n, false);
    b.set([0xfd, 120, 0, 0, 0, 1, 1, id, 0, 0], 8);
    return b;
  };
  for (const id of [33, 199]) {
    const data = tlog([count(0), ack(), fake(id), valid, new Uint8Array(200)]);
    const result = await parseFlightLog(data, { chunkSize: 512 });
    assert.equal(result.track.length, 1, `recover packet nested in forged header ${id}`);
    assert.ok(result.diagnosticCounts.mavlink_resynchronized_bytes);
  }
  const truncated = await parseFlightLog(tlog([count(0), ack(), fake(33), valid]));
  assert.equal(truncated.track.length, 1);
  assert.ok(truncated.diagnosticCounts.truncated_mavlink_packet);
  const unknownAtEof = await parseFlightLog(
    tlog([count(0), ack(), fake(199), valid, new Uint8Array(140 - 26 - valid.length)]),
  );
  assert.equal(unknownAtEof.track.length, 1);
});
