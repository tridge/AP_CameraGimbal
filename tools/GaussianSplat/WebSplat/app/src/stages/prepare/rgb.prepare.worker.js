/** Stage 5b worker for RGB recordings (workers/runtime.js protocol): train_rgb.py's scene setup
 * on map's model and select's RGB frames. msg: {selectDir, mapDir, outDir, prepare: {option
 * names}}. Writes outDir/{dataset.json, targets.u8, points.f32, ceiling.f32}; progress in images.
 */
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, root, writeFile, writeJson } from '../../storage/opfs.js';
import { prepareRgbScene } from './rgb-scene.js';

serveStage(async (msg, api) => {
  api.phase('load');
  const info = await readJson(`${msg.selectDir}/frames.json`);
  const read = async (p) => (await readFile(p)).arrayBuffer();
  const model = {
    cameras: await read(`${msg.mapDir}/final/cameras.bin`),
    images: await read(`${msg.mapDir}/final/images.bin`),
    points3D: await read(`${msg.mapDir}/final/points3D.bin`),
  };
  const rgb = await readFile(`${msg.selectDir}/rgb.u8`);
  const px = info.width * info.height * 3;
  if (rgb.size !== info.frames.length * px)
    throw new Error(`rgb.u8 is ${rgb.size} bytes, expected ${info.frames.length * px}`);
  const parts = `${msg.outDir}/targets.u8`.split('/').filter(Boolean);
  let d = await root();
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
  const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
  try {
    api.phase('targets');
    const res = await prepareRgbScene(
      {
        info,
        model,
        readRgb: async (pos) =>
          new Uint8Array(await rgb.slice(pos * px, (pos + 1) * px).arrayBuffer()),
        writeTarget: (n, bytes) => w.write(bytes),
        progress: (done, total) => api.progress(done, total),
        checkStop: () => api.throwIfStopped(),
        log: (t) => api.log(t),
      },
      msg.prepare ?? {},
    );
    await w.close();
    api.phase('write');
    await writeFile(`${msg.outDir}/points.f32`, res.points);
    await writeFile(`${msg.outDir}/ceiling.f32`, res.ceiling);
    await writeJson(`${msg.outDir}/dataset.json`, res.dataset);
    const ds = res.dataset;
    return {
      kind: 'rgb',
      images: ds.images.length,
      test: ds.stats.test,
      points: ds.points.count,
      width: ds.width,
      height: ds.height,
      timings: res.timings,
    };
  } catch (e) {
    await w.abort().catch(() => {});
    throw e;
  }
});
