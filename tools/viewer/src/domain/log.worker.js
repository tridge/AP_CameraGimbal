import { parseFlightLog } from './flight-log.js';
self.onmessage = async ({ data }) => {
  try {
    const log = await parseFlightLog(data.file, {
      onProgress: (progress) => self.postMessage({ type: 'progress', progress }),
    });
    self.postMessage({ type: 'log', log });
  } catch (error) {
    self.postMessage({ type: 'error', message: error.message });
  }
};
