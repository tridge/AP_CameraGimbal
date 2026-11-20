/** CSS-only magnification keeps the backing image and raw-pixel coordinates intact. */
export class VideoViewport {
  constructor(stage, canvas) {
    this.stage = stage;
    this.canvas = canvas;
    this.zoom = 'fit';
    this.width = this.height = 0;
    this.observer = new ResizeObserver(() => this.layout());
    this.observer.observe(stage);
  }
  setFrame(width, height) {
    if (width === this.width && height === this.height) return;
    this.width = width;
    this.height = height;
    this.layout();
  }
  setZoom(value) {
    const zoom = value === 'fit' ? 'fit' : Number(value);
    if (!['fit', 1, 2, 4, 8].includes(zoom)) return;
    const stage = this.stage,
      rect = this.canvas.getBoundingClientRect(),
      viewport = stage.getBoundingClientRect();
    // Preserve the point at the centre of the visible area when changing zoom.
    const centre =
      rect.width && rect.height
        ? [
            (viewport.left + stage.clientWidth / 2 - rect.left) / rect.width,
            (viewport.top + stage.clientHeight / 2 - rect.top) / rect.height,
          ]
        : [0.5, 0.5];
    this.zoom = zoom;
    this.layout(centre);
  }
  layout(centre = null) {
    if (!this.width || !this.height) return;
    const stage = this.stage;
    // Reserve the scrollbar gutters even at Fit, avoiding a resize/scrollbar loop.
    const scale =
        this.zoom === 'fit'
          ? Math.min(stage.clientWidth / this.width, stage.clientHeight / this.height)
          : this.zoom,
      width = this.width * scale,
      height = this.height * scale,
      left = Math.max(0, (stage.clientWidth - width) / 2),
      top = Math.max(0, (stage.clientHeight - height) / 2);
    Object.assign(this.canvas.style, {
      width: `${width}px`,
      height: `${height}px`,
      left: `${left}px`,
      top: `${top}px`,
    });
    if (centre) {
      stage.scrollLeft = left + centre[0] * width - stage.clientWidth / 2;
      stage.scrollTop = top + centre[1] * height - stage.clientHeight / 2;
    }
  }
  reset() {
    this.width = this.height = 0;
    this.stage.scrollLeft = this.stage.scrollTop = 0;
  }
  close() {
    this.observer.disconnect();
  }
}
