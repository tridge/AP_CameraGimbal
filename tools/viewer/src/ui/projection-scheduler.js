/** Keep only a request for the latest frame, never a queue of old images.
 * Playback yields a paint before projecting, with at most five updates/second
 * and an adaptive 20% rendering target. Paused requests bypass the rate limit.
 */
export class ProjectionScheduler {
  constructor(
    render,
    {
      now = () => performance.now(),
      setTimer = (fn, ms) => setTimeout(fn, ms),
      clearTimer = (id) => clearTimeout(id),
      requestPaint = (fn) => requestAnimationFrame(fn),
      cancelPaint = (id) => cancelAnimationFrame(id),
      onError = (error) => {
        throw error;
      },
    } = {},
  ) {
    Object.assign(this, { render, now, setTimer, clearTimer, requestPaint, cancelPaint, onError });
    this.timer = this.paint = null;
    this.epoch = 0;
    this.pending = this.active = this.closed = false;
    this.cost = this.nextDue = 0;
    this.interval = 200;
  }
  request(playing) {
    if (this.closed) return;
    this.playing = playing;
    if (!playing) this.cancel();
    this.pending = true;
    if (playing) this.schedule();
    else this.flush(false);
  }
  schedule() {
    if (!this.pending || this.active || this.timer !== null || this.paint !== null) return;
    this.timer = this.setTimer(
      () => {
        this.timer = null;
        if (this.now() < this.nextDue) {
          this.schedule();
          return;
        }
        this.paint = this.requestPaint(() => {
          this.paint = null;
          // Run after the video paint, rather than inside its animation callback.
          this.timer = this.setTimer(() => {
            this.timer = null;
            this.flush(true);
          }, 0);
        });
      },
      Math.max(0, this.nextDue - this.now()),
    );
  }
  flush(playing) {
    if (!this.pending || this.closed) return;
    this.pending = false;
    this.active = true;
    const epoch = ++this.epoch,
      started = this.now();
    const complete = () => {
      if (epoch !== this.epoch || !this.active) return;
      this.active = false;
      this.cancelRender = null;
      if (playing) {
        const elapsed = Math.max(0, this.now() - started);
        // React immediately to overload; recover more gradually after it clears.
        this.cost = Math.max(elapsed, this.cost * 0.8 + elapsed * 0.2);
        this.interval = Math.min(1000, Math.max(200, this.cost * 5));
      }
      this.nextDue = started + (playing ? this.interval : 200);
      if (this.pending && this.playing) this.schedule();
    };
    try {
      const cancel = this.render(playing, complete);
      if (this.active) this.cancelRender = cancel;
    } catch (error) {
      complete();
      this.onError(error);
    }
  }
  cancel() {
    this.epoch++;
    this.pending = this.active = false;
    if (this.timer !== null) this.clearTimer(this.timer);
    if (this.paint !== null) this.cancelPaint(this.paint);
    this.timer = this.paint = null;
    this.cancelRender?.();
    this.cancelRender = null;
  }
  reset() {
    this.cancel();
    this.cost = this.nextDue = 0;
    this.interval = 200;
  }
  destroy() {
    this.closed = true;
    this.cancel();
  }
}
