// Channel labels: persisted via /labels and rendered in the status-page grid.
// Restores the device's original labels afterwards.
import { test, expect } from '@playwright/test';

let original = '{}';

test.beforeAll(async ({ request }) => {
  original = JSON.stringify(await (await request.get('/labels.json')).json());
});
test.afterAll(async ({ request }) => {
  await request.post('/labels', {
    data: original, headers: { 'Content-Type': 'application/json' },
  });
});

test('labels round-trip through /labels', async ({ request }) => {
  const labels = { 1: 'Front Wash L', 5: 'Haze' };
  const res = await request.post('/labels', {
    data: JSON.stringify(labels), headers: { 'Content-Type': 'application/json' },
  });
  expect(res.ok()).toBeTruthy();
  const back = await (await request.get('/labels.json')).json();
  expect(back['1']).toBe('Front Wash L');
  expect(back['5']).toBe('Haze');
});

test('labels render in the status-page grid', async ({ page, request }) => {
  await request.post('/labels', {
    data: JSON.stringify({ 1: 'Front Wash L' }),
    headers: { 'Content-Type': 'application/json' },
  });
  await page.goto('/');
  await expect(page.locator('#l1')).toHaveText('Front Wash L');
  await expect(page.locator('#ch1')).toHaveClass(/labeled/);
});

test('per-output (nested) labels render for the viewed output', async ({ page, request }) => {
  // New format: { "<outputIdx>": { "<ch>": name } }. Output 0 is the default view.
  await request.post('/labels', {
    data: JSON.stringify({ 0: { 3: 'Nested A' }, 1: { 3: 'Nested B' } }),
    headers: { 'Content-Type': 'application/json' },
  });
  await page.goto('/');
  await expect(page.locator('#l3')).toHaveText('Nested A');  // viewing Output A
});

test('invalid labels payload is rejected', async ({ request }) => {
  const res = await request.post('/labels', {
    data: 'not json', headers: { 'Content-Type': 'application/json' },
  });
  expect(res.status()).toBe(400);
});
