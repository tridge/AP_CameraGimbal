import { finite } from '../domain/time.js';

/** Validate everything before preparing providers or changing active map state. */
export function validateMapConfiguration(config, baseURL) {
  if (!config || typeof config !== 'object') throw new Error('Map configuration is required.');
  const imagery = config.imagery === undefined ? 'none' : config.imagery;
  if (!['none', 'osm', 'satellite', 'google'].includes(imagery))
    throw new Error('Unknown map imagery provider.');
  const terrainUrl = config.terrainUrl === undefined ? '' : config.terrainUrl;
  if (typeof terrainUrl !== 'string') throw new Error('Terrain URL must be text.');
  if (typeof (config.googleMapsApiKey ?? '') !== 'string')
    throw new Error('Google Maps Tile API key must be text.');
  const googleMapsApiKey = (config.googleMapsApiKey ?? '').trim();
  if (imagery === 'google' && !googleMapsApiKey)
    throw new Error('Enter a Google Maps Tile API key to enable Google imagery.');
  const offset = config.amslToEllipsoidMetres === undefined ? 0 : config.amslToEllipsoidMetres;
  if (!finite(offset)) throw new Error('Geoid offset must be finite.');
  let terrainDatum = config.terrainDatum ?? 'ellipsoid';
  if (!['amsl', 'ellipsoid'].includes(terrainDatum))
    throw new Error('Unknown terrain height datum.');
  if (terrainUrl.trim()) {
    const base = new URL(baseURL);
    const url = new URL(terrainUrl.trim(), base);
    const sharedTerrain =
      url.origin === 'https://plot.ardupilot.org' && url.pathname.startsWith('/quantized/');
    if (sharedTerrain) {
      if (config.terrainDatum !== undefined && config.terrainDatum !== 'amsl')
        throw new Error(
          'The shared plot terrain uses AMSL heights; choose AMSL as its height reference.',
        );
      terrainDatum = 'amsl';
    }
    if (
      !['http:', 'https:'].includes(url.protocol) ||
      (url.origin !== base.origin && !sharedTerrain) ||
      url.username ||
      url.password
    )
      throw new Error(
        'Use terrain on this viewer’s origin or https://plot.ardupilot.org/quantized/.',
      );
  }
  return {
    imagery,
    terrainUrl: terrainUrl.trim(),
    googleMapsApiKey,
    amslToEllipsoidMetres: offset,
    terrainDatum,
  };
}

/** A location needs fresh position telemetry, independently of camera attitude. */
export function freshMapPosition(record) {
  const p = record?.position;
  if (
    !p ||
    ![p.lat_e7, p.lon_e7, p.alt_amsl_m, p.age_ms].every(finite) ||
    p.age_ms < 0 ||
    p.age_ms > 2000
  )
    return null;
  const lat = p.lat_e7 / 1e7;
  const lon = p.lon_e7 / 1e7;
  if (Math.abs(lat) > 90 || Math.abs(lon) > 180) return null;
  return { lat, lon, altMsl: p.alt_amsl_m };
}
