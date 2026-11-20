import {
  test,
  expect,
  fixture,
  start,
  openFile,
  waitIndexed,
  seekNumber,
  screenshot,
} from './helpers.js';

async function flatTerrain(page) {
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 0, description: 'Test terrain' };
  });
}

async function projectedSamples(page, coordinates) {
  return page.evaluate(async (coordinates) => {
    const { map, state, pose, elevation } = window.fireViewer,
      scene = map.viewer.scene;
    await new Promise((r) => setTimeout(r, 100));
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        remove();
        reject(new Error('Projection scene did not render within 5 seconds'));
      }, 5000);
      const remove = scene.postRender.addEventListener(() => {
        remove();
        clearTimeout(timeout);
        const gl = scene.context._gl;
        resolve(
          coordinates.map(([u, v]) => {
            const point = elevation.ground(
                pose,
                u * state.image.width - 0.5,
                v * state.image.height - 0.5,
                state.image.width,
                state.image.height,
              ),
              screen = scene.cartesianToCanvasCoordinates(
                map.position(point.lat, point.lon, point.altMsl + 0.5),
              ),
              pixel = new Uint8Array(4);
            gl.readPixels(
              Math.round((screen.x * gl.drawingBufferWidth) / scene.canvas.clientWidth),
              Math.round(
                gl.drawingBufferHeight -
                  (screen.y * gl.drawingBufferHeight) / scene.canvas.clientHeight,
              ),
              1,
              1,
              gl.RGBA,
              gl.UNSIGNED_BYTE,
              pixel,
            );
            return Array.from(pixel).slice(0, 3);
          }),
        );
      });
      scene.requestRender();
    });
  }, coordinates);
}

test('Project video is off by default beside 2D/3D and draws correctly oriented image colors in both modes', async ({
  page,
}, testInfo) => {
  await start(page);
  const toggle = page.getByRole('checkbox', { name: 'Project video', exact: true });
  await expect(toggle).not.toBeChecked();
  expect(
    await toggle.evaluate((e) =>
      e.closest('.map-controls').contains(document.querySelector('#map-3d')),
    ),
  ).toBe(true);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await toggle.check();
  await expect(page.locator('#map-projection-note')).toContainText('unavailable');
  await flatTerrain(page);
  // Four distinct quadrants make texture orientation observable in real pixels.
  await page.evaluate(() => {
    const { state, map, elevation, pose } = window.fireViewer;
    const image = new OffscreenCanvas(state.decoded.width, state.decoded.height),
      context = image.getContext('2d');
    ['#ff0000', '#00ff00', '#0000ff', '#ffff00'].forEach((color, i) => {
      context.fillStyle = color;
      context.fillRect(
        ((i % 2) * image.width) / 2,
        (Math.floor(i / 2) * image.height) / 2,
        image.width / 2,
        image.height / 2,
      );
    });
    state.image = image;
    map.videoProjection.update(image, pose, elevation);
  });
  await page.waitForFunction(() => window.fireViewer.map.videoProjection.primitive?.ready);
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.focus(-35.3219444, 148.8380556, 2200));
    const colors = await projectedSamples(page, [
      [0.25, 0.25],
      [0.75, 0.25],
      [0.25, 0.75],
      [0.75, 0.75],
    ]);
    const expected = [
      [255, 0, 0],
      [0, 255, 0],
      [0, 0, 255],
      [255, 255, 0],
    ];
    colors.forEach((color, i) =>
      color.forEach((value, c) =>
        expect(Math.abs(value - expected[i][c]), `${mode} quadrant ${i} channel ${c}`).toBeLessThan(
          25,
        ),
      ),
    );
    await screenshot(page, testInfo, `projected-quadrants-${mode}.png`);
  }
  await page.evaluate(() => {
    const p = window.fireViewer.map.videoProjection;
    window.oldProjectionResources = [p.primitive, p.material, p.texture];
    const image = new OffscreenCanvas(p.image.width, p.image.height);
    image.getContext('2d').fillStyle = '#ff00ff';
    image.getContext('2d').fillRect(0, 0, image.width, image.height);
    window.fireViewer.state.image = image;
    p.update(image, window.fireViewer.pose, window.fireViewer.elevation);
  });
  const [updated] = await projectedSamples(page, [[0.25, 0.25]]);
  updated.forEach((value, channel) =>
    expect(Math.abs(value - [255, 0, 255][channel])).toBeLessThan(25),
  );
  expect(
    await page.evaluate(
      () => window.fireViewer.map.videoProjection.texture === window.oldProjectionResources[2],
    ),
  ).toBe(true);
  await toggle.uncheck();
  await expect(page.locator('#map-projection-note')).toBeHidden();
  expect(
    await page.evaluate(() => window.oldProjectionResources.every((p) => p.isDestroyed())),
  ).toBe(true);
});

