// static build into dist/; served from any path. Stages import the viewer's media code from
// tools/viewer/src, and the export stage the splat viewer's pinned Spark (SPZ encoder), both
// outside this package.
import { fileURLToPath } from 'node:url';

const tools = fileURLToPath(new URL('../../../', import.meta.url));
const spark = fileURLToPath(
  new URL('../../viewer/node_modules/@sparkjsdev/spark/dist/spark.module.min.js', import.meta.url),
);

export default {
  base: './',
  resolve: { alias: { '@sparkjsdev/spark': spark } },
  server: { fs: { allow: ['.', tools] } },
  build: { outDir: 'dist', emptyOutDir: true, target: 'es2022' },
  worker: { format: 'es' },
};
