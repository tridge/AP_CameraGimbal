/** Independent, bounded local ArduPilot BIN / timestamped MAVLink reader.
 * Wire facts: MAVLink common/ardupilotmega definitions and ArduPilot LogStructure.
 * No UAVLogViewer source is incorporated. All times are original source clocks.
 */
const GLOBAL_FRAMES = new Set([0, 3, 5, 6, 10, 11]);
const NAV_LOCATION_COMMANDS = new Set([16, 17, 18, 19, 21, 22, 31, 82, 84, 85, 94]);
const text = new TextDecoder();
const finite = Number.isFinite;
const coordinate = (lat, lon) =>
  finite(lat) && finite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
const altitudeReference = (frame) =>
  [0, 5].includes(frame)
    ? 'amsl'
    : [3, 6].includes(frame)
      ? 'home'
      : [10, 11].includes(frame)
        ? 'terrain'
        : 'unsupported';
const string = (bytes) => text.decode(bytes).replace(/\0.*$/s, '');
const abort = (signal) => {
  if (signal?.aborted) throw new DOMException('Log parsing cancelled', 'AbortError');
};

/** Preserve altitude datum. Resolve only with explicitly supplied dependencies. */
export function resolveMissionAltitude(item, { home = null, terrainMsl = null } = {}) {
  const a = item.position?.altitude;
  if (!a || !finite(a.metres)) return { resolved: false, altMsl: null, reason: 'missing altitude' };
  if (a.reference === 'amsl') return { resolved: true, altMsl: a.metres, reference: a.reference };
  if (a.reference === 'home' && finite(home?.altMsl))
    return { resolved: true, altMsl: home.altMsl + a.metres, reference: a.reference };
  if (a.reference === 'terrain' && finite(terrainMsl))
    return { resolved: true, altMsl: terrainMsl + a.metres, reference: a.reference };
  return { resolved: false, altMsl: null, reason: `unresolved ${a.reference} altitude` };
}

function missionItem(raw, ref, intCoordinates = false) {
  const frame = raw.frame;
  const global = GLOBAL_FRAMES.has(frame);
  const lat = global ? raw.x / (intCoordinates ? 1e7 : 1) : null;
  const lon = global ? raw.y / (intCoordinates ? 1e7 : 1) : null;
  const hasPosition = global && coordinate(lat, lon);
  // These navigation commands allow 0,0 as current-location sentinel. Actions
  // and jump commands retain their parameters without becoming route vertices.
  const currentLocation = hasPosition && lat === 0 && lon === 0;
  return {
    seq: raw.seq,
    command: raw.command,
    frame,
    originalFrame: frame,
    params: [raw.param1, raw.param2, raw.param3, raw.param4],
    position: hasPosition
      ? {
          lat,
          lon,
          altitude: {
            metres: finite(raw.z) ? raw.z : null,
            reference: altitudeReference(frame),
            originalFrame: frame,
          },
        }
      : null,
    spatial: hasPosition && NAV_LOCATION_COMMANDS.has(raw.command) && !currentLocation,
    currentLocation,
    current: raw.current ?? null,
    autocontinue: raw.autocontinue ?? null,
    sourceRef: ref,
    raw,
  };
}

/** Route segments never span missing sequence numbers, including partial logs. */
export function missionSegments(items) {
  const result = [];
  let segment = [];
  let previousSeq = null;
  for (const item of [...items].sort((a, b) => a.seq - b.seq)) {
    if (previousSeq !== null && item.seq !== previousSeq + 1) {
      if (segment.length) result.push(segment);
      segment = [];
    }
    previousSeq = item.seq;
    if (item.spatial) segment.push(item);
  }
  if (segment.length) result.push(segment);
  return result;
}

function logVehicle(log) {
  const explicit = log.primaryVehicleId ?? log.track?.[0]?.vehicleId ?? log.tracks?.[0]?.vehicleId;
  if (explicit) return explicit;
  if (log.source?.format === 'dataflash-bin') return 'dataflash';
  const homeVehicles = [
    ...new Set((log.homes || []).map((h) => h.sourceRef?.vehicleId).filter(Boolean)),
  ];
  return homeVehicles.length === 1 ? homeVehicles[0] : null;
}
function belongsToVehicle(version, vehicle) {
  return (
    !vehicle ||
    version.startedAt?.vehicleId === vehicle ||
    (version.sender && endpointMatches(version.sender, vehicle)) ||
    (version.receiver && endpointMatches(version.receiver, vehicle))
  );
}
function sourceUtc(value) {
  return finite(value?.utcUs) ? value.utcUs : null;
}

/** Home observed at the requested source UTC, never a later arming's home. */
export function homeAt(log, utcUs, { vehicleId = logVehicle(log) } = {}) {
  if (finite(utcUs) && !vehicleId) return null;
  const homes = (log.homes || (log.home ? [log.home] : [])).filter(
    (h) => !vehicleId || h.sourceRef?.vehicleId === vehicleId,
  );
  if (!finite(utcUs)) return homes.at(-1) || null;
  return (
    homes
      .filter((h) => sourceUtc(h.sourceRef) !== null && h.sourceRef.utcUs <= utcUs)
      .sort(
        (a, b) =>
          a.sourceRef.utcUs - b.sourceRef.utcUs ||
          (a.sourceRef.byteOffset ?? 0) - (b.sourceRef.byteOffset ?? 0),
      )
      .at(-1) || null
  );
}

/** Time-specific map view; original histories and clocks remain in the log.
 * Missing UTC is unavailable evidence, never permission to use a future plan.
 */
