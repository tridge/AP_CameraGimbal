// Stage list: state, natural-unit progress, elapsed and remaining time per stage.
import { formatDuration, formatEta } from '../pipeline/eta.js';

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

export function renderStages(table, snap) {
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
    tr.dataset.state = s.state;
    tr.querySelector('.title').textContent = s.title;
    const phase = s.phase && s.phase !== 'work' && s.state === 'running' ? ` (${s.phase})` : '';
    tr.querySelector('.state').textContent = s.state + phase;
    const bar = tr.querySelector('progress');
    const p = s.progress;
    if (s.state === 'done') bar.value = 1;
    else if (p?.total) bar.value = Math.min(1, (p.done ?? 0) / p.total);
    else bar.removeAttribute('value');
    tr.querySelector('.units').textContent = s.error ? s.error.message : progressText(s);
    tr.querySelector('.elapsed').textContent = s.elapsedMs ? formatDuration(s.elapsedMs) : '';
    tr.querySelector('.eta').textContent =
      s.state === 'running' || s.state === 'checkpointing'
        ? formatEta(s.remainingSec) + ' left'
        : '';
  });
}
