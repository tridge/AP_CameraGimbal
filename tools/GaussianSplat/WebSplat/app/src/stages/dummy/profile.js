// Fake stage profiles with realistic work counts and times for the canonical orbit (seconds of
// work at speed 1 on the laptop; the desktop is faster). Shared by the stage defs and the worker.
export const G0 = 37780; // initial Gaussians (run13)
export const G1 = 591000; // final Gaussians

export function gaussians(it) {
  const t = Math.min(it, 15000); // densification stops at half the iterations
  return Math.round(G0 + (G1 - G0) * (1 - Math.exp(-t / 4000)));
}

// training slows with Gaussian count: cost of iteration i is 0.3 + g(i)/G1; integrated in steps
const STEP = 100;
const table = [];
function trainCost(u) {
  if (!table.length) {
    let c = 0;
    for (let i = 0; i <= 30000; i += STEP) {
      table.push(c);
      c += STEP * (0.3 + gaussians(i) / G1);
    }
  }
  const k = Math.min(table.length - 1, Math.floor(u / STEP));
  const frac = (u - k * STEP) / STEP;
  const next = table[Math.min(table.length - 1, k + 1)];
  return table[k] + (next - table[k]) * frac;
}

// map rounds take 4, 9, 10, 9 s natively; cost per round
const ROUNDS = [4, 9, 10, 9];
const mapCost = (u) => {
  const k = Math.floor(u);
  return ROUNDS.slice(0, k).reduce((a, b) => a + b, 0) + (k < 4 ? ROUNDS[k] * (u - k) : 0);
};

export const PROFILES = {
  ingest: {
    title: 'Index and decode',
    unit: 'bytes',
    total: 31752639,
    step: 65536,
    seconds: 20,
    overhead: { load: 1 },
    outBytes: 1 << 20,
  },
  select: {
    title: 'Select frames',
    unit: 'frames',
    total: 201,
    step: 1,
    seconds: 25,
    overhead: {},
    outBytes: 2 << 20,
  },
  features: {
    title: 'Features',
    unit: 'frames',
    total: 201,
    step: 1,
    seconds: 30,
    overhead: { compile: 2 },
    outBytes: 2 << 20,
  },
  match: {
    title: 'Pairs and matching',
    unit: 'pairs',
    total: 3175,
    step: 5,
    seconds: 70,
    overhead: { compile: 1 },
    outBytes: 1 << 20,
  },
  map: {
    title: 'Mapping',
    unit: 'BA rounds',
    total: 4,
    step: 0.05,
    seconds: 40,
    overhead: {},
    outBytes: 1 << 20,
    cumCost: 'map',
    uncertain: true,
  },
  prepare: {
    title: 'Scene preparation',
    unit: 'frames',
    total: 201,
    step: 1,
    seconds: 15,
    overhead: {},
    outBytes: 4 << 20,
  },
  train: {
    title: 'Training',
    unit: 'iterations',
    total: 30000,
    step: 10,
    seconds: 1200,
    overhead: { compile: 3, eval: 5 },
    outBytes: 4 << 20,
    cumCost: 'train',
    checkpointEvery: 5000,
    gpu: true,
  },
  export: {
    title: 'Export',
    unit: 'files',
    total: 3,
    step: 1,
    seconds: 10,
    overhead: {},
    outBytes: 1 << 20,
  },
};

export const COSTS = { train: trainCost, map: mapCost };
export const DESKTOP_FACTOR = 0.6;

export function cumCostOf(profile) {
  return COSTS[profile.cumCost] ?? ((u) => u);
}
