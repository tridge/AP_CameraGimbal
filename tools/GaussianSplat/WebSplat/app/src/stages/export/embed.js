// The result view: the GaussianSplat viewer (tools/GaussianSplat/viewer, the page served at
// plot.ardupilot.org/splat/) in a same-origin iframe, opened with the exported bytes by
// postMessage. The viewer is a page-level WebGL app, so it gets its own document.
//
//   const v = embedViewer(container, 'vendor/splat-viewer/index.html');
//   await v.open(files['splat.ply'], 'splat.ply');      // metadata from the PLY header
//   await v.open(files['splat.spz'], 'splat.spz', meta); // SPZ needs meta.json

export function embedViewer(container, viewerUrl) {
  const iframe = document.createElement('iframe');
  const url = new URL(viewerUrl, location.href);
  url.searchParams.set('embed', '1');
  iframe.src = url.href;
  iframe.title = 'Splat viewer';
  iframe.className = 'splat-viewer';
  const origin = url.origin;
  let pending = null;
  const ready = new Promise((resolve) => {
    const onMessage = (e) => {
      if (e.source !== iframe.contentWindow || e.origin !== origin) return;
      if (e.data?.type === 'apcg-splat-ready') resolve();
      else if (e.data?.type === 'apcg-splat-opened' && pending) {
        const p = pending;
        pending = null;
        if (e.data.error) p.reject(new Error(e.data.error));
        else p.resolve();
      }
    };
    window.addEventListener('message', onMessage);
  });
  container.append(iframe);
  return {
    iframe,
    ready,
    // bytes are copied, so the caller keeps its buffer
    async open(bytes, name, meta = null) {
      await ready;
      if (pending) throw new Error('the viewer is still opening a splat');
      const done = new Promise((resolve, reject) => (pending = { resolve, reject }));
      iframe.contentWindow.postMessage({ type: 'apcg-splat-open', bytes, name, meta }, origin);
      return done;
    },
  };
}
