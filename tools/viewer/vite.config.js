import { defineConfig } from 'vite';
// The same artifact layout can be hosted alongside other tools at any prefix.
const base = process.env.VIEWER_BASE_PATH || '/viewer/';
if (!/^\/(?:[A-Za-z0-9_-]+\/)*$/.test(base)) {
  throw new Error('VIEWER_BASE_PATH must be an absolute directory path ending in /.');
}
// Optional development/qualification access to the production terrain service.
// This proxy exists only in Vite; the deployed static site uses its own route.
const terrainProxy = process.env.VIEWER_TERRAIN_PROXY;
const proxy = terrainProxy
  ? { '/quantized/': { target: new URL(terrainProxy).origin, changeOrigin: true } }
  : undefined;
export default defineConfig({
  base,
  worker: { format: 'es' },
  server: { host: '127.0.0.1', port: 5173, strictPort: true, proxy },
  preview: { host: '127.0.0.1', port: 4173, strictPort: true, proxy },
  build: {
    target: 'es2022',
    chunkSizeWarningLimit: 5000,
    rolldownOptions: {
      output: {
        codeSplitting: { groups: [{ name: 'cesium', test: /node_modules.*(?:cesium|@cesium)/ }] },
      },
    },
  },
});