export function selectLogAt(log, utcUs, { vehicleId = logVehicle(log) } = {}) {
  if (!finite(utcUs)) return log; // Explicit latest-log overview, same as parse.
  const diagnostics = [...(log.diagnostics || [])];
  const uncertain = [];
  if (!vehicleId) {
    const detail = ['vehicle selection unavailable; specify vehicleId'];
    return {
      ...log,
      mission: { id: null, items: [], segments: [], complete: false, confirmed: false },
      fences: [],
      selectedFenceVersion: null,
      home: null,
      diagnostics: [...diagnostics, { code: 'log_time_selection_uncertain', detail }],
      selection: { mode: 'at-time', utcUs, vehicleId: null, uncertain: detail },
    };
  }
  const note = (detail) => {
    if (!uncertain.includes(detail)) uncertain.push(detail);
  };
  const select = (versions, kind) => {
    const available = [];
    for (const version of versions || []) {
      if (!belongsToVehicle(version, vehicleId)) continue;
      const completeAt = version.completedAt || version.endedAt;
      let time = sourceUtc(completeAt);
      const startTime = sourceUtc(version.startedAt);
      if (
        version.invalidatesPrevious &&
        startTime !== null &&
        startTime <= utcUs &&
        (time === null || time > utcUs)
      )
        time = startTime;
      if (time === null) {
        note(`${kind} version missing UTC`);
        continue;
      }
      if (time > utcUs) continue;
      if (version.rejected) {
        const rejectedTime = sourceUtc(version.rejectedAt);
        if (rejectedTime === null) {
          note(`${kind} rejection missing UTC`);
          continue;
        }
        if (rejectedTime <= utcUs) continue;
      }
      const items = version.items.filter((item) => {
        const itemTime = sourceUtc(item.sourceRef);
        if (itemTime === null) {
          note(`${kind} item missing UTC`);
          return false;
        }
        return itemTime <= utcUs;
      });
      const structurallyComplete =
        version.expectedCount !== null &&
        version.expectedCount === items.length &&
        !version.conflicting &&
        !version.partial;
      const complete =
        (version.complete || !!version.completedAt) &&
        structurallyComplete &&
        sourceUtc(completeAt) !== null &&
        sourceUtc(completeAt) <= utcUs;
      const confirmationTime = sourceUtc(
        version.confirmedAt || (version.confirmed ? version.endedAt : null),
      );
      const confirmed =
        !!version.confirmed && confirmationTime !== null && confirmationTime <= utcUs;
      if (version.confirmed && confirmationTime === null) note(`${kind} confirmation missing UTC`);
      available.push({ ...version, items, complete, confirmed, selectionTimeUtcUs: time });
    }
    available.sort(
      (a, b) =>
        a.selectionTimeUtcUs - b.selectionTimeUtcUs ||
        (a.endedAt?.byteOffset ?? 0) - (b.endedAt?.byteOffset ?? 0),
    );
    const barrier = available.filter((v) => v.invalidatesPrevious).at(-1);
    const candidates = barrier
      ? available.filter((v) => v.selectionTimeUtcUs >= barrier.selectionTimeUtcUs)
      : available;
    if (barrier && !barrier.complete)
      note(`${kind} snapshot changed; replacement is incomplete or empty`);
    return (
      candidates.filter((v) => v.complete).at(-1) ||
      candidates.filter((v) => v.items.length).at(-1) ||
      barrier ||
      null
    );
  };
  const selectedMission = select(log.missionVersions, 'mission');
  const selectedFenceVersion = select(log.fenceVersions, 'fence');
  const homes = (log.homes || []).filter((h) => !vehicleId || h.sourceRef?.vehicleId === vehicleId);
  if (homes.some((h) => sourceUtc(h.sourceRef) === null)) note('home missing UTC');
  if (uncertain.length)
    diagnostics.push({ code: 'log_time_selection_uncertain', detail: uncertain });
  const mission = selectedMission
    ? { ...selectedMission, segments: missionSegments(selectedMission.items) }
    : { id: null, items: [], segments: [], complete: false, confirmed: false };
  return {
    ...log,
    mission,
    selectedFenceVersion,
    fences: selectedFenceVersion
      ? fenceShapes(
          selectedFenceVersion,
          (code) => diagnostics.push({ code }),
          homeAt(log, utcUs, { vehicleId }),
        )
      : [],
    home: homeAt(log, utcUs, { vehicleId }),
    diagnostics,
    selection: { mode: 'at-time', utcUs, vehicleId, uncertain },
  };
}

class ChunkReader {
  constructor(file, { chunkSize, signal, onProgress }) {
    this.file = file;
    this.chunkSize = chunkSize;
    this.signal = signal;
    this.onProgress = onProgress;
    this.bytes = new Uint8Array();
    this.pos = 0;
    this.loaded = 0;
    this.offset = 0;
  }
  async ensure(n) {
    abort(this.signal);
    if (this.bytes.length - this.pos >= n) return true;
    if (this.loaded >= this.file.size) return false;
    const next = new Uint8Array(
      await this.file
        .slice(this.loaded, Math.min(this.loaded + this.chunkSize, this.file.size))
        .arrayBuffer(),
    );
    abort(this.signal);
    const rest = this.bytes.subarray(this.pos);
    const joined = new Uint8Array(rest.length + next.length);
    joined.set(rest);
    joined.set(next, rest.length);
    this.bytes = joined;
    this.pos = 0;
    this.loaded += next.length;
    this.onProgress?.({ bytesRead: this.loaded, totalBytes: this.file.size, phase: 'flight-log' });
    return this.bytes.length >= n || this.ensure(n);
  }
  peek(i = 0) {
    return this.bytes[this.pos + i];
  }
  view(n) {
    return this.bytes.subarray(this.pos, this.pos + n);
  }
  take(n) {
    const b = this.bytes.subarray(this.pos, this.pos + n);
    this.pos += n;
    this.offset += n;
    return b;
  }
}

