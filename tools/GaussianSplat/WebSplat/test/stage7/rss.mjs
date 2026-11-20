// Memory of the test Chrome: the resident set of every process whose command line holds `match`
// (the profile directory), sampled locally from /proc or on the win11 machine over ssh
// (Win32_Process WorkingSetSize). Reports the total and the largest single process (the renderer,
// where the page's workers run).
import { execFile } from 'node:child_process';
import { readdirSync, readFileSync } from 'node:fs';

function local(match) {
  const out = [];
  for (const pid of readdirSync('/proc')) {
    if (!/^\d+$/.test(pid)) continue;
    try {
      const cmd = readFileSync(`/proc/${pid}/cmdline`, 'utf8');
      if (!cmd.includes(match)) continue;
      const m = readFileSync(`/proc/${pid}/status`, 'utf8').match(/VmRSS:\s+(\d+) kB/);
      if (m) out.push(+m[1] * 1024);
    } catch {
      // process ended
    }
  }
  return out;
}

function remote(host, match) {
  const ps =
    `Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ` +
    `Where-Object { $_.CommandLine -like '*${match}*' } | ForEach-Object { $_.WorkingSetSize }`;
  const enc = Buffer.from(ps, 'utf16le').toString('base64');
  return new Promise((resolve) =>
    execFile(
      'ssh',
      ['-o', 'LogLevel=ERROR', host, `powershell -NoProfile -EncodedCommand ${enc}`],
      { timeout: 10000 },
      (err, out) => resolve(err ? null : out.trim().split(/\s+/).filter(Boolean).map(Number)),
    ),
  );
}

const mib = (b) => Math.round(b / 2 ** 20);

export class RssSampler {
  constructor({ match, host = null, everyMs = 250 }) {
    Object.assign(this, { match, host, everyMs: host ? 1000 : everyMs, samples: [] });
  }

  async sample() {
    const v = this.host ? await remote(this.host, this.match) : local(this.match);
    if (!v || !v.length) return null;
    const s = {
      t: Date.now(),
      tag: this.tag ?? null,
      total: v.reduce((a, b) => a + b, 0),
      max: Math.max(...v),
      n: v.length,
    };
    this.samples.push(s);
    return s;
  }

  async start() {
    this.base = await this.sample();
    this.running = true;
    this.loop = (async () => {
      while (this.running) {
        const t = Date.now();
        await this.sample();
        await new Promise((r) => setTimeout(r, Math.max(0, this.everyMs - (Date.now() - t))));
      }
    })();
    return this.base;
  }

  // window: [t0, t1] (ms since the epoch) to report the peak over, e.g. the export stage's run
  async stop(window = null) {
    this.running = false;
    await this.loop;
    const inWin = window
      ? this.samples.filter((s) => s.t >= window[0] - 500 && s.t <= window[1] + 500)
      : this.samples;
    const peak = (k) => (inWin.length ? Math.max(...inWin.map((s) => s[k])) : NaN);
    const byTag = {};
    for (const x of inWin) {
      if (x.tag) byTag[x.tag] = Math.max(byTag[x.tag] ?? 0, mib(x.total - (this.base?.total ?? 0)));
    }
    return {
      samples: inWin.length,
      window,
      peakAboveBaseByPhaseMiB: byTag,
      baseTotalMiB: mib(this.base?.total ?? 0),
      peakTotalMiB: mib(peak('total')),
      peakAboveBaseMiB: mib(peak('total') - (this.base?.total ?? 0)),
      peakProcessMiB: mib(peak('max')),
      baseProcessMiB: mib(this.base?.max ?? 0),
    };
  }
}
