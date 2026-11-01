import { meshSampler } from './mesh-sampler.js';
import { offsetLocation } from '../domain/geometry.js';
import { finite } from '../domain/time.js';

/** CPU-side terrain sampling, independent of the visible globe's mode and LOD.
 * Heights come from the best advertised mesh, never an ellipsoid/flat fallback.
 * Missing tiles load once, in a bounded queue; callers retry after onChange.
 */
export class TerrainElevation {
  constructor(onChange = () => {}, requestFactory = () => undefined) {
    this.onChange = onChange;
    this.requestFactory = requestFactory;
    this.generation = 0;
    this.cache = new Map();
    this.active = 0;
    this.maxTiles = 64;
    this.maxActive = 4;
    this.maxBytes = 64 * 1024 * 1024;
    this.lastIssue = { state: 'unavailable', reason: 'Terrain is not configured in Settings.' };
  }
  configure(
    provider,
    { terrainUrl = '', amslToEllipsoidMetres = 0, terrainDatum = 'ellipsoid' } = {},
  ) {
    this.generation++;
    for (const entry of this.cache.values()) {
      clearTimeout(entry.timer);
      entry.request?.cancel?.();
    }
    this.cache.clear();
    this.active = 0;
    this.provider = terrainUrl ? provider : null;
    this.url = terrainUrl;
    this.offset = amslToEllipsoidMetres;
    this.datum = terrainDatum;
    this.description = terrainUrl ? `Quantized terrain ${terrainUrl}` : 'Terrain not configured';
    this.lastIssue = { state: 'unavailable', reason: 'Terrain is not configured in Settings.' };
    this.onChange();
  }
  forRay(lat, lon) {
    const context = { level: this.levelAt(lat, lon), tiles: new Set() };
    const owner = this;
    return {
      at: (lat, lon) => this.at(lat, lon, context),
      get lastIssue() {
        return owner.lastIssue;
      },
      provenance: () => ({
        ...this.provenance(),
        terrainLevel: context.level,
        tileKeys: [...context.tiles],
      }),
      prefetchRay: (ray, maxRange) => {
        const seen = new Set();
        for (let d = 0; d <= maxRange; d += 20) {
          const p = offsetLocation(ray.pose.lat, ray.pose.lon, ray.north * d, ray.east * d);
          const tile = this.tileAt(p.lat, p.lon, context);
          if (tile && !seen.has(tile.key)) {
            seen.add(tile.key);
            this.ensure(tile);
          }
        }
      },
    };
  }
  levelAt(lat, lon) {
    if (!this.provider?.availability) return null;
    try {
      const level = this.provider.availability.computeMaximumLevelAtPosition({
        latitude: (lat * Math.PI) / 180,
        longitude: (lon * Math.PI) / 180,
        height: 0,
      });
      return Number.isInteger(level) && level >= 0 ? Math.min(level, 12) : null;
    } catch {
      return null;
    }
  }
  tileAt(lat, lon, context) {
    if (!this.provider) return this.unavailable('Terrain is not configured in Settings.');
    if (![lat, lon].every(finite) || Math.abs(lat) > 90 || Math.abs(lon) > 180)
      return this.unavailable('Pixel is outside terrain coverage.');
    const position = {
      latitude: (lat * Math.PI) / 180,
      longitude: (lon * Math.PI) / 180,
      height: 0,
    };
    const availability = this.provider.availability;
    if (!availability)
      return this.unavailable('Terrain service has no tile availability information.');
    const level = context ? context.level : this.levelAt(lat, lon);
    if (level === null) return this.unavailable('Pixel is outside terrain coverage.');
    const xy = this.provider.tilingScheme.positionToTileXY(position, level);
    if (!xy || !availability.isTileAvailable(level, xy.x, xy.y))
      return this.unavailable(`Terrain has no coverage at level ${level} for this pixel.`);
    return { level, x: xy.x, y: xy.y, key: `${level}/${xy.x}/${xy.y}`, position };
  }
  at(lat, lon, context) {
    const tile = this.tileAt(lat, lon, context);
    if (!tile) return null;
    const { level, x, y, key, position } = tile;
    const entry = this.ensure(tile);
    if (!entry) return null;
    context?.tiles.add(key);
    if (entry.state !== 'ready') {
      this.lastIssue =
        entry.state === 'error'
          ? {
              state: 'unavailable',
              reason: 'Terrain tile could not be loaded; hover again to retry.',
            }
          : { state: 'loading', reason: 'Loading terrain for this pixel…' };
      return null;
    }
    const rectangle = this.provider.tilingScheme.tileXYToRectangle(x, y, level);
    const height = entry.sampler.sample(rectangle, position.longitude, position.latitude);
    if (!finite(height)) return this.unavailable('No terrain height at this pixel.');
    this.lastIssue = null;
    return height - (this.datum === 'ellipsoid' ? this.offset : 0);
  }
  ensure({ key, level, x, y }) {
    let entry = this.cache.get(key);
    if (entry?.state === 'error' && Date.now() - entry.failedAt > 10000) {
      this.cache.delete(key);
      entry = null;
    }
    if (!entry) {
      // Evict old completed entries; never orphan a running request.
      while (this.cache.size >= this.maxTiles) {
        const oldest = [...this.cache].find(([, e]) => e.state !== 'loading');
        if (!oldest)
          return this.unavailable('Terrain request queue is full; try this pixel again.');
        this.cache.delete(oldest[0]);
      }
      entry = { key, level, x, y, state: 'queued' };
      this.cache.set(key, entry);
      this.pump();
    } else {
      this.cache.delete(key);
      this.cache.set(key, entry);
    }
    return entry;
  }
  unavailable(reason) {
    this.lastIssue = { state: 'unavailable', reason };
    return null;
  }
  pump() {
    for (const entry of this.cache.values()) {
      if (this.active >= this.maxActive) break;
      if (entry.state !== 'queued') continue;
      const generation = this.generation,
        provider = this.provider;
      entry.state = 'loading';
      entry.request = this.requestFactory();
      this.active++;
      // Deferring prevents callbacks from re-entering a projection synchronously.
      Promise.resolve()
        .then(async () => {
          if (generation !== this.generation) return;
          let pending;
          for (let attempt = 0; attempt < 4; attempt++) {
            if (generation !== this.generation) return;
            pending = provider.requestTileGeometry(entry.x, entry.y, entry.level, entry.request);
            if (pending) break;
            await new Promise((resolve) => setTimeout(resolve, 100));
          }
          if (!pending) throw new Error('Terrain request was deferred');
          return await new Promise((resolve, reject) => {
            entry.timer = setTimeout(() => {
              entry.request?.cancel?.();
              reject(new Error('Terrain request timed out'));
            }, 20000);
            Promise.resolve(pending).then(resolve, reject);
          });
        })
        .then((data) => {
          if (generation !== this.generation) return;
          if (!data || typeof data.interpolateHeight !== 'function')
            throw new Error('Invalid terrain mesh');
          const sampler = meshSampler(data);
          if (sampler.bytes > this.maxBytes) throw new Error('Terrain tile exceeds cache budget');
          entry.data = data;
          entry.sampler = sampler;
          entry.state = 'ready';
          this.trim(entry);
        })
        .catch(() => {
          if (generation !== this.generation) return;
          entry.state = 'error';
          entry.failedAt = Date.now();
        })
        .finally(() => {
          clearTimeout(entry.timer);
          if (generation !== this.generation) return;
          this.active--;
          this.pump();
          this.onChange();
        });
    }
  }
  trim(keep) {
    let bytes = [...this.cache.values()].reduce((sum, e) => sum + (e.sampler?.bytes || 0), 0);
    for (const [key, e] of this.cache) {
      if (bytes <= this.maxBytes) break;
      if (e === keep || e.state === 'loading') continue;
      this.cache.delete(key);
      bytes -= e.sampler?.bytes || 0;
    }
  }
  provenance() {
    return {
      source: this.description,
      terrainUrl: this.url,
      amslToEllipsoidMetres: this.offset,
      terrainDatum: this.datum,
      sampling: 'quantized-mesh triangle; fixed ray level (maximum12)',
      cachedTiles: this.cache.size,
    };
  }
  close() {
    this.configure(null);
  }
}
