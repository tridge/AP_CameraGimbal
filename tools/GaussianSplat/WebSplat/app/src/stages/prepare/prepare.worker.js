/** Stage 5b worker (workers/runtime.js protocol): train.py's scene loading on map's model and
 * select's raw radiometric frames. msg: {selectDir, mapDir, outDir, prepare: {train.py option
 * names}}. Writes outDir/{dataset.json, targets.f16, points.f32, ceiling.f32} (CONTRACTS.md 5b).
 * Progress in images (two passes over the chosen images, 30% / 70%).
 */
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, root, writeFile, writeJson } from '../../storage/opfs.js';
import { prepareScene } from './scene.js';

serveStage(async (msg, api) => {
  api.phase('load');
  const info = await readJson(`${msg.selectDir}/frames.json`);
  const read = async (p) => (await readFile(p)).arrayBuffer();
  const model = {
    cameras: await read(`${msg.mapDir}/final/cameras.bin`),
    images: await read(`${msg.mapDir}/final/images.bin`),
    points3D: await read(`${msg.mapDir}/final/points3D.bin`),
  };
  const thermal = await readFile(`${msg.selectDir}/thermal.u16`);
  const px = info.width * info.height;
  if (thermal.size !== info.frames.length * px * 2)
    throw new Error(
      `thermal.u16 is ${thermal.size} bytes, expected ${info.frames.length * px * 2}`,
    );
  // targets are streamed: one writable, one image at a time
  const parts = `${msg.outDir}/targets.f16`.split('/').filter(Boolean);
  let d = await root();
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
  const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
  let phase = null;
  try {
    const res = await prepareScene(
      {
        info,
        model,
        readThermal: async (pos) =>
          new Uint16Array(await thermal.slice(pos * px * 2, (pos + 1) * px * 2).arrayBuffer()),
        writeTarget: (n, bits) => w.write(bits),
        progress: (done, total) => {
          const p = done <= 0.3 * total ? 'scale' : 'targets';
          if (p !== phase) api.phase((phase = p));
          api.progress(done, total);
        },
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
      images: ds.images.length,
      test: ds.stats.test,
      hot: ds.stats.hot,
      points: ds.points.count,
      scale: ds.scale,
      hotspot: ds.hotspot,
      timings: res.timings,
    };
  } catch (e) {
    await w.abort().catch(() => {});
    throw e;
  }
});
