export class ThermalAnalysisClient {
  constructor(
    workerFactory = () =>
      new Worker(new URL('./thermal-analysis.worker.js', import.meta.url), { type: 'module' }),
  ) {
    this.workerFactory = workerFactory;
    this.sequence = 0;
    this.worker = null;
    this.pending = null;
  }
  cancel() {
    this.sequence++;
    clearTimeout(this.timer);
    this.worker?.terminate();
    this.worker = null;
    this.pending?.reject(new DOMException('Analysis cancelled.', 'AbortError'));
    this.pending = null;
  }
  run(pixels, width, height, settings, roi, identity) {
    this.cancel();
    if (typeof identity !== 'string')
      return Promise.reject(new Error('Analysis identity must be a string.'));
    const id = this.sequence;
    return new Promise((resolve, reject) => {
      try {
        const worker = (this.worker = this.workerFactory());
        this.pending = { reject };
        const finish = () => {
          clearTimeout(this.timer);
          worker.terminate();
          if (this.worker === worker) {
            this.worker = null;
            this.pending = null;
          }
        };
        this.timer = setTimeout(() => {
          if (id === this.sequence) {
            finish();
            reject(new Error('Analysis exceeded the 30-second work budget.'));
          }
        }, 30000);
        worker.onmessage = ({ data }) => {
          if (id !== this.sequence || data.id !== id || data.identity !== identity) return;
          finish();
          if (data.error) reject(new Error(data.error));
          else resolve(data.result);
        };
        worker.onerror = (event) => {
          event.preventDefault?.();
          if (id !== this.sequence) return;
          finish();
          reject(new Error(event.message || 'Thermal analysis worker failed.'));
        };
        // Decoder-owned storage is never transferred or mutated.
        const copy = pixels.slice();
        worker.postMessage({ id, identity, pixels: copy, width, height, settings, roi }, [
          copy.buffer,
        ]);
      } catch (e) {
        clearTimeout(this.timer);
        this.worker?.terminate();
        this.worker = null;
        this.pending = null;
        reject(e);
      }
    });
  }
}
