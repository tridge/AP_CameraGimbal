import manifest from '../package.json' with { type: 'json' };
// Versioned URLs prevent a new deployment from mixing cached WASM/map assets
// with a different library build. Dependency pins are the single source.
export const FFMPEG_VERSION = manifest.dependencies['@ffmpeg/core'];
export const CESIUM_VERSION = manifest.dependencies.cesium;
