// CPU-profile the Brush worker of a running harness Chrome (started with --remote-debugging-port)
// and print the functions with the most self time.
// usage: node profile_worker.mjs PORT SECONDS OUT.cpuprofile
const [port, seconds, out] = [process.argv[2], +process.argv[3], process.argv[4]];
const { writeFileSync } = await import('node:fs');
const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const ws = new WebSocket(version.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
  }
});
const send = (method, params = {}, sessionId) => new Promise((resolve) => {
  const msg = { id: ++id, method, params };
  if (sessionId) msg.sessionId = sessionId;
  pending.set(msg.id, resolve);
  ws.send(JSON.stringify(msg));
});
const { result } = await send('Target.getTargets');
const worker = result.targetInfos.find((t) => t.type === 'worker');
if (!worker) throw new Error('no worker target: ' + result.targetInfos.map((t) => t.type).join(','));
const { result: { sessionId } } = await send('Target.attachToTarget', { targetId: worker.targetId, flatten: true });
await send('Profiler.enable', {}, sessionId);
await send('Profiler.setSamplingInterval', { interval: 1000 }, sessionId);
await send('Profiler.start', {}, sessionId);
await new Promise((r) => setTimeout(r, seconds * 1000));
const { result: { profile } } = await send('Profiler.stop', {}, sessionId);
writeFileSync(out, JSON.stringify(profile));
const self = new Map();
const total = profile.samples.length;
const byId = new Map(profile.nodes.map((n) => [n.id, n]));
const counts = new Map();
for (const s of profile.samples) counts.set(s, (counts.get(s) || 0) + 1);
for (const [nid, c] of counts) {
  const f = byId.get(nid).callFrame;
  const name = (f.functionName || '(anon)') + ' ' + (f.url.split('/').pop() || '');
  self.set(name, (self.get(name) || 0) + c);
}
for (const [name, c] of [...self].sort((a, b) => b[1] - a[1]).slice(0, 25)) {
  console.log((100 * c / total).toFixed(1).padStart(5) + '%  ' + name.slice(0, 160));
}
ws.close();
