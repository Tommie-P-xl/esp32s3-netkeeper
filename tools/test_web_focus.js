// Run against the locally served main/index.html with playwright-cli run-code --filename.
// All fetch requests are mocked in the page; no device configuration is submitted.
async (page) => {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  await page.evaluate(() => {
    const fixture = window.webFocusFixture = {
      polls: 0, submitted: [], alerts: [], reply: {ok: false, msg: 'test rejection'}, delay: false,
      state: {state: 'ONLINE', stext: '外网正常', ssid: 'ExampleWiFi', ip: '192.0.2.1', account: 'saved',
        auth_count: 0, next_in_ms: 60000, sd: true, nets: [
          {ssid: 'ExampleWiFi', active: true, current: 'saved', has_pass: true, accts: [{user: 'saved'}]}
        ]}
    };
    window.alert = message => fixture.alerts.push(message);
    window.fetch = async (url, options) => {
      if (url === '/api/state') {
        fixture.polls++;
        const data = JSON.parse(JSON.stringify(fixture.state));
        data.detail = 'test poll ' + fixture.polls;
        return {json: async () => data};
      }
      if (url === '/api/acct' && options.method === 'POST') {
        const submitted = Object.fromEntries(new URLSearchParams(options.body));
        fixture.submitted.push(submitted);
        const reply = Object.assign({}, fixture.reply);
        if (fixture.delay) await new Promise(resolve => { fixture.release = resolve; });
        if (reply.ok) fixture.state.nets[0].accts.push({user: submitted.user});
        return {json: async () => reply};
      }
      throw new Error('Unexpected request in focus test: ' + url);
    };
    load();
  });
  const account = page.getByPlaceholder('校园网账号');
  const password = page.getByPlaceholder('密码', {exact: true});
  await account.waitFor();
  await account.fill('draft-account');
  const original = await account.elementHandle();
  await account.evaluate(el => el.setSelectionRange(2, 4));
  const pollsBefore = await page.evaluate(() => webFocusFixture.polls);
  await page.waitForTimeout(6500);
  check(await original.evaluate(el => el.isConnected), 'Polling replaced the editing input');
  check(await account.evaluate(el => document.activeElement === el), 'Polling stole input focus');
  check(await account.inputValue() === 'draft-account', 'Polling erased the account draft');
  check(await account.evaluate(el => el.selectionStart === 2 && el.selectionEnd === 4), 'Polling changed the selection');
  check(await page.evaluate(() => webFocusFixture.polls >= 3), 'Automatic polling stopped');
  check(await page.evaluate(() => webFocusFixture.polls > 0) && pollsBefore > 0, 'State fixture did not run');
  check((await page.locator('#detail').textContent()).startsWith('test poll '), 'Status card did not keep refreshing');

  // A changed backend list also waits while the user is typing/composing.
  await account.evaluate(el => el.dispatchEvent(new CompositionEvent('compositionstart', {data: '中'})));
  await page.evaluate(() => { webFocusFixture.state.nets[0].accts.push({user: 'external-account'}); load(); });
  await page.waitForTimeout(100);
  check(await original.evaluate(el => el.isConnected), 'Changed state interrupted composition');
  check(await account.evaluate(el => document.activeElement === el), 'Changed state stole focus');
  await account.evaluate(el => el.dispatchEvent(new CompositionEvent('compositionend', {data: '中'})));
  await page.getByRole('heading', {name: '校园网守护 · 配置'}).click();
  await page.evaluate(() => load());
  await page.getByText('external-account', {exact: true}).waitFor();
  check(await account.inputValue() === 'draft-account', 'Applying deferred changes erased the draft');
  await password.fill('draft-password');
  const originalPassword = await password.elementHandle();
  await page.waitForTimeout(3500);
  check(await originalPassword.evaluate(el => el.isConnected), 'Polling replaced the password input');
  check(await password.evaluate(el => document.activeElement === el), 'Password lost focus');
  check(await password.inputValue() === 'draft-password', 'Password draft was lost');

  // Static fields must also retain focus while unrelated list data changes.
  for (const id of ['gate_code', 'n_ssid', 'n_pass']) {
    const field = page.locator('#' + id);
    await field.fill(id === 'gate_code' ? '123456' : 'static-draft');
    await page.evaluate(() => { webFocusFixture.state.nets[0].active = !webFocusFixture.state.nets[0].active; load(); });
    await page.waitForTimeout(100);
    check(await field.evaluate(el => document.activeElement === el), 'Unrelated refresh stole focus from ' + id);
    check(await field.inputValue() === (id === 'gate_code' ? '123456' : 'static-draft'), 'Static draft was lost');
  }

  await page.getByRole('button', {name: '添加账号', exact: true}).click();
  await page.waitForFunction(() => webFocusFixture.alerts.length === 1);
  check(await account.inputValue() === 'draft-account' && await password.inputValue() === 'draft-password', 'Rejected save erased a draft');
  await page.evaluate(() => { webFocusFixture.reply = {ok: true}; });
  await page.getByRole('button', {name: '添加账号', exact: true}).click();
  await page.waitForFunction(() => document.querySelector('#nets input').value === '');
  check(await password.inputValue() === '', 'Successful save did not clear submitted password');

  // A delayed success must not erase text entered after the request was sent.
  await account.fill('submitted-account');
  await password.fill('submitted-password');
  await page.evaluate(() => { webFocusFixture.delay = true; });
  await page.getByRole('button', {name: '添加账号', exact: true}).click();
  await page.waitForFunction(() => typeof webFocusFixture.release === 'function');
  await account.fill('next-account-draft');
  await password.fill('next-password-draft');
  await page.evaluate(() => webFocusFixture.release());
  await page.waitForTimeout(200);
  check(await account.inputValue() === 'next-account-draft' && await password.inputValue() === 'next-password-draft', 'Delayed success erased newer input');
  return {viewport: page.viewportSize(), automaticPolls: await page.evaluate(() => webFocusFixture.polls),
    passed: ['focus, node identity and selection across polls', 'deferred list update and composition',
      'account/password drafts', 'static input focus', 'failed and successful save', 'delayed save preserves newer draft']};
}