test('projected video follows frame and palette changes, hides stale poses, and clears replaced files', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await flatTerrain(page);
  await page.locator('#map-project-video').check();
  await expect(page.locator('#map-projection-note')).toContainText('Video projected onto terrain.');
  await page.waitForFunction(() => window.fireViewer.map.videoProjection.primitive?.ready);
  await page.evaluate(() => {
    const p = window.fireViewer.map.videoProjection;
    window.originalProjection = { primitive: p.primitive, texture: p.texture, image: p.image };
  });
  await seekNumber(page, 5);
  await page.locator('#palette').selectOption('Inferno');
  expect(
    await page.evaluate(() => {
      const p = window.fireViewer.map.videoProjection;
      return (
        p.image === window.fireViewer.state.image &&
        p.image !== window.originalProjection.image &&
        p.texture === window.originalProjection.texture &&
        p.primitive === window.originalProjection.primitive
      );
    }),
  ).toBe(true);
  await page.locator('#rotation').selectOption('180');
  await page.locator('#video-zoom').selectOption('4');
  expect(
    await page.evaluate(
      () => window.fireViewer.map.videoProjection.primitive === window.originalProjection.primitive,
    ),
  ).toBe(true);
  // An aligned pose change must rebuild the ground mesh without replacing the
  // image texture, even when the selected frame itself has not changed.
  await page.evaluate(() => {
    window.fireViewer.state.frame.telemetry.position.lat_e7 += 1000;
    window.fireViewer.state.dirtyPose = true;
  });
  await page.locator('#offset').fill('0.1');
  expect(await page.evaluate(() => window.originalProjection.primitive.isDestroyed())).toBe(true);
  expect(
    await page.evaluate(
      () => window.fireViewer.map.videoProjection.texture === window.originalProjection.texture,
    ),
  ).toBe(true);
  await page.locator('#offset').fill('0');
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = {
      at: () => null,
      lastIssue: { state: 'loading', reason: 'Loading test terrain' },
    };
  });
  await page.locator('#offset').fill('0.1');
  await expect(page.locator('#map-projection-note')).toContainText('Loading test terrain');
  expect(await page.evaluate(() => !window.fireViewer.map.videoProjection.primitive)).toBe(true);
  await flatTerrain(page);
  await page.locator('#offset').fill('0');
  await page.locator('#offset').fill('99999');
  await expect(page.locator('#map-projection-note')).toContainText('pose');
  expect(await page.evaluate(() => !window.fireViewer.map.videoProjection.primitive)).toBe(true);
  await page.locator('#offset').fill('0');
  await expect(page.locator('#map-projection-note')).toContainText('Video projected onto terrain.');
  await openFile(page, fixture('misb.ts'));
  await waitIndexed(page);
  expect(await page.evaluate(() => window.originalProjection.texture.isDestroyed())).toBe(true);
  expect(
    await page.evaluate(
      () => window.fireViewer.map.videoProjection.image === window.fireViewer.state.image,
    ),
  ).toBe(true);
});

test('projection drapes continuously when ray-sampling heights differ from the rendered terrain LOD', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await page.evaluate(() => {
    const { state, map, pose, elevation } = window.fireViewer;
    // CPU samples are below the rendered ellipsoid, as can happen between
    // terrain LODs. A separate flat triangle mesh disappears into the globe.
    elevation.terrain = {
      at: (lat, lon) => -40 + 15 * Math.sin(lat * 30000) * Math.cos(lon * 30000),
      description: 'Different terrain LOD',
    };
    const image = new OffscreenCanvas(state.image.width, state.image.height);
    const c = image.getContext('2d');
    c.fillStyle = '#ff00ff';
    c.fillRect(0, 0, image.width, image.height);
    state.image = image;
    map.videoProjection.setEnabled(true);
    map.videoProjection.update(image, pose, elevation);
    map.viewer.entities.show = false;
    map.clearTemperatureTrack();
  });
  await page.waitForFunction(() => window.fireViewer.map.videoProjection.primitive?.ready);
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.focus(-35.3219444, 148.8380556, 2200));
    const coordinates = [0.2, 0.35, 0.5, 0.65, 0.8].flatMap((u) =>
      [0.2, 0.35, 0.5, 0.65, 0.8].map((v) => [u, v]),
    );
    const colors = await projectedSamples(page, coordinates);
    for (const color of colors) {
      expect(color[0], mode + ' red').toBeGreaterThan(235);
      expect(color[1], mode + ' green').toBeLessThan(20);
      expect(color[2], mode + ' blue').toBeGreaterThan(235);
    }
  }
});

