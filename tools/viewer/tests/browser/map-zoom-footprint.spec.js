import { test, expect, fixture, start, openFile, waitIndexed, seekNumber } from './helpers.js';

const scale = (page) =>
  page.evaluate(() => {
    const camera = window.fireViewer.map.viewer.camera;
    return camera.frustum.right !== undefined
      ? Math.max(
          camera.frustum.right - camera.frustum.left,
          camera.frustum.top - camera.frustum.bottom,
        )
      : camera.positionCartographic.height;
  });

test('map zoom buttons, ordinary and numpad keys work in 2D/3D, preserve Follow, and respect editable fields', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.focus(-35.3219444, 148.8380556, 3500));
    const before = await scale(page);
    await page.getByRole('button', { name: 'Zoom in', exact: true }).click();
    expect(await scale(page)).toBeCloseTo(before * 0.75, 1);
    await page.getByRole('button', { name: 'Zoom out', exact: true }).focus();
    await page.keyboard.press('Enter');
    expect(await scale(page)).toBeCloseTo(before, 1);
    await page.locator('#map').focus();
    await page.keyboard.press('=');
    const zoomed = await scale(page);
    expect(zoomed).toBeCloseTo(before * 0.75, 1);
    await seekNumber(page, 3);
    expect(await scale(page)).toBeCloseTo(zoomed, 1);
    await page.keyboard.press('-');
    expect(await scale(page)).toBeCloseTo(before, 1);
    await page.keyboard.press('Shift+Equal');
    expect(await scale(page)).toBeCloseTo(zoomed, 1);
    await page.keyboard.press('NumpadSubtract');
    expect(await scale(page)).toBeCloseTo(before, 1);
    await page.keyboard.press('NumpadAdd');
    expect(await scale(page)).toBeCloseTo(zoomed, 1);
    await page.getByRole('button', { name: 'Zoom out', exact: true }).click();
    expect(await scale(page)).toBeCloseTo(before, 1);
    await expect(page.locator('#map-follow')).toBeChecked();
    await page.locator('#seek').focus();
    await page.keyboard.press('=');
    expect(await scale(page)).toBeCloseTo(zoomed, 1);
    await page.keyboard.press('-');
    expect(await scale(page)).toBeCloseTo(before, 1);
    await page.locator('#offset').focus();
    await page.keyboard.press('-');
    expect(await scale(page)).toBeCloseTo(before, 1);
    const browserZoom = await page.evaluate(() => {
      const event = new KeyboardEvent('keydown', {
        key: '+',
        code: 'Equal',
        ctrlKey: true,
        bubbles: true,
        cancelable: true,
      });
      document.body.dispatchEvent(event);
      return event.defaultPrevented;
    });
    expect(browserZoom).toBe(false);
    expect(await scale(page)).toBeCloseTo(before, 1);
  }
});

test('continuously moving partial footprints remain visible in rendered pixels in 2D and 3D', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  for (const mode of ['2d', '3d']) {
    const result = await page.evaluate(async (mode) => {
      const { map, state, elevation } = window.fireViewer;
      map.setFollow(false);
      map.setMode(mode);
      map.focus(-35.3219444, 148.8380556, 3500);
      elevation.terrain = { at: () => 1360, description: 'Test terrain' };
      const record = {
        ...state.frame.telemetry,
        hfov_deg: 70,
        gimbal_attitude: { roll_rad: 0, pitch_rad: 0, yaw_rad: 0, age_ms: 0 },
      };
      const update = (i) => {
        const next = {
          ...record,
          position: { ...record.position, lat_e7: record.position.lat_e7 + i * 2 },
        };
        map.update(next, elevation.footprint(next, 640, 512), i / 30);
      };
      update(0);
      await new Promise((r) => setTimeout(r, 500));
      const scene = map.viewer.scene,
        samples = [];
      const gl = scene.context._gl,
        pixels = new Uint8Array(gl.drawingBufferWidth * gl.drawingBufferHeight * 4);
      const remove = scene.postRender.addEventListener(() => {
        gl.readPixels(
          0,
          0,
          gl.drawingBufferWidth,
          gl.drawingBufferHeight,
          gl.RGBA,
          gl.UNSIGNED_BYTE,
          pixels,
        );
        let yellow = 0;
        for (let i = 0; i < pixels.length; i += 4)
          if (pixels[i] > 200 && pixels[i + 1] > 200 && pixels[i + 2] < 100) yellow++;
        samples.push({ yellow, show: map.footprint.show });
      });
      try {
        for (let i = 1; i <= 40; i++) {
          update(i);
          await new Promise((r) => setTimeout(r, 33));
        }
      } finally {
        remove();
      }
      return { samples, entities: map.footprintPieces.length };
    }, mode);
    expect(result.samples.length).toBeGreaterThan(15);
    expect(result.samples.every((s) => s.show)).toBe(true);
    expect(
      result.samples.filter((s) => s.yellow === 0),
      `${mode}: no blank footprint frames`,
    ).toEqual([]);
    expect(result.entities).toBe(1);
  }
});
