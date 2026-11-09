// Stage list: state, natural-unit progress, elapsed and remaining time per stage.
import { formatDuration, formatEta } from '../pipeline/eta.js';
import { readFile } from '../storage/opfs.js';

const fmtCount = (n) => (n == null ? '?' : Math.round(n).toLocaleString('en'));

function fmtUnits(unit, done, total) {
  if (unit === 'bytes') {
    const mb = (b) => (b / 1048576).toFixed(1);
    return total ? `${mb(done)} / ${mb(total)} MiB` : `${mb(done)} MiB`;
  }
  if (unit === 'BA rounds')
    return `round ${Math.min(total ?? 99, Math.floor(done) + 1)} of ${total ?? '?'}`;
  return total ? `${fmtCount(done)} / ${fmtCount(total)} ${unit}` : `${fmtCount(done)} ${unit}`;
}

function progressText(s) {
  const p = s.progress;
  if (!p) return '';
  let t = fmtUnits(s.unit, p.done ?? 0, p.total);
  if (p.extra?.gaussians != null) t += `, ${fmtCount(p.extra.gaussians)} Gaussians`;
  // map: the work inside the current round (stages/map/map.stage.worker.js extra)
  if (s.state === 'running' && p.extra?.phase === 'ba' && p.extra.total)
    t += `, bundle adjustment iteration ${p.extra.done} of at most ${p.extra.total}`;
  if (s.state === 'running' && p.extra?.phase === 'triangulate' && p.extra.total)
    t += `, triangulating image ${p.extra.done} of ${p.extra.total}`;
  if (p.extra?.iterations != null && s.state === 'running')
    t += `, ${p.extra.iterations} iterations (open-ended)`;
  return t;
}

// a running stage's capped preview image in OPFS (progress extra.preview: {path, seq, view,
// iteration}), e.g. training's held-out render; read again only when seq changes
export function showPreview(tr, s) {
  const active = s.state === 'running' || s.state === 'checkpointing';
  let img = tr.querySelector('img.preview');
  if (!active) {
    img?.remove();
    return;
  }
  // ordinary progress carries no preview: keep showing the last one
  const prev = s.progress?.extra?.preview;
  if (!prev) return;
  if (!img) {
    img = document.createElement('img');
    img.className = 'preview';
    img.style.cssText = 'display:block;max-width:320px;margin-top:4px;image-rendering:auto';
    tr.querySelector('.bar').append(img);
  }
  if (img.dataset.seq === String(prev.seq)) return;
  img.dataset.seq = String(prev.seq);
  readFile(prev.path)
    .then((f) => {
      const url = URL.createObjectURL(f);
      img.onload = () => URL.revokeObjectURL(url);
      img.src = url;
      img.alt = img.title = `${prev.view} at iteration ${prev.iteration}`;
    })
    .catch(() => {});
}

// needs: {stage, text} for a stage waiting for the user, shown in its row
export function renderStages(table, snap, needs = null) {
  if (table.rows.length !== snap.stages.length) {
    table.replaceChildren(
      ...snap.stages.map((s) => {
        const tr = document.createElement('tr');
        tr.dataset.stage = s.id;
        tr.innerHTML =
          '<td class="title"></td><td class="state"></td><td class="bar"><progress max="1"></progress>' +
          '<div class="units"></div></td><td class="elapsed"></td><td class="eta"></td>';
        return tr;
      }),
    );
  }
  snap.stages.forEach((s, k) => {
    const tr = table.rows[k];
    const asking = needs?.stage === s.id;
    tr.dataset.state = asking ? 'needs-input' : s.state;
    tr.querySelector('.title').textContent = s.title;
    const phase = s.phase && s.phase !== 'work' && s.state === 'running' ? ` (${s.phase})` : '';
    tr.querySelector('.state').textContent = asking ? 'waiting for you' : s.state + phase;
    const bar = tr.querySelector('progress');
    const p = s.progress;
    const active = s.state === 'running' || s.state === 'checkpointing';
    if (s.state === 'done') bar.value = 1;
    else if (p?.total) bar.value = Math.min(1, (p.done ?? 0) / p.total);
    // indeterminate only while work is actually happening
    else if (active) bar.removeAttribute('value');
    else bar.value = 0;
    tr.querySelector('.units').textContent = asking
      ? needs.text
      : s.error
        ? s.error.message
        : progressText(s);
    showPreview(tr, s);
    tr.querySelector('.elapsed').textContent = s.elapsedMs ? formatDuration(s.elapsedMs) : '';
    tr.querySelector('.eta').textContent =
      s.state === 'running' || s.state === 'checkpointing'
        ? formatEta(s.remainingSec) + ' left'
        : '';
  });
}