class Versions {
  constructor(kind, diagnostic, limit) {
    this.kind = kind;
    this.list = [];
    this.active = new Map();
    this.diagnostic = diagnostic;
    this.limit = limit;
  }
  start(key, expectedCount, ref, evidence, extra = {}) {
    const v = {
      id: `${this.kind}-${ref.byteOffset}`,
      kind: this.kind,
      key,
      expectedCount,
      items: [],
      startedAt: ref,
      endedAt: ref,
      evidence,
      complete: false,
      confirmed: false,
      rejected: false,
      diagnostics: [],
      ...extra,
      slots: new Map(),
    };
    if (
      expectedCount !== null &&
      (!Number.isInteger(expectedCount) || expectedCount < 0 || expectedCount > 10000)
    ) {
      v.expectedCount = null;
      v.diagnostics.push('invalid item count');
    }
    this.active.set(key, v);
    this.list.push(v);
    if (this.list.length > this.limit) {
      const old = this.list.shift();
      if (this.active.get(old.key) === old) this.active.delete(old.key);
      this.diagnostic('version_history_limited');
    }
    return v;
  }
  add(key, item, expectedCount, ref, evidence) {
    let v = this.active.get(key);
    if (
      !v ||
      (expectedCount !== null && v.expectedCount !== null && expectedCount !== v.expectedCount) ||
      (item.seq === 0 &&
        v.complete &&
        JSON.stringify(v.slots.get(0)?.raw) !== JSON.stringify(item.raw))
    )
      v = this.start(key, expectedCount, ref, evidence);
    if (
      !Number.isInteger(item.seq) ||
      item.seq < 0 ||
      item.seq >= 10000 ||
      (v.expectedCount !== null && item.seq >= v.expectedCount)
    ) {
      v.diagnostics.push('item sequence out of range');
      return;
    }
    const previous = v.slots.get(item.seq);
    if (previous && JSON.stringify(previous.raw) !== JSON.stringify(item.raw)) {
      v.diagnostics.push(`conflicting duplicate sequence ${item.seq}`);
      v.conflicting = true;
    } else if (!previous) v.slots.set(item.seq, item);
    if (!(previous && v.confirmed)) v.endedAt = ref;
    v.complete =
      v.expectedCount !== null &&
      v.expectedCount > 0 &&
      v.slots.size === v.expectedCount &&
      !v.conflicting &&
      !v.partial;
    if (v.complete && !v.completedAt) v.completedAt = ref;
    if (v.complete && v.evidence === 'dataflash-dump') {
      v.confirmed = true;
      if (!v.confirmedAt) v.confirmedAt = ref;
    }
  }
  finish() {
    for (const v of this.list) {
      v.items = [...v.slots.values()].sort((a, b) => a.seq - b.seq);
      delete v.slots;
      if (!v.complete)
        v.diagnostics.push('incomplete snapshot; items are not merged with other versions');
      if (v.rejected) v.complete = false;
    }
    return this.list;
  }
}

function fenceShapes(version, diagnostic, home = null) {
  const shapes = [];
  const items = version.items;
  for (let i = 0; i < items.length; ) {
    const item = items[i];
    const r = item.raw;
    const bin = r.fenceType !== undefined;
    const type = bin ? r.fenceType : item.command;
    const polygon = bin ? [98, 97].includes(type) : [5001, 5002].includes(type);
    // ArduPilot AC_Fence/AC_PolyFence_loader.h: AC_PolyFenceType::HOME_CIRCLE_INCLUSION = 91.
    // https://github.com/ArduPilot/ardupilot/blob/master/libraries/AC_Fence/AC_PolyFence_loader.h
    const homeCircle = bin ? type === 91 : type === 5005;
    const circle =
      homeCircle || (bin ? [92, 93, 94, 96].includes(type) : [5003, 5004].includes(type));
    const exclusion = bin ? [97, 93, 96].includes(type) : [5002, 5004].includes(type);
    const point = homeCircle
      ? home && coordinate(home.lat, home.lon) && { lat: home.lat, lon: home.lon }
      : item.position && { lat: item.position.lat, lon: item.position.lon };
    if (polygon) {
      const count = bin ? r.vertexCount : r.param1;
      const group = items.slice(i, i + count);
      const valid =
        Number.isInteger(count) &&
        count >= 3 &&
        count <= 10000 &&
        group.length === count &&
        group.every(
          (p, j) =>
            p.seq === item.seq + j &&
            (bin
              ? p.raw.fenceType === type && p.raw.vertexCount === count
              : p.command === type && p.raw.param1 === count) &&
            p.position,
        );
      if (!valid) {
        diagnostic('invalid_fence_polygon');
        i++;
        continue;
      }
      shapes.push({
        kind: 'polygon',
        exclusion,
        rule: exclusion ? 'exclusion' : 'inclusion',
        points: group.map((p) => ({ lat: p.position.lat, lon: p.position.lon })),
        sourceItemSeqs: group.map((p) => p.seq),
        sourceVersionId: version.id,
      });
      i += count;
    } else if (circle) {
      const radius = bin ? r.radius : r.param1;
      if (point && finite(radius) && radius > 0)
        shapes.push({
          kind: 'circle',
          exclusion,
          rule: exclusion ? 'exclusion' : 'inclusion',
          points: [point],
          center: point,
          radius,
          sourceItemSeqs: [item.seq],
          sourceVersionId: version.id,
        });
      else
        diagnostic(
          homeCircle && !point ? 'home_fence_position_unavailable' : 'invalid_fence_circle',
        );
      i++;
    } else {
      if (![95, 5000].includes(type)) diagnostic('unsupported_fence_type');
      i++;
    }
  }
  return shapes;
}

