// Controlled injections, not a field accuracy claim. Run with node; no flight files.
import { analyzeThermal } from '../src/domain/thermal-analysis.js';
const width = 128,
  height = 96,
  sigma = 1.2,
  noiseSigma = 2;
const report = {
  description:
    'Seeded quantized Gaussian noise + gradient + row/column offsets; 24 trials per signal level. Candidate threshold3, top10, within2px of injection. Multi-scale trial count is not a corrected probability.',
  width,
  height,
  sigmaPixels: sigma,
  noiseSigmaRaw: noiseSigma,
  rows: [],
};
for (const amplitude of [0, 2, 4, 8])
  for (const method of ['matched', 'cfar', 'multiscale']) {
    let detected = 0,
      falsePeaks = 0;
    for (let trial = 0; trial < 24; trial++) {
      let seed = 103 + trial;
      const uniform = () =>
        ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) + 1) / 4294967297;
      const normal = () => Math.sqrt(-2 * Math.log(uniform())) * Math.cos(2 * Math.PI * uniform());
      const rows = Array.from({ length: height }, normal),
        columns = Array.from({ length: width }, normal);
      const cx = 63 + uniform(),
        cy = 47 + uniform();
      const raw = Uint16Array.from({ length: width * height }, (_, i) => {
        const x = i % width,
          y = Math.floor(i / width);
        return Math.round(
          18000 +
            0.05 * x +
            0.03 * y +
            noiseSigma * normal() +
            rows[y] +
            columns[x] +
            amplitude * Math.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * sigma * sigma)),
        );
      });
      const result = analyzeThermal(raw, width, height, { method, sigma, destripe: true });
      if (amplitude && result.candidates.some((p) => Math.hypot(p.x - cx, p.y - cy) <= 2))
        detected++;
      falsePeaks += result.candidates.filter(
        (p) => !amplitude || Math.hypot(p.x - cx, p.y - cy) > 5,
      ).length;
    }
    report.rows.push({
      method,
      perPixelPeakToNoise: amplitude / noiseSigma,
      trials: 24,
      recovered: amplitude ? detected : null,
      otherCandidatesPerFrame: falsePeaks / 24,
    });
  }
console.log(JSON.stringify(report, null, 2));
