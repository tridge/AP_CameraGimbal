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

test.use({ deviceScaleFactor: 2 });

test('Follow starts checked below map and preserves2D zoom through seeking and toggles', async ({
  page,
}) => {
  await start(page);
  await expect(page.getByRole('checkbox', { name: 'Follow', exact: true })).toBeChecked();
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  const layout = await page.evaluate(() => ({
    map: document.querySelector('#map').getBoundingClientRect().bottom,
    follow: document.querySelector('#map-follow').getBoundingClientRect().top,
  }));
  expect(layout.follow).toBeGreaterThanOrEqual(layout.map);
  // Give the fixture a moving embedded position, keeping its original frame PTS.
  await page.evaluate(() => {
    const s = window.fireViewer.state;
    s.frames.forEach((f, i) => {
      f.telemetry = {
        ...f.telemetry,
        position: {
          ...f.telemetry.position,
          lat_e7: -353200000 + i * 1000,
          lon_e7: 1488400000 + i * 2000,
        },
      };
    });
    s.dirtyPose = true;
  });
  await seekNumber(page, 0);
  const before = await page.evaluate(() => ({
    height: window.fireViewer.map.viewer.camera.positionCartographic.height,
    heading: window.fireViewer.map.viewer.camera.heading,
  }));
  await seekNumber(page, 10);
  const moving = await page.evaluate(() => {
    const m = window.fireViewer.map,
      v = m.viewer,
      p = m.currentPosition,
      c = v.scene.cartesianToCanvasCoordinates(m.position(p.lat, p.lon, p.altMsl));
    return {
      height: v.camera.positionCartographic.height,
      heading: v.camera.heading,
      x: c.x,
      y: c.y,
      width: v.scene.canvas.clientWidth,
      heightPx: v.scene.canvas.clientHeight,
    };
  });
  expect(moving.height).toBeCloseTo(before.height, 5);
  expect(moving.heading).toBeCloseTo(before.heading, 5);
  expect(moving.x).toBeCloseTo(moving.width / 2, 1);
  expect(moving.y).toBeCloseTo(moving.heightPx / 2, 1);
  await page.getByRole('checkbox', { name: 'Follow', exact: true }).uncheck();
  const frozen = await page.evaluate(() =>
    Object.values(window.fireViewer.map.viewer.camera.positionWC),
  );
  await seekNumber(page, 2);
  expect(
    await page.evaluate(() => Object.values(window.fireViewer.map.viewer.camera.positionWC)),
  ).toEqual(frozen);
  await page.getByRole('checkbox', { name: 'Follow', exact: true }).check();
  expect(await page.evaluate(() => window.fireViewer.map.follow)).toBe(true);
  expect(
    await page.evaluate(() => window.fireViewer.map.viewer.camera.positionCartographic.height),
  ).toBeCloseTo(before.height, 5);
});

test('map clicks near the video track seek and pause in 2D and 3D, while off-track clicks do nothing', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await page.getByRole('checkbox', { name: 'Follow', exact: true }).uncheck();
  // A straight, well separated test route makes time-to-pixel checks unambiguous.
  await page.evaluate(() => {
    const { state, map } = window.fireViewer;
    state.frames.forEach((f, i) => {
      f.telemetry = {
        ...f.telemetry,
        position: {
          ...f.telemetry.position,
          lat_e7: -353200000 + i * 1500,
          lon_e7: 1488400000 + i * 3000,
        },
      };
    });
    state.dirtyPose = true;
  });
  await page.locator('#offset').fill('0.1');
  await page.locator('#offset').fill('0');
  await seekNumber(page, 0);
  await page.evaluate(() => {
    window.fireViewer.map.viewer.useBrowserRecommendedResolution = false;
    window.fireViewer.map.viewer.resolutionScale = 1.5;
    window.fireViewer.map.render();
  });
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.focus(-35.3192, 148.8416, 3500));
    await page.waitForTimeout(250);
    const target = await page.evaluate(() => {
      const { map, state } = window.fireViewer,
        point = map.videoTrack.find((p) => p.frameId === state.frames[8].id),
        screen = map.viewer.scene.cartesianToCanvasCoordinates(point.world);
      return { x: screen.x, y: screen.y };
    });
    const bounds = await page.locator('#map canvas').first().boundingBox();
    await page.mouse.click(bounds.x + target.x, bounds.y + target.y + 4);
    await page.waitForFunction(
      () => Math.abs(window.fireViewer.state.number - 8) <= 1 && !window.fireViewer.state.decoding,
    );
    await expect(page.locator('#play')).toHaveText('Play');
    await screenshot(page, testInfo, 'track-seek-' + mode + '.png');
    const number = await page.evaluate(() => window.fireViewer.state.number);
    await page.mouse.click(bounds.x + 20, bounds.y + 20);
    await page.waitForTimeout(100);
    expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(number);
    await seekNumber(page, 0);
  }
});

