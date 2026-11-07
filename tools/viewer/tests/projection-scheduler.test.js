import test from 'node:test';
import assert from 'node:assert/strict';
import { ProjectionScheduler } from '../src/ui/projection-scheduler.js';

function clock() {
  let time = 0,
    next = 0;
  const tasks = new Map(),
    paints = [];
  const setTimer = (fn, delay) => {
    const id = ++next;
    tasks.set(id, { at: time + delay, fn });
    return id;
  };
  const clearTimer = (id) => tasks.delete(id);
  return {
    now: () => time,
    setTimer,
    clearTimer,
    requestPaint: (fn) =>
      setTimer(() => {
        paints.push(time);
        fn();
      }, 16),
    cancelPaint: clearTimer,
    advance(ms) {
      const until = time + ms;
      while (true) {
        const pending = [...tasks]
          .filter(([, t]) => t.at <= until)
          .sort((a, b) => a[1].at - b[1].at)[0];
        if (!pending) break;
        const [id, t] = pending;
        tasks.delete(id);
        time = t.at;
        t.fn();
      }
      time = until;
    },
    tasks,
    paints,
  };
}

test('slow playback drops intermediate projection requests and uses the latest frame after a paint', () => {
  const c = clock(),
    seen = [];
  let newest = 0;
  const scheduler = new ProjectionScheduler((playing, complete) => {
    seen.push({ frame: newest, time: c.now(), playing });
    if (!playing) {
      complete();
      return;
    }
    const id = c.setTimer(complete, 100);
    return () => c.clearTimer(id);
  }, c);
  for (let i = 0; i < 100; i++) {
    newest = i;
    scheduler.request(true);
    c.advance(10);
  }
  assert.equal(seen.length, 2, '100 requests become two slow projections, not queued images');
  assert.equal(seen[0].time, c.paints[0]);
  assert.equal(seen[0].frame, 1, 'the frame requested before the paint was superseded');
  assert.ok(seen[1].frame >= 50, 'next render uses the current frame');
  assert.equal(scheduler.interval, 500, 'render completion time is included in the budget');
  newest = 999;
  scheduler.request(false);
  assert.equal(seen.at(-1).frame, 999, 'pause catches up synchronously');
  assert.equal(seen.at(-1).playing, false);
  c.advance(2000);
  assert.equal(seen.length, 3, 'no stale scheduled render survives pausing');
});

test('cheap projections are capped at five fps and cancellation covers timers, paints and active draws', () => {
  const c = clock(),
    seen = [];
  const scheduler = new ProjectionScheduler((playing, complete) => {
    seen.push(c.now());
    const id = c.setTimer(complete, 1);
    return () => c.clearTimer(id);
  }, c);
  scheduler.request(true);
  c.advance(0);
  scheduler.reset();
  c.advance(100);
  assert.equal(seen.length, 0, 'reset cancels a pending paint');
  for (let i = 0; i < 100; i++) {
    scheduler.request(true);
    c.advance(10);
  }
  assert.equal(seen.length, 5);
  assert.ok(seen.slice(1).every((time, i) => time - seen[i] >= 200));
  scheduler.reset();
  scheduler.request(true);
  c.advance(16);
  const count = seen.length;
  scheduler.destroy();
  c.advance(2000);
  scheduler.request(true);
  c.advance(100);
  assert.equal(seen.length, count, 'destroy cancels active completion and ignores later requests');
  assert.equal(c.tasks.size, 0);
});