class LogState {
  constructor(file, options) {
    this.options = options;
    this.counts = {};
    this.diagnostics = [];
    this.tracks = new Map();
    this.homes = [];
    this.clockSegments = new Map();
    this.bootLast = new Map();
    this.bootSegment = new Map();
    this.firstGpsFix = new Map();
    this.source = {
      name: file.name || '(local blob)',
      size: file.size,
      lastModified: file.lastModified ?? null,
      format: null,
    };
    this.missions = new Versions('mission', (code) => this.diag(code), options.maxVersions);
    this.fenceVersions = new Versions('fence', (code) => this.diag(code), options.maxVersions);
  }
  diag(code, detail) {
    this.counts[code] = (this.counts[code] || 0) + 1;
    if (this.diagnostics.length < 100 && !this.diagnostics.some((d) => d.code === code))
      this.diagnostics.push({ code, detail: detail ?? null });
  }
  ref(message, byteOffset, vehicleId = 'dataflash', bootUs = null, receiptUtcUs = null) {
    let segment = this.bootSegment.get(vehicleId) || 0;
    // SYSTEM_TIME and position messages can legitimately describe different
    // instants several seconds apart. Reset detection compares like messages.
    const clockKey = `${vehicleId}/${message}`;
    const last = this.bootLast.get(clockKey);
    if (finite(bootUs)) {
      if (
        last &&
        last.segment === segment &&
        bootUs < last.bootUs - this.options.bootResetThresholdUs
      ) {
        segment++;
        this.bootSegment.set(vehicleId, segment);
        this.diag('boot_clock_reset', {
          vehicleId,
          message,
          previousBootUs: last.bootUs,
          bootUs,
          byteOffset,
        });
      } else if (last && bootUs < last.bootUs - 1000000)
        this.diag('boot_time_reordered', {
          vehicleId,
          message,
          differenceUs: bootUs - last.bootUs,
        });
      this.bootLast.set(clockKey, { bootUs, segment });
    }
    const clockSegment = `${vehicleId}:${segment}`;
    if (!this.clockSegments.has(clockSegment)) {
      if (this.clockSegments.size >= 4096)
        throw new Error('Flight log exceeds the supported clock-segment limit');
      this.clockSegments.set(clockSegment, { id: clockSegment, vehicleId, anchors: [] });
    }
    return {
      message,
      byteOffset,
      vehicleId,
      bootUs: finite(bootUs) ? bootUs : null,
      receiptUtcUs,
      utcUs: receiptUtcUs,
      utcEvidence: receiptUtcUs === null ? null : 'tlog-receipt',
      clockSegment,
    };
  }
  anchor(ref, utcUs, evidence) {
    if (!finite(ref.bootUs) || !finite(utcUs) || utcUs < 946684800e6 || utcUs > 4102444800e6)
      return;
    const segment = this.clockSegments.get(ref.clockSegment);
    const offset = utcUs - ref.bootUs;
    const previous = segment.anchors.at(-1);
    if (previous && Math.abs(previous.offsetUs - offset) > 2e6)
      this.diag('utc_anchor_disagreement', {
        segment: segment.id,
        differenceUs: offset - previous.offsetUs,
      });
    // Bounded anchors, keeping recent samples as well as the first.
    if (segment.anchors.length >= 128) segment.anchors.splice(1, 1);
    segment.anchors.push({ bootUs: ref.bootUs, utcUs, offsetUs: offset, evidence });
  }
  addHome(home, sourceRef) {
    const previous = this.homes.findLast(
      (h) =>
        h.sourceRef.vehicleId === sourceRef.vehicleId &&
        h.sourceRef.clockSegment === sourceRef.clockSegment,
    );
    if (
      previous &&
      previous.lat === home.lat &&
      previous.lon === home.lon &&
      previous.altMsl === home.altMsl
    )
      return;
    this.homes.push({ ...home, sourceRef });
    if (this.homes.length > 4096) {
      this.homes.splice(1, 1);
      this.diag('home_history_limited');
    }
  }
  addTrack(point, ref) {
    if (!coordinate(point.lat, point.lon) || !finite(point.altMsl)) return;
    const key = `${ref.vehicleId}/${point.source}`;
    if (!this.tracks.has(key)) {
      if (this.tracks.size >= 16) {
        this.diag('track_source_limit');
        return;
      }
      this.tracks.set(key, {
        vehicleId: ref.vehicleId,
        source: point.source,
        points: [],
        count: 0,
        stride: 1,
        last: null,
      });
    }
    const t = this.tracks.get(key);
    const p = { ...point, ...ref };
    t.count++;
    t.last = p;
    if ((t.count - 1) % t.stride) return;
    t.points.push(p);
    if (t.points.length > this.options.maxTrackPoints) {
      t.points = t.points.filter((_, i) => !(i % 2));
      t.stride *= 2;
      this.diag('track_decimated', key);
    }
  }
  finish() {
    const utc = (ref) => {
      const anchors = this.clockSegments.get(ref.clockSegment)?.anchors || [];
      if (!finite(ref.bootUs) || !anchors.length) return;
      let closest = anchors[0];
      for (const a of anchors)
        if (Math.abs(a.bootUs - ref.bootUs) < Math.abs(closest.bootUs - ref.bootUs)) closest = a;
      ref.bootUtcUs = ref.bootUs + closest.offsetUs;
      ref.bootUtcEvidence = closest.evidence;
      // A tlog receipt UTC and autopilot UTC are distinct source clocks. Keep
      // the actual tlog timeline; do not overwrite it with a derived clock fit.
      if (!finite(ref.utcUs)) {
        ref.utcUs = ref.bootUtcUs;
        ref.utcEvidence = closest.evidence;
      }
    };
    const missionVersions = this.missions.finish();
    const fenceVersions = this.fenceVersions.finish();
    for (const v of [...missionVersions, ...fenceVersions]) {
      for (const ref of [v.startedAt, v.endedAt, v.completedAt, v.confirmedAt, v.rejectedAt])
        if (ref) utc(ref);
      for (const p of v.items) utc(p.sourceRef);
    }
    const tracks = [...this.tracks.values()];
    for (const t of tracks) {
      if (t.last && t.points.at(-1) !== t.last) t.points.push(t.last);
      const firstFix = this.firstGpsFix.get(t.vehicleId);
      if (t.source === 'GLOBAL_POSITION_INT' && firstFix !== undefined)
        t.points = t.points.filter((p) => p.byteOffset >= firstFix);
      for (const p of t.points) utc(p);
      delete t.last;
    }
    tracks.sort(
      (a, b) =>
        (['POS', 'GLOBAL_POSITION_INT'].includes(b.source) ? 1 : 0) -
          (['POS', 'GLOBAL_POSITION_INT'].includes(a.source) ? 1 : 0) || b.count - a.count,
    );
    const primary = tracks[0];
    const vehicle = primary?.vehicleId;
    const selectable = (list) => list.filter((v) => belongsToVehicle(v, vehicle));
    const latest = (list) => {
      const all = selectable(list);
      const barrierIndex = all.findLastIndex((v) => v.invalidatesPrevious);
      const available = barrierIndex < 0 ? all : all.slice(barrierIndex);
      return (
        available.filter((v) => v.complete && !v.rejected).at(-1) ||
        available.filter((v) => !v.rejected && v.items.length).at(-1) ||
        available.findLast((v) => v.invalidatesPrevious) ||
        null
      );
    };
    const selectedMission = latest(missionVersions);
    const selectedFence = latest(fenceVersions);
    const mission = selectedMission
      ? { ...selectedMission, segments: missionSegments(selectedMission.items) }
      : { items: [], segments: [], complete: false };
    if (selectedMission && !selectedMission.complete) this.diag('partial_mission_selected');
    if (selectedFence && !selectedFence.complete) this.diag('partial_fence_selected');
    for (const h of this.homes) utc(h.sourceRef);
    const home =
      this.homes.filter((h) => !vehicle || h.sourceRef.vehicleId === vehicle).at(-1) || null;
    return {
      source: this.source,
      primaryVehicleId: vehicle ?? null,
      missionVersions,
      fenceVersions,
      mission,
      fences: selectedFence ? fenceShapes(selectedFence, (code) => this.diag(code), home) : [],
      selectedFenceVersion: selectedFence,
      track: primary?.points || [],
      tracks,
      home,
      homes: this.homes,
      clockSegments: [...this.clockSegments.values()],
      diagnostics: this.diagnostics,
      diagnosticCounts: this.counts,
    };
  }
}