test('3D Follow transports camera pose without changing range and waits for a map drag', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await page.evaluate(() => {
    const s = window.fireViewer.state;
    s.frames.forEach(
      (f, i) =>
        (f.telemetry = {
          ...f.telemetry,
          position: {
            ...f.telemetry.position,
            lat_e7: -353200000 + i * 1000,
            lon_e7: 1488400000 + i * 2000,
          },
        }),
    );
    s.dirtyPose = true;
  });
  await seekNumber(page, 0);
  await page.locator('#map-3d').click();
  const before = await page.evaluate(() => {
    const m = window.fireViewer.map,
      c = m.viewer.camera;
    c.setView({
      destination: m.position(-35.33, 148.84, 4000),
      orientation: { heading: 0.65, pitch: -0.8, roll: 0.17 },
    });
    m.centerCurrent();
    const p = c.positionWC,
      a = m.followAnchor;
    return {
      heading: c.heading,
      pitch: c.pitch,
      roll: c.roll,
      range: Math.hypot(p.x - a.x, p.y - a.y, p.z - a.z),
    };
  });
  await seekNumber(page, 10);
  const after = await page.evaluate(() => {
    const m = window.fireViewer.map,
      c = m.viewer.camera,
      p = c.positionWC,
      a = m.followAnchor,
      v = m.viewer.scene;
    const s = v.cartesianToCanvasCoordinates(a);
    return {
      heading: c.heading,
      pitch: c.pitch,
      roll: c.roll,
      range: Math.hypot(p.x - a.x, p.y - a.y, p.z - a.z),
      x: s.x,
      y: s.y,
      width: v.canvas.clientWidth,
      height: v.canvas.clientHeight,
    };
  });
  expect(after.range).toBeCloseTo(before.range, 4);
  for (const key of ['heading', 'pitch', 'roll']) expect(after[key]).toBeCloseTo(before[key], 3);
  expect(after.x).toBeCloseTo(after.width / 2, 1);
  expect(after.y).toBeCloseTo(after.height / 2, 1);
  const bounds = await page.locator('#map canvas').first().boundingBox();
  await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
  await page.mouse.down();
  await page.mouse.move(bounds.x + bounds.width / 2 + 60, bounds.y + bounds.height / 2 + 25, {
    steps: 10,
  });
  expect(await page.evaluate(() => window.fireViewer.map.dragging)).toBe(true);
  const anchor = await page.evaluate(() => Object.values(window.fireViewer.map.followAnchor));
  await seekNumber(page, 7);
  expect(await page.evaluate(() => Object.values(window.fireViewer.map.followAnchor))).toEqual(
    anchor,
  );
  await page.mouse.up();
  await page.waitForTimeout(200);
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(7);
  expect(await page.evaluate(() => window.fireViewer.map.dragging)).toBe(false);
});

const realThermal = process.env.VIEWER_REAL_THERMAL,
  realBin = process.env.VIEWER_REAL_BIN;
test('real thermal video and BIN support Follow and map seeking on the recorded track', async ({
  page,
}, testInfo) => {
  test.skip(
    !realThermal || !realBin,
    'Set real thermal/BIN paths for map navigation qualification.',
  );
  test.setTimeout(180000);
  await start(page);
  await openFile(page, realThermal, { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#log-file').setInputFiles(realBin);
  await page.waitForFunction(() => !!window.fireViewer.state.log);
  await seekNumber(page, 8315);
  await page.getByRole('checkbox', { name: 'Follow', exact: true }).uncheck();
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.centerCurrent());
    await page.waitForTimeout(300);
    const target = await page.evaluate(() => {
      const { map, state } = window.fireViewer,
        s = map.viewer.scene;
      for (const p of map.videoTrack.filter(
        (p) => p.pts - state.frame.pts > 5 && p.pts - state.frame.pts < 25,
      )) {
        const q = s.cartesianToCanvasCoordinates(p.world);
        if (
          !q ||
          q.x < 20 ||
          q.y < 20 ||
          q.x > s.canvas.clientWidth - 20 ||
          q.y > s.canvas.clientHeight - 20
        )
          continue;
        const hit = map.pickVideoTrack({ x: q.x, y: q.y + 2 });
        if (!hit) continue;
        let index = 0;
        state.frames.forEach((f, i) => {
          if (Math.abs(f.pts - hit.pts) < Math.abs(state.frames[index].pts - hit.pts)) index = i;
        });
        return { x: q.x, y: q.y + 2, index };
      }
      return null;
    });
    expect(target).not.toBeNull();
    const b = await page.locator('#map canvas').first().boundingBox();
    await page.mouse.click(b.x + target.x, b.y + target.y);
    await page.waitForFunction(
      (index) =>
        Math.abs(window.fireViewer.state.number - index) <= 1 && !window.fireViewer.state.decoding,
      target.index,
    );
    await expect(page.locator('#play')).toHaveText('Play');
    await screenshot(page, testInfo, 'real-video-track-seek-' + mode + '.png');
    await seekNumber(page, 8315);
  }
});
