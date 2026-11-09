// A viewable PLY of the splat as training has it (a Brush PLY snapshot or a checkpoint's WSSP
// params.bin): the export's PLY form (stream.js plyPass), with its colours, temperature scale and
// start camera, so the result viewer shows it as it will show the result. No DOM.
import { exportInputs } from '../export/export.js';
import { plyPass } from '../export/stream.js';
import { viewCamera } from '../export/view.js';

// in-memory file and sink for plyPass (writes arrive in order)
function memFile(bytes) {
  return { size: bytes.length, read: (offset, length) => bytes.slice(offset, offset + length) };
}

function memSink() {
  const parts = [];
  let size = 0;
  return {
    write(bytes, offset) {
      if (offset !== size) throw new Error('preview: out-of-order write');
      parts.push(bytes.slice());
      size += bytes.length;
    },
    truncate(n) {
      if (n !== 0 || size) throw new Error('preview: unexpected truncate');
    },
    flush() {},
    bytes() {
      const out = new Uint8Array(size);
      let at = 0;
      for (const p of parts) {
        out.set(p, at);
        at += p.length;
      }
      return out;
    },
  };
}

// splat: Uint8Array (PLY or WSSP); dataset, frames: the job's 5b and stage 2 files; view,
// targetPolicy, title: config.export. Returns {bytes, info, count}.
export async function previewPly({ splat, dataset, frames, view, targetPolicy, title }) {
  let camera = null;
  if (view) {
    const { target } = exportInputs(dataset, frames, targetPolicy);
    camera = viewCamera(view, frames.origin, target, dataset.bounds?.radius).camera;
  }
  const sink = memSink();
  const r = await plyPass({
    input: memFile(splat),
    dataset,
    frames,
    title,
    rasterizeMode: 'classic',
    targetPolicy,
    camera,
    ply: sink,
  });
  return { bytes: sink.bytes(), info: r.info, count: r.count };
}