const FIELD = {
  b: [1, 'getInt8'],
  B: [1, 'getUint8'],
  M: [1, 'getUint8'],
  h: [2, 'getInt16'],
  H: [2, 'getUint16'],
  i: [4, 'getInt32'],
  I: [4, 'getUint32'],
  f: [4, 'getFloat32'],
  d: [8, 'getFloat64'],
  q: [8, 'getBigInt64'],
  Q: [8, 'getBigUint64'],
  c: [2, 'getInt16', 100],
  C: [2, 'getUint16', 100],
  e: [4, 'getInt32', 100],
  E: [4, 'getUint32', 100],
  L: [4, 'getInt32', 1e7],
  n: [4],
  N: [16],
  Z: [64],
  a: [64],
};
const BIN_WANTED = new Set(['CMD', 'FNCE', 'MSG', 'POS', 'GPS', 'ORGN']);
function decodeBin(bytes, format) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out = {};
  let offset = 3;
  for (let i = 0; i < format.format.length; i++) {
    const c = format.format[i];
    const f = FIELD[c];
    if (!f || offset + f[0] > bytes.length) return null;
    const name = format.columns[i];
    if (name)
      out[name] = f[1]
        ? Number(view[f[1]](offset, true)) / (f[2] || 1)
        : c === 'a'
          ? null
          : string(bytes.subarray(offset, offset + f[0]));
    offset += f[0];
  }
  return out;
}
// GPS UTC offset history, protocol facts. Dates are effective UTC boundaries.
const GPS_LEAPS = [
  '1981-07-01',
  '1982-07-01',
  '1983-07-01',
  '1985-07-01',
  '1988-01-01',
  '1990-01-01',
  '1991-01-01',
  '1992-07-01',
  '1993-07-01',
  '1994-07-01',
  '1996-01-01',
  '1997-07-01',
  '1999-01-01',
  '2006-01-01',
  '2009-01-01',
  '2012-07-01',
  '2015-07-01',
  '2017-01-01',
].map((d) => Date.parse(`${d}T00:00:00Z`) * 1000);
function gpsUtc(week, towMs) {
  if (!Number.isInteger(week) || week < 1000 || !finite(towMs) || towMs < 0 || towMs >= 604800000)
    return null;
  const gps = 315964800e6 + week * 604800e6 + towMs * 1000;
  let utc = gps;
  for (let i = 0; i < 3; i++) utc = gps - GPS_LEAPS.filter((d) => utc >= d).length * 1e6;
  return utc;
}

