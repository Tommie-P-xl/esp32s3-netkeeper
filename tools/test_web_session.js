// Playwright CLI run-code --filename; serve main/index.html locally first.
// API responses are mocked: this never unlocks or changes a real device.
async (page) => {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  let valid = false, hold = false, networkError = false, authOK = true, release, seen, requests = 0;
  const state = {state: 'ONLINE', stext: '外网正常', nets: [], sd: false, auth_count: 0};
  const apiPattern = new URL(page.url()).origin + '/api/**';
  await page.route(apiPattern, async route => {
    const path = new URL(route.request().url()).pathname;
    let result;
    if (path === '/api/state') result = state;
    else if (path === '/api/auth') result = authOK ? {ok: true, token: 'fresh-session'} : {ok: false, msg: 'wrong code'};
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
  const panel = page.locator('#panel');
  async function checkLocked(message) {
    check(await gate.isVisible() && !await panel.isVisible() && !await page.locator('#status').isVisible(), message);
  }
  async function enter(token, accepted, paused = false, fail = false) {
    valid = accepted; hold = paused; networkError = fail; authOK = true; release = null;
    const requested = new Promise(resolve => { seen = resolve; });
    await page.evaluate(value => {
      if (value) localStorage.setItem('pg_token', value);
      else localStorage.removeItem('pg_token');
    }, token);
    await page.reload();
    if (token) await requested;
  }
  await enter('', false);
  await checkLocked('First visit must show only the code form');
  check(requests === 0, 'First visit without a token must not validate a session');
  authOK = false;
  await page.getByPlaceholder('6 位验证码').fill('000000');
  const rejected = page.waitForResponse(response => new URL(response.url()).pathname === '/api/auth');
  await page.getByRole('button', {name: '解锁', exact: true}).click();
  await rejected;
  await checkLocked('Wrong verification code exposed the panel');
  await enter('stale-token-after-reboot', false);
  await page.waitForFunction(() => !localStorage.getItem('pg_token'));
  await checkLocked('Stale token exposed the panel');

  await enter('valid-session', true, true);
  await checkLocked('Cached token exposed the panel before server validation');
  release();
  await gate.waitFor({state: 'hidden'});
  check(await panel.isVisible(), 'Validated cached session did not show the panel');
  check(await page.evaluate(() => TOKEN === 'valid-session'), 'Valid session was discarded');
  await page.getByRole('button', {name: '锁定页面', exact: true}).click();
  await checkLocked('Explicit locking did not hide the complete panel');

  await enter('valid-session', true, true);
  await page.evaluate(() => lockNow());
  release();await page.waitForTimeout(200);
  check(await gate.isVisible() && await page.evaluate(() => !TOKEN && !localStorage.getItem('pg_token')),
    'Old validation response overrode explicit locking');
  await checkLocked('Old validation response exposed the panel after locking');

  await enter('stale-token', false, true);
  await page.getByPlaceholder('6 位验证码').fill('123456');
  await page.getByRole('button', {name: '解锁', exact: true}).click();
  await gate.waitFor({state: 'hidden'});
  release();await page.waitForTimeout(200);
  check(!await gate.isVisible() && await page.evaluate(() => TOKEN === 'fresh-session'),
    'Old rejection overrode a new successful unlock');
  check(await panel.isVisible(), 'Successful code submission did not show the panel');

  await enter('possibly-valid-session', false, false, true);
  await page.waitForTimeout(200);
  check(await gate.isVisible() && await page.evaluate(() => localStorage.getItem('pg_token') === 'possibly-valid-session'),
    'Network failure must keep the form visible without deleting an unverified token');
  await checkLocked('Validation network failure exposed the panel');

  await enter('valid-session', true);
  await gate.waitFor({state: 'hidden'});
  await page.getByRole('button', {name: '立即检测', exact: true}).click();
  await gate.waitFor({state: 'visible'});
  check(await page.evaluate(() => !TOKEN && !localStorage.getItem('pg_token')),
    'Expired write-session response did not clear the cached token');
  await checkLocked('Expired session left the configuration panel visible');
  return {viewport: page.viewportSize(), passed: ['first visit', 'stale token after reboot',
    'valid token checked before hiding', 'locking during validation', 'new unlock during old validation',
    'validation network failure', 'write-session expiry', 'wrong verification code', 'explicit lock hides panel']};
}
