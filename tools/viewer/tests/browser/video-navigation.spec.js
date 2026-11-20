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

// Target a raw detector pixel through the actual scrolled canvas, then use a
// physical pointer event (not a direct call to the inspection implementation).
async function hoverRaw(page, x, y, rotation = 0) {
  await page.locator('#video-stage').scrollIntoViewIfNeeded();
  const point = await page.evaluate(
    ({ x, y, rotation }) => {
      const canvas = document.querySelector('#video-canvas'),
        stage = document.querySelector('#video-stage'),
        d = window.fireViewer.state.decoded;
      const sx = rotation ? d.width - 1 - x : x,
        sy = rotation ? d.height - 1 - y : y;
      let rect = canvas.getBoundingClientRect(),
        viewport = stage.getBoundingClientRect();
      stage.scrollLeft +=
        rect.left + ((sx + 0.5) * rect.width) / d.width - viewport.left - stage.clientWidth / 2;
      stage.scrollTop +=
        rect.top + ((sy + 0.5) * rect.height) / d.height - viewport.top - stage.clientHeight / 2;
      rect = canvas.getBoundingClientRect();
      return {
        x: rect.left + ((sx + 0.5) * rect.width) / d.width,
        y: rect.top + ((sy + 0.5) * rect.height) / d.height,
      };
    },
    { x, y, rotation },
  );
  await page.mouse.move(point.x, point.y);
  await expect(page.locator('#hover-reading')).toContainText(`Raw pixel ${x}, ${y}`);
}

test('video magnification scrolls while raw hover, rotation and findings retain detector coordinates', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  const backing = await page.locator('#video-canvas').evaluate((c) => [c.width, c.height]);
  await expect(page.locator('#video-zoom')).toHaveValue('fit');
  for (const zoom of ['1', '2', '4', '8']) {
    await page.locator('#video-zoom').selectOption(zoom);
    const displayedSize = () =>
      page.locator('#video-canvas').evaluate((c) => {
        const rect = c.getBoundingClientRect();
        return [rect.width, rect.height];
      });
    await expect.poll(displayedSize).toEqual(backing.map((size) => size * Number(zoom)));
    if (zoom === '1') {
      await page.setViewportSize({ width: 1000, height: 800 });
      await expect.poll(displayedSize).toEqual(backing);
      await page.setViewportSize({ width: 1500, height: 1000 });
      await expect.poll(displayedSize).toEqual(backing);
    } else {
      await expect
        .poll(() =>
          page
            .locator('#video-stage')
            .evaluate((s) => s.scrollWidth > s.clientWidth && s.scrollHeight > s.clientHeight),
        )
        .toBe(true);
    }
    await hoverRaw(page, 500, 400);
    await expect(page.locator('#pixel-x')).toHaveValue('500');
    await expect(page.locator('#pixel-y')).toHaveValue('400');
    expect(await page.locator('#video-canvas').evaluate((c) => [c.width, c.height])).toEqual(
      backing,
    );
  }
  await page.locator('#rotation').selectOption('180');
  await hoverRaw(page, 500, 400, 180);
  await page.locator('#mark-finding').click();
  expect(await page.evaluate(() => window.fireViewer.state.findings[0].rawPixel)).toEqual({
    x: 500,
    y: 400,
  });
  await page.locator('#video-zoom').selectOption('fit');
  await expect
    .poll(() =>
      page
        .locator('#video-stage')
        .evaluate(
          (s) => s.scrollWidth <= s.clientWidth + 1 && s.scrollHeight <= s.clientHeight + 1,
        ),
    )
    .toBe(true);
  await page.locator('#video-zoom').selectOption('4');
  await hoverRaw(page, 320, 256, 180);
  await screenshot(page, testInfo, 'video-4x-scrolled.png');
});

