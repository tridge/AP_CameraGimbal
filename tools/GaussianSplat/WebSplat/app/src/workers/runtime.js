// Worker side: serveStage(async (msg, api) => result). api.stopped / api.throwIfStopped()
// let long loops end cleanly; api.checkpoint(fn) writes then waits until the host published it.
// api.onCommand = async (name, data) => ({value, transfer}) answers the host's commands.
export function serveStage(body) {
  let stopped = false;
  let nextId = 1;
  const acks = new Map();
  const post = (m) => self.postMessage(m);
  const api = {
    onCommand: null,
    get stopped() {
      return stopped;
    },
    throwIfStopped() {
      if (stopped) {
        const e = new Error('stopped');
        e.name = 'AbortError';
        throw e;
      }
    },
    progress: (done, total, extra) => post({ type: 'progress', done, total, extra }),
    // kind: 'work' (counts towards throughput) or 'overhead'; default by label (pipeline/eta.js)
    phase: (phase, kind) => post({ type: 'phase', phase, kind }),
    log: (text, level = 'info') => post({ type: 'log', level, text }),
    // fn(generation) writes the checkpoint files into msg.ckptDir(generation); info goes in the manifest
    async checkpoint(generation, fn, info) {
      post({ type: 'checkpoint-begin' });
      await fn();
      const id = nextId++;
      const acked = new Promise((r) => acks.set(id, r));
      post({ type: 'checkpoint', id, generation, info });
      await acked;
    },
  };
  self.onmessage = async ({ data: m }) => {
    if (m.type === 'stop') stopped = true;
    else if (m.type === 'ack') (acks.get(m.id)?.(), acks.delete(m.id));
    else if (m.type === 'command') {
      try {
        if (!api.onCommand) throw new Error(`this stage has no command ${m.name}`);
        const { value, transfer = [] } = (await api.onCommand(m.name, m.data)) ?? {};
        self.postMessage({ type: 'reply', id: m.id, value }, transfer);
      } catch (e) {
        post({ type: 'reply', id: m.id, error: e?.message ?? String(e) });
      }
    } else if (m.type === 'start') {
      globalThis.websplatTestFault = m.env?.testFault ?? null; // tests only (?testFault=)
      try {
        post({ type: 'result', value: await body(m, api) });
      } catch (e) {
        post({ type: 'error', name: e?.name, message: e?.message ?? String(e), stack: e?.stack });
      }
    }
  };
}