async function parseBin(reader, state) {
  const formats = new Map();
  let lost = 0;
  let missionKey = 'dataflash:0';
  let fenceKey = 'dataflash:1';
  while (await reader.ensure(3)) {
    if (reader.peek() !== 0xa3 || reader.peek(1) !== 0x95) {
      reader.take(1);
      lost++;
      continue;
    }
    const id = reader.peek(2);
    const fmt = formats.get(id);
    const length = id === 128 ? 89 : fmt?.length;
    if (!length || length < 3 || length > 255) {
      reader.take(1);
      lost++;
      continue;
    }
    if (!(await reader.ensure(length))) {
      state.diag('truncated_bin_record');
      break;
    }
    const byteOffset = reader.offset;
    const bytes = reader.take(length);
    if (id === 128) {
      const type = bytes[3];
      const recordLength = bytes[4];
      const name = string(bytes.subarray(5, 9));
      const format = string(bytes.subarray(9, 25));
      const columns = string(bytes.subarray(25, 89)).split(',');
      const expected = [...format].reduce((n, c) => n + (FIELD[c]?.[0] || 1000), 3);
      if (expected !== recordLength) {
        state.diag('invalid_bin_format', name);
        continue;
      }
      formats.set(type, { length: recordLength, name, format, columns });
      continue;
    }
    if (!BIN_WANTED.has(fmt.name)) continue;
    const r = decodeBin(bytes, fmt);
    if (!r) {
      state.diag('invalid_bin_record', fmt.name);
      continue;
    }
    const ref = state.ref(fmt.name, byteOffset, 'dataflash', r.TimeUS ?? null);
    missionKey = `dataflash:${ref.clockSegment}:0`;
    fenceKey = `dataflash:${ref.clockSegment}:1`;
    if (fmt.name === 'MSG') {
      if (r.Message === 'New mission')
        state.missions.start(missionKey, null, ref, 'dataflash-dump');
      if (r.Message === 'New fence')
        state.fenceVersions.start(fenceKey, null, ref, 'dataflash-dump', {
          invalidatesPrevious: true,
        });
    } else if (fmt.name === 'CMD') {
      const raw = {
        seq: r.CNum,
        command: r.CId,
        frame: r.Frame,
        param1: r.Prm1,
        param2: r.Prm2,
        param3: r.Prm3,
        param4: r.Prm4,
        x: r.Lat,
        y: r.Lng,
        z: r.Alt,
      };
      const active = state.missions.active.get(missionKey);
      if (active?.expectedCount === null && !active.items.length && !active.slots.size)
        active.expectedCount = r.CTot;
      state.missions.add(missionKey, missionItem(raw, ref), r.CTot, ref, 'dataflash-dump');
    } else if (fmt.name === 'FNCE') {
      const raw = {
        seq: r.Seq,
        fenceType: r.Type,
        vertexCount: r.Count,
        radius: r.Radius,
        frame: 0,
        x: r.Lat,
        y: r.Lng,
        z: null,
      };
      const active = state.fenceVersions.active.get(fenceKey);
      if (active?.expectedCount === null && !active.slots.size) active.expectedCount = r.Tot;
      state.fenceVersions.add(fenceKey, missionItem(raw, ref), r.Tot, ref, 'dataflash-dump');
    } else if (fmt.name === 'POS')
      state.addTrack(
        { lat: r.Lat, lon: r.Lng, altMsl: r.Alt, altRelative: r.RelHomeAlt ?? null, source: 'POS' },
        ref,
      );
    else if (fmt.name === 'GPS' && r.Status >= 3) {
      const utc = gpsUtc(r.GWk, r.GMS);
      if (utc !== null) state.anchor(ref, utc, 'GPS-week/TOW; leap-table-through-2017');
      state.addTrack(
        { lat: r.Lat, lon: r.Lng, altMsl: r.Alt, fix: r.Status, source: `GPS${r.I ?? 0}` },
        ref,
      );
    } else if (fmt.name === 'ORGN' && r.Type === 1 && coordinate(r.Lat, r.Lng) && finite(r.Alt))
      state.addHome({ lat: r.Lat, lon: r.Lng, altMsl: r.Alt }, ref);
  }
  if (lost) state.diag('bin_resynchronized_bytes', lost);
  if (reader.bytes.length - reader.pos) state.diag('trailing_bin_bytes');
}

// [CRC-extra, minimum payload length], independently recorded wire metadata.
const MAV = {
  0: [50, 9],
  2: [137, 12],
  24: [24, 30],
  33: [104, 28],
  38: [9, 6],
  39: [254, 37],
  40: [230, 4],
  43: [132, 2],
  44: [221, 4],
  45: [232, 2],
  47: [153, 3],
  51: [196, 4],
  73: [38, 37],
  160: [78, 12],
  242: [104, 52],
};
function crcAccumulate(crc, value) {
  let t = value ^ (crc & 255);
  t ^= (t << 4) & 255;
  return ((crc >>> 8) ^ (t << 8) ^ (t << 3) ^ (t >>> 4)) & 65535;
}
function validCrc(bytes, extra, end) {
  let crc = 65535;
  for (let i = 1; i < end; i++) crc = crcAccumulate(crc, bytes[i]);
  crc = crcAccumulate(crc, extra);
  return crc === bytes[end] + bytes[end + 1] * 256;
}
const endpoint = (system, component) => `${system}:${component}`;
function endpointMatches(a, b) {
  const [as, ac] = a.split(':');
  const [bs, bc] = b.split(':');
  return as === bs && (ac === bc || ac === '0' || bc === '0');
}
function paddedPayload(bytes, start, length) {
  const p = new Uint8Array(255);
  p.set(bytes.subarray(start, start + length));
  return new DataView(p.buffer);
}

