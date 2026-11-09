// The viewer's own temperatures against a gsplat reference (viewer_reference.py): the "peak in
// view" (the hottest covered pixel) and the pointer readout at the reference's hottest pixel.
// Bound: 0.005 of the scaled value at that temperature, plus the 0.1 C display rounding, as for
// the tiny model in stage 7a. With the thermal settings (extended accumulator, alpha threshold
// 1/255, depth order, footprints to 4096 px, a float32 target) the measured residuals on G1 and
// run13 (start views and held-out view 00140) are at most 0.002 on the laptop (notes/stage7.md
// for win11). The half-float target drifted by up to 0.023, the 512 px footprint cap by 0.03, the
// packed accumulator and a sampled peak by far more.
export function tolerance(ref, c) {
  const s = ref.temperature_scale;
  const norm = Math.log1p((s.t1_c - s.t0_c) / s.k_c);
  const v = Math.log1p(Math.max(c - s.t0_c, 0) / s.k_c) / norm;
  return 0.1 + 0.005 * s.k_c * norm * Math.exp(Math.min(1, v) * norm);
}

// page: a viewer page showing the splat from ref.camera at ref.width x ref.height (thermal
// controls up); hides the panel, waits for the full-size render
export async function compareViewer(page, ref) {
  await page.addStyleTag({
    content: '#panel, .credits, #status, .project, #open-panel { display: none !important; }',
  });
  await page.waitForTimeout(2000);
  const size = await page.evaluate(() => [
    document.getElementById('canvas').width,
    document.getElementById('canvas').height,
  ]);
  const stats = await page.evaluate(() => document.getElementById('stats').textContent);
  await page.mouse.move(ref.argmax[0] + 0.5, ref.argmax[1] + 0.5);
  await page.waitForTimeout(300);
  const hover = await page.evaluate(() => document.getElementById('hover').textContent);
  const peak = Number(stats.match(/peak (?:≈ )?(-?[\d.]+) °C/)?.[1]);
  const approx = stats.includes('≈') && hover.includes('≈');
  const notice = await page.evaluate(() => {
    const p = document.getElementById('precision');
    return p ? p.textContent : null;
  });
  // the image shown is not black (a float target that cannot be filtered samples black)
  const shot = await page.screenshot({
    clip: { x: 0, y: 0, width: ref.width, height: ref.height },
  });
  const litPixels = await countLit(page, shot);
  const lit = litPixels >= 100; // not the black image of a float target that cannot be filtered
  const readout = Number(hover.match(/(-?[\d.]+) °C/)?.[1]);
  const tp = tolerance(ref, ref.peak_c);
  const tr = tolerance(ref, ref.argmax_c);
  const ok =
    lit &&
    !notice &&
    !approx &&
    size[0] === ref.width &&
    size[1] === ref.height &&
    Math.abs(peak - ref.peak_c) <= tp &&
    Math.abs(readout - ref.argmax_c) <= tr;
  return {
    ok,
    detail:
      `peak ${peak} vs gsplat ${ref.peak_c.toFixed(2)} C (bound ${tp.toFixed(2)}), readout ${readout} ` +
      `vs ${ref.argmax_c.toFixed(2)} C (bound ${tr.toFixed(2)}), canvas ${size}`,
    peak,
    readout,
    approx,
    notice,
    lit,
    litPixels,
    errors: { peakC: peak - ref.peak_c, readoutC: readout - ref.argmax_c },
  };
}

// pixels of a screenshot (PNG) with any channel above 16, decoded in the page
async function countLit(page, png) {
  return page.evaluate(async (b64) => {
    const img = new Image();
    img.src = `data:image/png;base64,${b64}`;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0);
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (Math.max(d[i], d[i + 1], d[i + 2]) > 16) n++;
    return n;
  }, png.toString('base64'));
}