test('typed frame jumps use displayed numbering, reject invalid targets and update for replacement files', async ({
  page,
}) => {
  await start(page);
  await expect(page.locator('#frame-input')).toBeDisabled();
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#frame-input').fill('7');
  await page.locator('#frame-input').press('Enter');
  await expect(page.locator('#frame-number')).toHaveText('Frame 7 / 12');
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(6);
  for (const value of ['0', '13', '1.5', '']) {
    await page.locator('#frame-input').fill(value);
    await page.locator('#frame-go').click();
    expect(await page.locator('#frame-input').evaluate((input) => input.checkValidity())).toBe(
      false,
    );
    expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(6);
  }
  await page.locator('#frame-input').fill('12');
  await page.locator('#frame-go').click();
  await expect(page.locator('#frame-number')).toHaveText('Frame 12 / 12');
  await page.locator('#frame-input').fill('1');
  await page.locator('#frame-input').press('Enter');
  await expect(page.locator('#frame-number')).toHaveText('Frame 1 / 12');
  await openFile(page, fixture('rgb.mp4'), { kind: 'rgba' });
  await waitIndexed(page);
  const count = await page.evaluate(() => window.fireViewer.state.frames.length);
  await expect(page.locator('#frame-input')).toHaveAttribute('max', String(count));
  await page.locator('#frame-input').fill(String(count));
  await page.locator('#frame-go').click();
  await expect.poll(() => page.evaluate(() => window.fireViewer.state.number)).toBe(count - 1);
});

test('video hover dot stays hidden after leaving, including frame and terrain refreshes', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 0, description: 'Hover test terrain' };
  });
  for (const [i, mode] of ['2d', '3d'].entries()) {
    await page.locator('#map-' + mode).click();
    await hoverRaw(page, 320, 256);
    expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(true);
    await page.locator('#play').hover();
    expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(false);
    await seekNumber(page, i + 1);
    expect(await page.evaluate(() => window.fireViewer.state.cursor)).toEqual({ x: 320, y: 256 });
    expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(false);
    await page.evaluate(() => window.fireViewer.map.terrain.onChange());
    await page.waitForTimeout(100);
    expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(false);
    await page.locator('#pixel-x').fill('100');
    expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(false);
  }
  await hoverRaw(page, 100, 256);
  expect(await page.evaluate(() => window.fireViewer.map.hover.show)).toBe(true);
});

test('map pointer shows sampled AMSL terrain height in both map modes and never uses a stale location', async ({
  page,
}, testInfo) => {
  await start(page);
  await page.evaluate(() => {
    const map = window.fireViewer.map;
    map.setFollow(false);
    map.focus(-35.322, 148.838, 3500);
    window.heightCalls = [];
    map.terrain.at = (lat, lon) => {
      window.heightCalls.push({ lat, lon });
      return 812.345;
    };
  });
  for (const mode of ['2d', '3d']) {
    await page.locator(`#map-${mode}`).click();
    const rect = await page.locator('#map canvas').boundingBox();
    await page.mouse.move(rect.x + rect.width / 2, rect.y + rect.height / 2);
    await expect(page.locator('#map-terrain-height')).toHaveText('Terrain height: 812.3 m AMSL');
  }
  const rect = await page.locator('#map canvas').boundingBox();
  await page.evaluate(() => {
    const terrain = window.fireViewer.map.terrain;
    terrain.at = (lat, lon) => {
      window.loadingLocation = { lat, lon };
      terrain.lastIssue = { state: 'loading', reason: 'Loading terrain for this pixel…' };
      return null;
    };
  });
  await page.mouse.move(rect.x + rect.width / 3, rect.y + rect.height / 2);
  await expect(page.locator('#map-terrain-height')).toContainText('Loading terrain');
  // The first tile completes after moving to a different pointer location.
  await page.evaluate(() => {
    const map = window.fireViewer.map;
    window.pendingPoint = { ...map.pointerPosition };
    map.terrain.at = (lat, lon) => {
      window.heightCalls.push({ lat, lon });
      return 845.678;
    };
  });
  await page.mouse.move(rect.x + (rect.width * 2) / 3, rect.y + rect.height / 2);
  await expect(page.locator('#map-terrain-height')).toHaveText('Terrain height: 845.7 m AMSL');
  const currentLocation = await page.evaluate(() => window.heightCalls.at(-1));
  expect(currentLocation).not.toEqual(await page.evaluate(() => window.loadingLocation));
  await page.evaluate(() => window.fireViewer.map.terrain.onChange());
  expect(await page.evaluate(() => window.heightCalls.at(-1))).toEqual(currentLocation);
  await page.evaluate(() => {
    window.fireViewer.map.terrain.at = () => 0;
    window.fireViewer.map.terrain.onChange();
  });
  await expect(page.locator('#map-terrain-height')).toHaveText('Terrain height: 0.0 m AMSL');
  expect(
    await page.evaluate(() => window.fireViewer.map.pointerPosition.x > window.pendingPoint.x),
  ).toBe(true);
  await page.mouse.move(10, 10);
  await expect(page.locator('#map-terrain-height')).toContainText('move pointer');
  await page.evaluate(() => window.fireViewer.map.terrain.onChange());
  await expect(page.locator('#map-terrain-height')).toContainText('move pointer');
  await screenshot(page, testInfo, 'map-terrain-pointer.png');
});

