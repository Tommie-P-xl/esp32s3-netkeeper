// Run with playwright-cli run-code --filename tools/screenshots/web.js.
// The Python driver serves the actual main/index.html and fixed demo APIs locally.
async (page) => {
  await page.setViewportSize({width: 760, height: 1100});
  const backend = new URL(page.url()).origin;
  const demoOrigin = 'http://192.0.2.10';
  // Show a stable documentation address while every request stays on localhost.
  await page.route(demoOrigin + '/**', async route => {
    const url = new URL(route.request().url());
    const response = await route.fetch({url: backend + url.pathname + url.search});
    await route.fulfill({response});
  });
  await page.goto(demoOrigin + '/index.html');
  await page.evaluate(() => localStorage.clear());
  await page.reload();
  await page.locator('#gate').waitFor({state: 'visible'});
  if (await page.locator('#panel').isVisible()) throw new Error('Locked page exposed the configuration panel');
  await page.locator('.wrap').screenshot({path: 'docs/images/07_web_locked.png'});
  await page.locator('#gate_code').fill('123456');
  await page.getByRole('button', {name: '解锁', exact: true}).click();
  await page.locator('#gate').waitFor({state: 'hidden'});
  await page.locator('#panel').waitFor({state: 'visible'});
  await page.locator('#stext').filter({hasText: '外网正常'}).waitFor();
  await page.locator('#nets .acct .u').filter({hasText: 'demo-user-01'}).waitFor();
  await page.screenshot({path: 'docs/images/06_web.png', fullPage: true});
  return {screenshots: ['07_web_locked.png', '06_web.png'], data: 'synthetic only'};
}
