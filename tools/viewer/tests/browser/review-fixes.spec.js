import { test, expect, fixture, start, openFile, waitIndexed, seekNumber } from './helpers.js';

test('edited MP4 hides preroll while decoding from its preceding keyframe; AVC Matroska uses native decoding', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('trimmed.mp4'), { kind: 'rgba' });
  await waitIndexed(page);
  const edit = await page.evaluate(() => {
    const { state } = window.fireViewer;
    return {
      pts: state.frame.pts,
      count: state.frames.length,
      packets: state.source.frames.length,
      mode: state.decoder.mode,
      submitted: state.decoder.stats.submittedPackets,
    };
  });
  expect(edit.pts).toBeCloseTo(0, 8);
  expect(edit.count).toBe(10);
  expect(edit.packets).toBe(17);
  expect(edit.mode).toContain('WebCodecs');
  expect(edit.submitted).toBeGreaterThanOrEqual(8);
  await expect(page.locator('#duration')).toHaveText('00:01.000');
  await seekNumber(page, 9);
  expect(await page.evaluate(() => window.fireViewer.state.frame.pts)).toBeCloseTo(0.9, 8);
  await openFile(page, fixture('rgb.mkv'), { kind: 'rgba' });
  expect(await page.evaluate(() => window.fireViewer.state.decoder.mode)).toContain('WebCodecs');
  const profile = await page.evaluate(() => {
    const track = window.fireViewer.state.source.tracks[0];
    return {
      actual: track.codecString,
      expected:
        'avc1.' +
        [...track.codecPrivate.slice(1, 4)].map((b) => b.toString(16).padStart(2, '0')).join(''),
    };
  });
  expect(profile.actual).toBe(profile.expected);
});

test('mid-GOP capture opens at its first keyframe with navigable controls and original elapsed time', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('mid-gop.ts'), { kind: 'rgba' });
  await waitIndexed(page);
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(9);
  expect(
    await page.evaluate(() => window.fireViewer.state.frame.pts - window.fireViewer.state.origin),
  ).toBeCloseTo(0.9, 6);
  for (const id of ['seek', 'next', 'previous', 'play'])
    await expect(page.locator('#' + id)).toBeEnabled();
  await expect(page.locator('#error-banner')).toBeHidden();
  await page.locator('#next').click();
  await expect(page.locator('#frame-number')).toHaveText('Frame 11 / 19');
  await page.evaluate(() => window.fireViewer.showFrame(0));
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(9);
});

test('legacy edited-video reviews retain source frames and anchors when presentation time is rebased', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('trimmed.mp4'), { kind: 'rgba' });
  await waitIndexed(page);
  await page.evaluate(async () => {
    const { state, openVideo } = window.fireViewer,
      file = state.file;
    localStorage.setItem(
      `fire-review:${file.name}:${file.size}:${file.lastModified}`,
      JSON.stringify({
        findings: [
          {
            id: 'excluded',
            label: 'F001',
            videoSeconds: 0,
            originalPts: -0.7,
            rawPixel: { x: 20, y: 30 },
            temperatureC: null,
          },
          {
            id: 'visible',
            label: 'F002',
            videoSeconds: 1,
            originalPts: 0.3,
            rawPixel: { x: 20, y: 30 },
            temperatureC: null,
          },
        ],
        anchors: [
          { videoSeconds: 0, utcUs: 1787882912367000 },
          { videoSeconds: 1, utcUs: 1787882913367000 },
        ],
      }),
    );
    state.origin = 42; // another video's origin must not leak into a pending open
    await openVideo(file);
    const rotation = document.getElementById('rotation');
    rotation.value = '180';
    rotation.dispatchEvent(new Event('change'));
    if (state.origin !== null) throw new Error('Pending recording retained an old timeline origin');
    const saved = JSON.parse(
      localStorage.getItem(`fire-review:${file.name}:${file.size}:${file.lastModified}`),
    );
    if (saved.timelineOrigin !== undefined)
      throw new Error('Pending save overwrote the legacy clock');
  });
  await page.waitForFunction(
    () => window.fireViewer.state.frame && !window.fireViewer.state.decoding,
  );
  await expect(page.locator('#finding-list button').filter({ hasText: 'F001' })).toBeDisabled();
  await expect(page.locator('#finding-list')).toContainText('outside clip');
  const before = await page.evaluate(() => ({
    findings: window.fireViewer.state.findings,
    anchors: window.fireViewer.state.anchors,
  }));
  expect(before.findings[1].videoSeconds).toBe(0.3);
  expect(before.anchors[0].videoSeconds).toBeCloseTo(-0.7, 8);
  await page.evaluate(() => window.fireViewer.openVideo(window.fireViewer.state.file));
  await page.waitForFunction(
    () => window.fireViewer.state.frame && !window.fireViewer.state.decoding,
  );
  expect(
    await page.evaluate(() => ({
      findings: window.fireViewer.state.findings,
      anchors: window.fireViewer.state.anchors,
    })),
  ).toEqual(before);
});

