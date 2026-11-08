// Playwright CLI run-code --filename; serve main/index.html locally first.
// API responses are mocked: this never unlocks or changes a real device.
async (page) => {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  let valid = false, hold = false, networkError = false, release, seen, requests = 0;
  const state = {state: 'ONLINE', stext: '外网正常', nets: [], sd: false, auth_count: 0};
  const apiPattern = new URL(page.url()).origin + '/api/**';
  await page.route(apiPattern, async route => {
    const path = new URL(route.request().url()).pathname;
    let result;
    if (path === '/api/state') result = state;
    else if (path === '/api/auth') result = {ok: true, token: 'fresh-session'};
    else if (path === '/api/check') result = {ok: false, need_auth: true, msg: 'expired'};
    else if (path === '/api/session') {
      requests++;
      const accepted = valid, fail = networkError;
      const paused = hold ? new Promise(resolve => { release = resolve; }) : Promise.resolve();
      seen();
      await paused;
      if (fail) { await route.abort('failed'); return; }
      result = {ok: accepted, need_auth: !accepted};
    } else throw new Error('Unexpected session-test API: ' + path);
    await route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify(result)});
  });
  await page.addInitScript(() => { window.alert = () => {}; });
  const gate = page.locator('#gate');
  async function enter(token, accepted, paused = false, fail = false) {
    valid = accepted; hold = paused; networkError = fail; release = null;
    const requested = new Promise(resolve => { seen = resolve; });
    await page.evaluate(value => {
      if (value) localStorage.setItem('pg_token', value);
      else localStorage.removeItem('pg_token');
    }, token);
    await page.reload();
    if (token) await requested;
  }
  await enter('', false);
  check(await gate.isVisible() && requests === 0, 'First visit must show the code form');
  await enter('stale-token-after-reboot', false);
  await page.waitForFunction(() => !localStorage.getItem('pg_token'));
  check(await gate.isVisible(), 'Stale token hid the code form');

  await enter('valid-session', true, true);
  check(await gate.isVisible(), 'Cached token hid the form before server validation');
  release();
  await gate.waitFor({state: 'hidden'});
  check(await page.evaluate(() => TOKEN === 'valid-session'), 'Valid session was discarded');

  await enter('valid-session', true, true);
  await page.getByRole('button', {name: '锁定页面', exact: true}).click();
  release();await page.waitForTimeout(200);
  check(await gate.isVisible() && await page.evaluate(() => !TOKEN && !localStorage.getItem('pg_token')),
    'Old validation response overrode explicit locking');

  await enter('stale-token', false, true);
  await page.getByPlaceholder('6 位验证码').fill('123456');
  await page.getByRole('button', {name: '解锁', exact: true}).click();
  await gate.waitFor({state: 'hidden'});
  release();await page.waitForTimeout(200);
  check(!await gate.isVisible() && await page.evaluate(() => TOKEN === 'fresh-session'),
    'Old rejection overrode a new successful unlock');

  await enter('possibly-valid-session', false, false, true);
  await page.waitForTimeout(200);
  check(await gate.isVisible() && await page.evaluate(() => localStorage.getItem('pg_token') === 'possibly-valid-session'),
    'Network failure must keep the form visible without deleting an unverified token');

  await enter('valid-session', true);
  await gate.waitFor({state: 'hidden'});
  await page.getByRole('button', {name: '立即检测', exact: true}).click();
  await gate.waitFor({state: 'visible'});
  check(await page.evaluate(() => !TOKEN && !localStorage.getItem('pg_token')),
    'Expired write-session response did not clear the cached token');
  return {viewport: page.viewportSize(), passed: ['first visit', 'stale token after reboot',
    'valid token checked before hiding', 'locking during validation', 'new unlock during old validation',
    'validation network failure', 'write-session expiry']};
}
