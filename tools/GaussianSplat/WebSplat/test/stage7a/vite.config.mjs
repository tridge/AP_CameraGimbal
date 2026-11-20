// Builds the stage 7a test page (page/) with the app's resolve settings into $OUT (argument
// --outDir); the page's worker bundles the export module and Spark as the app build does.
import app from '../../app/vite.config.js';

export default {
  ...app,
  root: new URL('./page/', import.meta.url).pathname,
  build: { ...app.build, emptyOutDir: true },
};