test('a pointer sweep on a slow terrain link preserves cached video-pixel heights', async ({
  page,
}) => {
  await start(page);
  await page.evaluate(async () => {
    const map = window.fireViewer.map;
    map.setFollow(false);
    map.focus(-35.322, 148.838, 20_000_000);
    const tilingScheme = map.viewer.terrainProvider.tilingScheme,
      videoPosition = { latitude: (-35.322 * Math.PI) / 180, longitude: (148.838 * Math.PI) / 180 },
      videoTile = tilingScheme.positionToTileXY(videoPosition, 12);
    window.pointerTerrainRequests = 0;
    map.terrain.configure(
      {
        tilingScheme,
        availability: { computeMaximumLevelAtPosition: () => 12, isTileAvailable: () => true },
        requestTileGeometry: (x, y) => {
          if (x === videoTile.x && y === videoTile.y)
            return Promise.resolve({ interpolateHeight: () => 812 });
          window.pointerTerrainRequests++;
          return new Promise(() => {}); // Slow/stalled terrain downloads.
        },
      },
      { terrainUrl: '/test-terrain/', terrainDatum: 'amsl' },
    );
    map.terrain.at(-35.322, 148.838);
  });
  await page.waitForFunction(() => window.fireViewer.map.terrain.at(-35.322, 148.838) === 812);
  const sweep = await page.evaluate(() => {
    const map = window.fireViewer.map,
      canvas = map.viewer.scene.canvas,
      rect = canvas.getBoundingClientRect();
    for (let i = 0; i < 120; i++) {
      canvas.dispatchEvent(
        new PointerEvent('pointermove', {
          clientX: rect.left + 10 + ((rect.width - 20) * i) / 119,
          clientY: rect.top + rect.height / 2,
        }),
      );
      // Terrain completion and render callbacks must respect the same gate.
      map.terrain.onChange();
      map.refreshPointerHeight();
    }
    return {
      cacheSize: map.terrain.cache.size,
      videoHeight: map.terrain.at(-35.322, 148.838),
    };
  });
  expect(sweep).toEqual({ cacheSize: 1, videoHeight: 812 });
  await page.waitForFunction(() => window.pointerTerrainRequests === 1);
  expect(
    await page.evaluate(() => ({
      cacheSize: window.fireViewer.map.terrain.cache.size,
      videoHeight: window.fireViewer.map.terrain.at(-35.322, 148.838),
    })),
  ).toEqual({ cacheSize: 2, videoHeight: 812 });
  await page.evaluate(() => {
    const canvas = window.fireViewer.map.viewer.scene.canvas,
      rect = canvas.getBoundingClientRect();
    canvas.dispatchEvent(
      new PointerEvent('pointermove', {
        clientX: rect.left + rect.width / 3,
        clientY: rect.top + rect.height / 2,
      }),
    );
    canvas.dispatchEvent(new PointerEvent('pointerleave'));
  });
  await page.waitForTimeout(220);
  expect(await page.evaluate(() => window.pointerTerrainRequests)).toBe(1);
  await expect(page.locator('#map-terrain-height')).toContainText('move pointer');
});
