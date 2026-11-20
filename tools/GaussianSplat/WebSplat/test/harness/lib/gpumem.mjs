// GPU memory/utilisation sampler: nvidia-smi locally or over ssh (the win11 machine). Records the
// whole GPU and, where nvidia-smi reports it, the test Chrome's own GPU process (rows whose
// process name contains `match`, e.g. the profile directory), since other work may share the GPU.
import { execFile } from 'node:child_process';

const GPU =
  'nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader,nounits';
const APPS =
  'nvidia-smi --query-compute-apps=pid,used_memory,process_name --format=csv,noheader,nounits';

function run(sshHost, cmd) {
  return new Promise((resolve) => {
    const [c, argv] = sshHost
      ? ['ssh', ['-o', 'LogLevel=ERROR', sshHost, cmd]]
      : ['sh', ['-c', cmd]];
    execFile(c, argv, { timeout: 8000, maxBuffer: 4 << 20 }, (err, out) =>
      resolve(err ? null : out),
    );
  });
}

async function query(sshHost, match) {
  const g = await run(sshHost, `${GPU}; echo ---; ${APPS}`);
  if (!g) return null;
  const [gpu, apps = ''] = g.split('---');
  const [name, used, total, util] = gpu
    .trim()
    .split('\n')[0]
    .split(',')
    .map((s) => s.trim());
  let procMiB = null;
  if (match) {
    for (const line of apps.trim().split('\n')) {
      const [, mem, ...rest] = line.split(',');
      if (rest.join(',').includes(match) && /^\s*\d+\s*$/.test(mem ?? ''))
        procMiB = (procMiB ?? 0) + +mem;
    }
  }
  return { name, usedMiB: +used, totalMiB: +total, util: +util, procMiB };
}

export class GpuSampler {
  constructor({ sshHost = null, match = null, everyMs = 1000 } = {}) {
    Object.assign(this, { sshHost, match, everyMs, samples: [], marks: [] });
  }

  async start() {
    this.base = await query(this.sshHost, this.match);
    this.running = true;
    this.loop = (async () => {
      while (this.running) {
        const t = Date.now();
        const s = await query(this.sshHost, this.match);
        if (s) this.samples.push({ t, ...s });
        await new Promise((r) => setTimeout(r, Math.max(0, this.everyMs - (Date.now() - t))));
      }
    })();
    return this.base;
  }

  mark(label) {
    this.marks.push({ t: Date.now(), label });
  }

  async stop() {
    this.running = false;
    await this.loop;
  }

  toJSON() {
    return {
      sshHost: this.sshHost,
      match: this.match,
      base: this.base,
      marks: this.marks,
      samples: this.samples,
    };
  }

  // peak whole-GPU use above the starting value, and the test process's own peak
  peak(t0 = 0, t1 = Infinity) {
    const s = this.samples.filter((x) => x.t >= t0 && x.t <= t1);
    if (!s.length || !this.base) return null;
    const max = Math.max(...s.map((x) => x.usedMiB));
    const proc = s.map((x) => x.procMiB).filter((x) => x != null);
    return {
      peakMiB: max,
      aboveStartMiB: max - this.base.usedMiB,
      processPeakMiB: proc.length ? Math.max(...proc) : null,
      maxUtil: Math.max(...s.map((x) => x.util)),
    };
  }
}
