// static build into dist/; served from any path. Stages import the viewer's media code from
// tools/viewer/src, outside this package.
import { fileURLToPath } from 'node:url';

const tools = fileURLToPath(new URL('../../../', import.meta.url));

export default {
  base: './',
  server: { fs: { allow: ['.', tools] } },
  build: { outDir: 'dist', emptyOutDir: true, target: 'es2022' },
  worker: { format: 'es' },
};
