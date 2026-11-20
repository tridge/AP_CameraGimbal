// builds the viewer into dist/, copied to SITE/viewer/
import { fileURLToPath } from 'node:url';

export default {
  base: './',
  resolve: {
    // Spark's pre-minified build; library mode does not minify
    alias: {
      '@sparkjsdev/spark': fileURLToPath(
        new URL('node_modules/@sparkjsdev/spark/dist/spark.module.min.js', import.meta.url),
      ),
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    lib: { entry: 'src/viewer.js', formats: ['es'], fileName: () => 'viewer.js' },
  },
};