test('thermal FOV takes precedence, partial footprints stay open, and datum changes redraw existing findings', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  const thermal = await page.evaluate(() => ({
    outer: window.fireViewer.state.frame.thermalMetadata.hfov_deg,
    nested: window.fireViewer.state.frame.telemetry.provenance.nested_hfov_deg,
    used: window.fireViewer.pose.hfov_deg,
  }));
  expect(thermal).toEqual({ outer: 24.2, nested: 60, used: 24.2 });
  await page.evaluate(async () => {
    const { state, elevation, showFrame } = window.fireViewer;
    elevation.terrain = { at: () => 900, description: 'Test terrain' };
    state.frames[0].telemetry.gimbal_attitude.pitch_rad = 0;
    await showFrame(0);
  });
  await expect(page.locator('#map-note')).toContainText('Partial footprint');
  const partial = await page.evaluate(() => {
    const { map } = window.fireViewer;
    const points = map.footprint.polyline.positions.getValue();
    return {
      visible: map.footprint.show,
      count: points.length,
      closed: points[0].equals(points.at(-1)),
    };
  });
  expect(partial.visible).toBe(true);
  expect(partial.count).toBeGreaterThan(2);
  expect(partial.closed).toBe(false);
  await page.evaluate(async () => {
    const { state, showFrame, markFinding } = window.fireViewer;
    state.frames[0].telemetry.gimbal_attitude.pitch_rad = -Math.PI / 2;
    await showFrame(0);
    await markFinding();
  });
  await page.locator('#settings-button').click();
  await page.locator('#geoid-offset').fill('30');
  await page.locator('#fov-override').fill('30');
  await page.locator('#apply-settings').click();
  expect(await page.evaluate(() => window.fireViewer.pose.hfov_deg)).toBe(30);
  const distance = await page.evaluate(() => {
    const { map, state } = window.fireViewer,
      g = state.findings[0].ground;
    const actual = map.findingEntities[0].position.getValue();
    const expected = map.position(g.lat, g.lon, g.altMsl + 2);
    return Math.hypot(actual.x - expected.x, actual.y - expected.y, actual.z - expected.z);
  });
  expect(distance).toBeLessThan(0.001);
});

test('old saved temperatures and cancelled/reselected anchor files cannot prevent review', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.evaluate(async () => {
    const { state, openVideo } = window.fireViewer,
      file = state.file;
    localStorage.setItem(
      `fire-review:${file.name}:${file.size}:${file.lastModified}`,
      JSON.stringify({
        findings: [
          {
            id: 'old',
            label: 'F001',
            videoSeconds: 0,
            rawPixel: { x: 20, y: 30 },
            temperatureC: 'old',
            ground: null,
          },
        ],
      }),
    );
    await openVideo(file);
  });
  await page.waitForFunction(
    () => window.fireViewer.state.frame && !window.fireViewer.state.decoding,
  );
  await expect(page.locator('#finding-list')).toContainText('no pixel temperature');
  await page.locator('#anchors-button').click();
  await page.locator('#anchors-file').setInputFiles([]);
  await expect(page.locator('#error-banner')).toBeHidden();
  const anchor = {
    name: 'anchors.json',
    mimeType: 'application/json',
    buffer: Buffer.from('[{"videoSeconds":0,"utc":"2026-08-28T02:08:32Z"}]'),
  };
  await page.locator('#anchors-file').setInputFiles(anchor);
  await expect(page.locator('#anchors-json')).toHaveValue(/2026-08-28/);
  await page.locator('#anchors-json').fill('[]');
  await page.locator('#anchors-file').setInputFiles(anchor);
  await expect(page.locator('#anchors-json')).toHaveValue(/2026-08-28/);
});