test('projection yaw stabilisation is adjustable without changing recorded poses or findings', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await flatTerrain(page);
  await page.locator('#map-project-video').check();
  const before = await page.evaluate(() =>
    JSON.stringify(window.fireViewer.state.frames.map((f) => f.telemetry)),
  );
  await page.locator('#settings-button').click();
  await expect(page.locator('#projection-yaw-seconds')).toHaveValue('2');
  await page.locator('#projection-yaw-seconds').selectOption('0');
  await page.locator('#apply-settings').click();
  await expect(page.locator('#settings-dialog')).not.toBeVisible();
  expect(await page.evaluate(() => window.fireViewer.state.projectionYawSeconds)).toBe(0);
  expect(
    await page.evaluate(() =>
      JSON.stringify(window.fireViewer.state.frames.map((f) => f.telemetry)),
    ),
  ).toBe(before);
});

test('local recording projects onto real quantized terrain in both map modes', async ({
  page,
}, testInfo) => {
  const recording = process.env.VIEWER_PROJECT_VIDEO,
    terrainUrl = process.env.VIEWER_PROJECT_TERRAIN_URL;
  test.skip(
    !recording || !terrainUrl,
    'Set VIEWER_PROJECT_VIDEO/TERRAIN_URL for real terrain projection.',
  );
  test.setTimeout(180000);
  await page.route('**/viewer-config.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `window.VIEWER_CONFIG=${JSON.stringify({ imagery: 'satellite', terrainUrl, terrainDatum: 'amsl', amslToEllipsoidMetres: 0 })};`,
    }),
  );
  await start(page);
  await page.waitForFunction(
    (url) => window.fireViewer.map.configuration.terrainUrl === url,
    terrainUrl,
  );
  await openFile(page, recording);
  await waitIndexed(page);
  await seekNumber(page, Number(process.env.VIEWER_PROJECT_FRAME || 0));
  if (await page.evaluate(() => window.fireViewer.state.decoded.kind === 'gray16'))
    await page.locator('#palette').selectOption('Inferno');
  await page.locator('#map-follow').uncheck();
  await page.locator('#map-project-video').check();
  await page.waitForFunction(
    () =>
      window.fireViewer.map.videoProjection.mesh?.complete &&
      window.fireViewer.map.videoProjection.primitive?.ready,
    null,
    { timeout: 120000 },
  );
  const view = await page.evaluate(() => {
    const { map, elevation, pose, state } = window.fireViewer,
      center = elevation.ground(
        pose,
        (state.image.width - 1) / 2,
        (state.image.height - 1) / 2,
        state.image.width,
        state.image.height,
      ),
      vertices = map.videoProjection.mesh.vertices,
      north =
        (Math.max(...vertices.map((p) => p.lat)) - Math.min(...vertices.map((p) => p.lat))) *
        111000,
      east =
        (Math.max(...vertices.map((p) => p.lon)) - Math.min(...vertices.map((p) => p.lon))) *
        111000 *
        Math.cos((center.lat * Math.PI) / 180);
    return { center, distance: Math.max(150, Math.hypot(north, east) * 1.5) };
  });
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(
      ({ view, mode }) =>
        window.fireViewer.map.focus(
          view.center.lat,
          view.center.lon,
          view.distance + (mode === '3d' ? view.center.altMsl : 0),
        ),
      { view, mode },
    );
    await page.waitForTimeout(1500);
    // Real imagery has subpixel texture detail; compare the rendered layer
    // against the same view without it rather than one aliased source texel.
    const samples = [
      [0.25, 0.25],
      [0.75, 0.25],
      [0.4, 0.6],
      [0.6, 0.6],
      [0.75, 0.75],
    ];
    await page.evaluate(() => {
      window.fireViewer.map.videoProjection.primitive.show = false;
    });
    const background = await projectedSamples(page, samples);
    await page.evaluate(() => {
      window.fireViewer.map.videoProjection.primitive.show = true;
    });
    const actual = await projectedSamples(page, samples);
    const changed = actual.filter((color, i) =>
      color.some((value, c) => Math.abs(value - background[i][c]) > 20),
    );
    expect(
      changed.length,
      `${mode}: projected video visible on real terrain`,
    ).toBeGreaterThanOrEqual(4);
    await screenshot(page, testInfo, `video-projection-real-${mode}.png`);
  }
});
