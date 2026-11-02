import { pixelSha256 } from './pixel-hash.js';
import { analyzeThermal } from './thermal-analysis.js';
self.onmessage = async ({ data }) => {
  const { id, identity, pixels, width, height, settings, roi } = data;
  try {
    const hash = await pixelSha256(pixels.buffer);
    const result = analyzeThermal(pixels, width, height, settings, roi);
    result.inputSha256 = hash;
    const transfers = new Set();
    for (const key of ['plane', 'score']) if (result[key]) transfers.add(result[key].buffer);
    transfers.add(result.rawStats.bins.buffer);
    if (result.processedStats) transfers.add(result.processedStats.bins.buffer);
    if (result.displayStats) transfers.add(result.displayStats.bins.buffer);
    self.postMessage({ id, identity, result }, [...transfers]);
  } catch (e) {
    self.postMessage({ id, identity, error: e.message || 'Thermal analysis failed.' });
  }
};