async function parseMavlink(reader, state, timestamped) {
  const fixes = new Map();
  let lost = 0;
  while (await reader.ensure(timestamped ? 9 : 1)) {
    const prefix = timestamped ? 8 : 0;
    const magic = reader.peek(prefix);
    if (![0xfe, 0xfd].includes(magic)) {
      reader.take(1);
      lost++;
      continue;
    }
    const header = magic === 0xfd ? 10 : 6;
    if (!(await reader.ensure(prefix + header))) {
      state.diag('truncated_mavlink_header');
      break;
    }
    const payloadLength = reader.peek(prefix + 1);
    const flags = magic === 0xfd ? reader.peek(prefix + 2) : 0;
    const signature = flags & 1 ? 13 : 0;
    const length = header + payloadLength + 2 + signature;
    if (!(await reader.ensure(prefix + length))) {
      state.diag('truncated_mavlink_packet');
      reader.take(1);
      lost++;
      continue;
    }
    if (flags & ~1) {
      state.diag('unsupported_mavlink_incompatibility');
      reader.take(1);
      lost++;
      continue;
    }
    const offset = reader.offset;
    const all = reader.view(prefix + length);
    const bytes = all.subarray(prefix);
    const id = magic === 0xfd ? bytes[7] + bytes[8] * 256 + bytes[9] * 65536 : bytes[5];
    const spec = MAV[id];
    if (!spec) {
      // Unsupported messages cannot be CRC authenticated by this narrow reader.
      // Verify the next packet boundary before trusting an unknown length.
      const next = prefix + length + prefix;
      const hasNext = await reader.ensure(next + 1);
      if (!hasNext || ![0xfe, 0xfd].includes(reader.peek(next))) {
        state.diag('unknown_mavlink_boundary_error');
        reader.take(1);
        lost++;
        continue;
      }
      reader.take(prefix + length);
      continue;
    }
    if (
      (magic === 0xfe && payloadLength < spec[1]) ||
      !validCrc(bytes, spec[0], header + payloadLength)
    ) {
      state.diag('mavlink_crc_or_length_error', { id, byteOffset: offset });
      reader.take(1);
      lost++;
      continue;
    }
    reader.take(prefix + length);
    if (signature) state.diag('mavlink_signature_not_authenticated');
    const receipt = timestamped
      ? Number(new DataView(all.buffer, all.byteOffset, 8).getBigUint64(0, false))
      : null;
    const receiptUtcUs = receipt > 946684800e6 && receipt < 4102444800e6 ? receipt : null;
    const src = endpoint(bytes[magic === 0xfd ? 5 : 3], bytes[magic === 0xfd ? 6 : 4]);
    const p = paddedPayload(bytes, header, payloadLength);
    const u8 = (o) => p.getUint8(o);
    const u16 = (o) => p.getUint16(o, true);
    const i32 = (o) => p.getInt32(o, true);
    const u32 = (o) => p.getUint32(o, true);
    const f = (o) => p.getFloat32(o, true);
    const u64 = (o) => Number(p.getBigUint64(o, true));
    const bootUs = [2, 33].includes(id) ? u32(id === 2 ? 8 : 0) * 1000 : null;
    const ref = state.ref(`MAVLink:${id}`, offset, src, bootUs, receiptUtcUs);
    if (id === 2) {
      state.anchor(ref, u64(0), 'SYSTEM_TIME');
      continue;
    }
    if (id === 24) {
      fixes.set(src, u8(28));
      if (u8(28) < 3) continue;
      if (!state.firstGpsFix.has(src)) state.firstGpsFix.set(src, offset);
      const clock = u64(0);
      if (clock > 946684800e6 && clock < 4102444800e6) {
        ref.sourceUtcUs = clock;
        ref.sourceUtcEvidence = 'GPS_RAW_INT Unix';
        if (!finite(ref.utcUs)) {
          ref.utcUs = clock;
          ref.utcEvidence = ref.sourceUtcEvidence;
        }
      } else ref.bootUs = clock;
      state.addTrack(
        {
          lat: i32(8) / 1e7,
          lon: i32(12) / 1e7,
          altMsl: i32(16) / 1000,
          fix: u8(28),
          source: 'GPS_RAW_INT',
        },
        ref,
      );
      continue;
    }
    if (id === 33) {
      if ((fixes.has(src) && fixes.get(src) < 3) || (!i32(4) && !i32(8) && !fixes.has(src)))
        continue;
      state.addTrack(
        {
          lat: i32(4) / 1e7,
          lon: i32(8) / 1e7,
          altMsl: i32(12) / 1000,
          altRelative: i32(16) / 1000,
          source: 'GLOBAL_POSITION_INT',
        },
        ref,
      );
      continue;
    }
    if (id === 242) {
      const lat = i32(0) / 1e7;
      const lon = i32(4) / 1e7;
      if (coordinate(lat, lon)) state.addHome({ lat, lon, altMsl: i32(8) / 1000 }, ref);
      continue;
    }
    if (id === 160) {
      state.diag(
        'legacy_fence_points_not_interpreted',
        'Legacy return/closure protocol requires an explicit complete snapshot; not merged with mission fence uploads.',
      );
      continue;
    }
    if (![38, 39, 40, 43, 44, 45, 47, 51, 73].includes(id)) continue;
    let targetSystem, targetComponent, type;
    if ([39, 73].includes(id)) {
      targetSystem = u8(32);
      targetComponent = u8(33);
      type = u8(37);
    } else if ([38, 40, 44, 51].includes(id)) {
      targetSystem = u8(id === 38 ? 4 : 2);
      targetComponent = u8(id === 38 ? 5 : 3);
      type = u8(id === 38 ? 6 : 4);
    } else {
      targetSystem = u8(0);
      targetComponent = u8(1);
      type = u8(id === 47 ? 3 : 2);
    }
    const dest = endpoint(targetSystem, targetComponent);
    if (type === 255 && id === 45) {
      // ALL is legal only for CLEAR_ALL. Keep one linked transaction in each
      // history, and do not make an empty plan applicable before its ACK.
      for (const [family, versions] of [
        [0, state.missions],
        [1, state.fenceVersions],
      ]) {
        for (const [oldKey, v] of versions.active)
          if (v.sender && endpointMatches(v.sender, src) && endpointMatches(v.receiver, dest))
            versions.active.delete(oldKey);
        versions.start(
          `${src}>${dest}/${family}/${ref.clockSegment}/all-${offset}`,
          0,
          ref,
          'clear-all-request',
          { sender: src, receiver: dest, missionType: family, clearAllId: offset },
        );
      }
      continue;
    }
    if (type === 255 && id === 47) {
      const matches = [state.missions, state.fenceVersions].map((versions) =>
        [...versions.active.values()].filter(
          (v) =>
            v.clearAllId !== undefined &&
            endpointMatches(v.sender, dest) &&
            endpointMatches(v.receiver, src),
        ),
      );
      if (
        matches.some((list) => list.length !== 1) ||
        matches[0][0].clearAllId !== matches[1][0].clearAllId
      ) {
        state.diag('unmatched_or_ambiguous_mission_ack');
        continue;
      }
      for (const [v] of matches) {
        if (u8(2) !== 0 && v.confirmed) continue;
        v.endedAt = ref;
        if (u8(2) === 0) {
          v.confirmed = v.complete = true;
          v.confirmedAt ??= ref;
          v.completedAt ??= ref;
        } else {
          v.rejected = true;
          v.rejectedAt = ref;
          v.diagnostics.push(`MISSION_ACK rejected: ${u8(2)}`);
        }
      }
      continue;
    }
    if (![0, 1].includes(type)) continue;
    let key = `${src}>${dest}/${type}/${ref.clockSegment}`;
    const versions = type === 1 ? state.fenceVersions : state.missions;
    if ([39, 73].includes(id)) {
      const candidates = [...versions.active.values()].filter(
        (v) => v.sender && endpointMatches(v.sender, src) && endpointMatches(v.receiver, dest),
      );
      if (candidates.length === 1) key = candidates[0].key;
    }
    const start = (count, evidence, extra = {}) => {
      for (const [oldKey, v] of versions.active)
        if (v.sender && endpointMatches(v.sender, src) && endpointMatches(v.receiver, dest))
          versions.active.delete(oldKey);
      return versions.start(key, count, ref, evidence, {
        sender: src,
        receiver: dest,
        missionType: type,
        ...extra,
      });
    };
    if (id === 44) start(u16(0), 'mavlink-count');
    else if (id === 38) {
      start(null, 'partial-write', { partial: true });
      state.diag('partial_mission_write');
    } else if (id === 45) start(0, 'clear-request');
    else if ([39, 73].includes(id)) {
      const raw = {
        param1: f(0),
        param2: f(4),
        param3: f(8),
        param4: f(12),
        x: id === 73 ? i32(16) : f(16),
        y: id === 73 ? i32(20) : f(20),
        z: f(24),
        seq: u16(28),
        command: u16(30),
        frame: u8(34),
        current: u8(35),
        autocontinue: u8(36),
      };
      // Guided current=2/3 items are one-shot commands, not a stored mission.
      if (raw.current > 1) {
        state.diag('guided_mission_item_ignored');
        continue;
      }
      versions.add(key, missionItem(raw, ref, id === 73), null, ref, 'orphan-items');
    } else if (id === 47) {
      const matches = [...versions.active.values()].filter(
        (v) =>
          v.sender &&
          endpointMatches(v.sender, dest) &&
          endpointMatches(v.receiver, src) &&
          v.missionType === type,
      );
      if (matches.length !== 1) {
        state.diag('unmatched_or_ambiguous_mission_ack');
        continue;
      }
      const v = matches[0];
      // A guided command or duplicate item may elicit an unrelated error ACK.
      // INVALID_SEQUENCE (13) explicitly permits retrying the requested item.
      if (u8(2) !== 0 && (v.confirmed || (u8(2) === 13 && v.evidence !== 'clear-request'))) {
        state.diag('nonterminal_mission_ack', u8(2));
        continue;
      }
      v.endedAt = ref;
      if (u8(2) === 0) {
        v.confirmed = true;
        if (!v.confirmedAt) v.confirmedAt = ref;
        if (v.expectedCount === 0) {
          v.complete = true;
          if (!v.completedAt) v.completedAt = ref;
        }
      } else {
        v.rejected = true;
        if (!v.rejectedAt) v.rejectedAt = ref;
        v.diagnostics.push(`MISSION_ACK rejected: ${u8(2)}`);
      }
    }
  }
  if (lost) state.diag('mavlink_resynchronized_bytes', lost);
  if (reader.bytes.length - reader.pos) state.diag('trailing_mavlink_bytes');
}

