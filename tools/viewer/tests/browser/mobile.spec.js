import { test, expect, fixture, start, openFile, screenshot } from './helpers.js';

test('mobile review fits 390 px and deployed About notices load through local GETs', async ({
  page,
}, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  const width = await page.evaluate(() => ({
    viewport: innerWidth,
    document: document.documentElement.scrollWidth,
    body: document.body.scrollWidth,
  }));
  expect(width.viewport).toBe(390);
  expect(width.document).toBeLessThanOrEqual(width.viewport);
  expect(width.body).toBeLessThanOrEqual(width.viewport);
  // the map controls wrap inside their panel, Follow still directly above Project video
  const controls = await page.evaluate(() => {
    const box = (q) => document.querySelector(q).getBoundingClientRect();
    return {
      panel: box('.map-panel'),
      follow: box('#map-follow'),
      project: box('#map-project-video'),
      map: box('#map'),
    };
  });
  for (const key of ['follow', 'project']) {
    expect(controls[key].left).toBeGreaterThanOrEqual(controls.panel.left);
    expect(controls[key].right).toBeLessThanOrEqual(controls.panel.right);
    expect(controls[key].bottom).toBeLessThanOrEqual(controls.map.top);
  }
  expect(controls.follow.bottom).toBeLessThanOrEqual(controls.project.top);
  expect(Math.abs(controls.follow.left - controls.project.left)).toBeLessThan(1);
  await screenshot(page, testInfo, 'mobile-thermal-review.png');
  const link = page.getByRole('link', { name: 'About / licences', exact: true });
  const licences = new URL('licenses/', page.url());
  await expect(link).toHaveAttribute('href', licences.pathname);
  await expect(link).toHaveAttribute('rel', 'noopener');
  const pending = page.waitForEvent('popup');
  await link.click();
  const about = await pending;
  await about.waitForLoadState();
  await expect(about).toHaveURL(licences.href);
  await expect(about.getByRole('heading', { level: 1 })).toContainText('third-party notices');
  expect(await about.evaluate(() => window.opener === null)).toBe(true);
  // Retrieve the actual deployed link targets in the browser. Markdown/plain
  // licence links can trigger downloads when clicked, so inspect their GET
  // responses without navigating away from the notice page.
  const resources = await about.evaluate(async () => {
    const names = [
      'THIRD_PARTY.md',
      'dependency-manifest.json',
      'GPL-2.0.txt',
      'Cesium-LICENSE.md',
    ];
    return Promise.all(
      names.map(async (name) => {
        const link = [...document.querySelectorAll('a')].find(
          (a) => a.getAttribute('href') === name,
        );
        if (!link) throw new Error('Missing deployed licence link ' + name);
        const response = await fetch(link.href, { credentials: 'omit' });
        return { name, url: response.url, status: response.status, text: await response.text() };
      }),
    );
  });
  for (const resource of resources) {
    expect(resource.status, resource.name).toBe(200);
    expect(new URL(resource.url).origin).toBe(new URL(page.url()).origin);
    expect(resource.text.length).toBeGreaterThan(100);
  }
  const text = (name) => resources.find((r) => r.name === name).text;
  expect(text('THIRD_PARTY.md')).toContain('Dependencies and source references');
  expect(text('GPL-2.0.txt')).toContain('GNU GENERAL PUBLIC LICENSE');
  expect(text('GPL-2.0.txt')).toContain('Version 2, June 1991');
  expect(text('Cesium-LICENSE.md')).toContain('Apache License');
  const manifest = JSON.parse(text('dependency-manifest.json'));
  for (const [name, version] of [
    ['@ffmpeg/core', '0.12.10'],
    ['cesium', '1.146.0'],
  ]) {
    const entry = manifest.dependencies.find((d) => d.name === name);
    expect(entry.version).toBe(version);
    expect(entry.integrity).toMatch(/^sha512-/);
  }
  await screenshot(about, testInfo, 'mobile-about-licences.png');
});
