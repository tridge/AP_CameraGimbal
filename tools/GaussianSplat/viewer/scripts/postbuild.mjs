// after vite build: licence notices of the bundled libraries, and gzip copies for nginx gzip_static
import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const notices = ['three', '@sparkjsdev/spark'].map((name) => {
  const { version } = JSON.parse(readFileSync(`node_modules/${name}/package.json`, 'utf8'));
  return `${name} ${version}\n\n${readFileSync(`node_modules/${name}/LICENSE`, 'utf8').trim()}\n`;
});
writeFileSync('dist/LICENSES.txt', notices.join('\n----\n\n'));

for (const name of readdirSync('dist')) {
  if (/\.(js|css|json|html|txt)$/.test(name)) {
    writeFileSync(`dist/${name}.gz`, gzipSync(readFileSync(`dist/${name}`), { level: 9 }));
  }
}