/** File/Blob stays local; reads are bounded even for multi-gigabyte logs. */
export async function parseFlightLog(file, options = {}) {
  if (!file || !finite(file.size) || typeof file.slice !== 'function')
    throw new TypeError('Expected a local File or Blob');
  const config = {
    chunkSize: 1024 * 1024,
    maxTrackPoints: 100000,
    maxVersions: 128,
    bootResetThresholdUs: 30e6,
    ...options,
  };
  config.chunkSize = Math.max(512, Math.min(4 * 1024 * 1024, config.chunkSize));
  config.maxTrackPoints = Math.max(2, Math.min(1000000, config.maxTrackPoints));
  config.maxVersions = Math.max(2, Math.min(1000, config.maxVersions));
  const reader = new ChunkReader(file, config);
  const state = new LogState(file, config);
  if (!(await reader.ensure(3))) throw new Error('Flight log is empty or truncated');
  if (reader.peek() === 0xa3 && reader.peek(1) === 0x95) {
    state.source.format = 'dataflash-bin';
    await parseBin(reader, state);
  } else if ([0xfe, 0xfd].includes(reader.peek())) {
    state.source.format = 'mavlink-raw';
    state.diag('no_tlog_receipt_timestamps');
    await parseMavlink(reader, state, false);
  } else if ((await reader.ensure(9)) && [0xfe, 0xfd].includes(reader.peek(8))) {
    state.source.format = 'mavlink-tlog';
    await parseMavlink(reader, state, true);
  } else
    throw new Error('Unsupported flight log: expected ArduPilot BIN or timestamped MAVLink tlog');
  abort(config.signal);
  return state.finish();
}
