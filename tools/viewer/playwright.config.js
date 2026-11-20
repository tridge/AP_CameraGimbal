import { defineConfig } from '@playwright/test';

// Browser qualification is deliberately visible inside the dedicated Xephyr
// display. Never fall back to the operator's desktop or to headless rendering.
const display = process.env.VIEWER_TEST_DISPLAY || ':99';
if (!/^:\d+(?:\.\d+)?$/.test(display) || /^:0(?:\.|$)/.test(display)) {
  throw new Error('VIEWER_TEST_DISPLAY must name the dedicated Xephyr display, not :0');
}
export default defineConfig({
  testDir: './tests/browser',
  testMatch: '**/*.spec.js',
  globalSetup: './tests/browser/setup.js',
  timeout: 90_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [['list']],
  outputDir: process.env.VIEWER_TEST_OUTPUT || './test-results/browser',
  use: {
    baseURL: process.env.VIEWER_TEST_URL || 'http://127.0.0.1:4173/viewer/',
    browserName: 'chromium',
    headless: false,
    viewport: { width: 1500, height: 1000 },
    deviceScaleFactor: 1,
    acceptDownloads: true,
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
    launchOptions: {
      executablePath: process.env.VIEWER_CHROME || '/opt/google/chrome/chrome',
      env: { ...process.env, DISPLAY: display },
      args: [
        '--no-sandbox',
        '--disable-setuid-sandbox',
        '--disable-dev-shm-usage',
        '--use-angle=swiftshader',
        '--enable-unsafe-swiftshader',
        '--disable-features=Vulkan',
      ],
    },
  },
});
