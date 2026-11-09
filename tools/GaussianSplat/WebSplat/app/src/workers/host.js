// Main-thread side of a stage worker. Protocol (worker -> host):
//   {type:'progress', done, total?, extra?}  {type:'phase', phase, kind?}  {type:'log', level, text}
//   {type:'checkpoint-begin'}  {type:'checkpoint', id, generation, info}   (files already written)
//   {type:'result', value}  {type:'error', name, message, stack}
//   {type:'reply', id, value?, error?}                                     (answer to a command)
// host -> worker: {type:'start', ...msg}, {type:'stop'}, {type:'ack', id, error?},
//   {type:'command', id, name, data}   (opts.channel.request(name, data): a promise of the reply)
// The returned promise settles only after every pending handler (checkpoint publication) has
// finished, so nothing from this run is still in flight when the caller moves on.
export class StageAborted extends Error {
  name = 'AbortError';
}

export class WorkerError extends Error {
  constructor(name, message, stack) {
    super(message);
    this.name = name || 'WorkerError';
    this.workerStack = stack;
  }
}

// factory: () => new Worker(new URL('./x.worker.js', import.meta.url), {type: 'module'})
// handlers: { progress, phase, log, checkpointBegin, checkpoint (async, returns when published) }
export function runWorker(
  factory,
  msg,
  { signal, handlers = {}, transfer = [], stopGraceMs = 3000, channel = null } = {},
) {
  return new Promise((resolve, reject) => {
    let worker = null;
    let settled = false;
    const pending = new Set();
    const replies = new Map();
    let nextId = 1;
    if (channel)
      channel.request = (name, data) =>
        new Promise((res, rej) => {
          if (settled || !worker) return rej(new Error('the stage is not running'));
          const id = nextId++;
          replies.set(id, { res, rej });
          worker.postMessage({ type: 'command', id, name, data });
        });
    const finish = (fn, v) => {
      if (settled) return;
      settled = true;
      if (channel) channel.request = null;
      for (const r of replies.values()) r.rej(new Error('the stage ended'));
      replies.clear();
      signal?.removeEventListener('abort', onAbort);
      worker?.terminate();
      Promise.allSettled([...pending]).then(() => fn(v));
    };
    const onAbort = () => {
      try {
        worker?.postMessage({ type: 'stop' });
      } catch {
        // already gone
      }
      setTimeout(() => finish(reject, new StageAborted('stopped')), stopGraceMs);
    };
    if (signal?.aborted) return finish(reject, new StageAborted('stopped'));
    signal?.addEventListener('abort', onAbort);
    try {
      worker = factory();
      worker.onerror = (e) => {
        e.preventDefault?.();
        finish(reject, new WorkerError('WorkerError', e.message || 'worker failed to load'));
      };
      worker.onmessageerror = () =>
        finish(reject, new WorkerError('WorkerError', 'unreadable worker message'));
      worker.onmessage = ({ data: m }) => {
        if (settled) return;
        try {
          switch (m.type) {
            case 'progress':
              return handlers.progress?.(m.done, m.total, m.extra);
            case 'phase':
              return handlers.phase?.(m.phase, m.kind);
            case 'log':
              return handlers.log?.(m.level, m.text);
            case 'checkpoint-begin':
              return handlers.checkpointBegin?.();
            case 'checkpoint': {
              const p = Promise.resolve(handlers.checkpoint?.(m.generation, m.info))
                .then(() => !settled && worker.postMessage({ type: 'ack', id: m.id }))
                .catch((e) => finish(reject, e));
              pending.add(p);
              p.finally(() => pending.delete(p));
              return;
            }
            case 'reply': {
              const r = replies.get(m.id);
              replies.delete(m.id);
              if (m.error) r?.rej(new Error(m.error));
              else r?.res(m.value);
              return;
            }
            case 'result':
              return finish(resolve, m.value);
            case 'error':
              return finish(
                reject,
                m.name === 'AbortError'
                  ? new StageAborted(m.message)
                  : new WorkerError(m.name, m.message, m.stack),
              );
          }
        } catch (e) {
          finish(reject, e);
        }
      };
      worker.postMessage({ type: 'start', ...msg }, transfer);
    } catch (e) {
      // e.g. an uncloneable message or a worker that cannot be constructed
      finish(reject, e);
    }
  });
}
