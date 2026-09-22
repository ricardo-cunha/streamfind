import { chromium } from 'playwright';

const url = process.env.STREAMFIND_FRONTEND_URL || 'http://127.0.0.1:5173/';
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
const errors = [];

page.on('pageerror', (error) => errors.push(error.message));
page.on('console', (message) => {
  if (message.type() === 'error') errors.push(message.text());
});

try {
  await page.goto(url, { waitUntil: 'networkidle' });
  await page.waitForTimeout(3_300);

  await page.getByText('Create project', { exact: true }).waitFor();
  await page.getByText('Open project', { exact: true }).waitFor();
  await page.getByRole('main').waitFor();

  await page.getByRole('button', { name: 'Open appearance settings' }).click();
  await page.getByRole('heading', { name: 'Settings' }).waitFor();
  await page.getByRole('button', { name: 'Close settings' }).click();

  await page.getByRole('button', { name: 'Notifications' }).click();
  await page.getByRole('heading', { name: 'Notifications' }).waitFor();
  await page.getByRole('button', { name: 'Close notifications' }).click();

  await page.getByRole('button', { name: /Backend / }).click();
  await page.getByRole('heading', { name: 'Backend' }).waitFor();
  await page.getByRole('button', { name: 'Close backend details' }).click();

  if (errors.length > 0) throw new Error(`Browser console errors: ${errors.join(' | ')}`);
  console.log(`Browser smoke passed: ${url}`);
} finally {
  await browser.close();
}
