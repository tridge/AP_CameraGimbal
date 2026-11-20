import { test, expect, fixture, start, openFile, waitIndexed } from './helpers.js';

test('software thermal decoding survives repeated playback with bit-exact pixels', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  const result = await page.evaluate(async () => {
    const state = window.fireViewer.state;
    const frame = state.frame;
    const digest = async (data) =>
      [...new Uint8Array(await crypto.subtle.digest('SHA-256', data))].join(',');
    const expected = await digest(state.decoded.data);
    let worker = state.decoder.software._worker;
    let replacements = 0;
    for (let i = 0; i < 256; i++) {
      const decoded = await state.decoder.decode(frame);
      if (decoded.kind !== 'gray16' || (await digest(decoded.data)) !== expected)
        throw new Error(`Raw pixels changed at decode ${i}`);
      if (state.decoder.software._worker !== worker) {
        replacements++;
        worker = state.decoder.software._worker;
      }
    }
    return { replacements };
  });
  expect(result.replacements).toBeGreaterThanOrEqual(8);
});
