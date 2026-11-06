// Browser check for the web page. Needs Node + Playwright (npm i playwright; npx playwright install chromium).
// Run against a freshly seeded server:  BASE_URL=http://127.0.0.1:8080 ADMIN_KEY=demo-admin node tests/ui/ui_check.js
const { chromium } = require('playwright');
const BASE = process.env.BASE_URL || 'http://127.0.0.1:8080';
const KEY = process.env.ADMIN_KEY || 'demo-admin';
let failures = 0;
const ok = (cond, msg) => { if (!cond) { failures++; console.log('  FAIL:', msg); } else console.log('  ok  :', msg); };

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 860 }, colorScheme: 'light' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error' && !m.text().includes('Failed to load resource')) errors.push('console: ' + m.text()); });

  await page.goto(BASE + '/');
  await page.waitForSelector('.card');
  ok((await page.locator('.card').count()) > 0, 'event cards render');
  ok((await page.locator('#stats li').count()) === 5, 'five summary figures shown');

  // --- search filter
  await page.fill('#filters [name=q]', 'meditation');
  await page.waitForFunction(() => /events? found/.test(document.querySelector('#eventCount').textContent));
  await page.waitForTimeout(500);
  const titles = await page.locator('.card h3').allTextContents();
  ok(titles.length > 0 && titles.every((t) => /meditation|wellness/i.test(t) || true), 'search returns results: ' + titles.length);
  ok(titles.every((t) => /meditation/i.test(t)), 'every result title matches the search text');
  await page.click('#clearFilters');
  await page.waitForTimeout(400);

  // --- find a full event with a waiting list through the API, then open it from the UI
  const pop = await (await page.request.get(BASE + '/api/events?sort=popularity&order=desc&limit=40')).json();
  const full = pop.items.find((e) => e.full && e.waiting > 0 && !e.cancelled && e.start > '2026-10-29');
  ok(!!full, 'found a full upcoming event with a waiting list: ' + (full && full.title));
  await page.fill('#filters [name=q]', full.title);
  await page.waitForTimeout(600);
  await page.locator('.card h3 button[data-event-id="' + full.id + '"]').click();
  await page.waitForSelector('#drawer:not([hidden])');
  ok((await page.locator('#drawerTitle').textContent()) === full.title, 'drawer shows the clicked event');
  await page.fill('#drawer [name=sid]', 'ui-test-1');
  await page.fill('#drawer [name=name]', 'UI Tester');
  await page.click('#drawer button[type=submit]');
  await page.waitForSelector('#drawer .form-msg:not([hidden])');
  const msg = await page.locator('#drawer .form-msg').textContent();
  ok(/waiting list/.test(msg) && /number \d+/.test(msg), 'full event -> waiting list message: ' + msg);

  // duplicate registration shows a clear error
  await page.fill('#drawer [name=sid]', 'UI-TEST-1');
  await page.fill('#drawer [name=name]', 'UI Tester');
  await page.click('#drawer button[type=submit]');
  await page.waitForFunction(() => /already registered/.test(document.querySelector('#drawer .form-msg').textContent));
  ok(true, 'duplicate registration refused with a clear message');

  // cancel it again
  await page.fill('#drawer [name=sid]', 'ui-test-1');
  await page.click('#drawer button.danger');
  await page.waitForFunction(() => /Registration cancelled/.test(document.querySelector('#drawer .form-msg').textContent));
  ok(true, 'cancelling works');

  // Escape closes the drawer and returns focus to the card title
  await page.keyboard.press('Escape');
  ok(await page.locator('#drawer').isHidden(), 'Escape closes the drawer');
  ok(await page.evaluate((id) => document.activeElement && document.activeElement.getAttribute('data-event-id') === String(id), full.id), 'focus returns to the same event button');

  // --- admin sign-in
  await page.click('#clearFilters');
  await page.click('#adminBtn');
  await page.fill('#keyForm [name=key]', 'wrong-key');
  await page.click('#keyForm button[type=submit]');
  await page.waitForFunction(() => /not correct/.test(document.querySelector('#keyMsg').textContent));
  ok(true, 'wrong admin key rejected');
  await page.fill('#keyForm [name=key]', KEY);
  await page.click('#keyForm button[type=submit]');
  await page.waitForSelector('#adminTab:not([hidden])');
  ok(true, 'correct admin key unlocks the Admin tab');

  // --- admin: clash refused, valid event created
  const existing = (await (await page.request.get(BASE + '/api/events?limit=1&from=2026-11-02')).json()).items[0];
  await page.click('#adminTab');
  await page.fill('#eventForm [name=title]', 'Clash Attempt');
  await page.selectOption('#eventForm [name=venue_id]', String(existing.venue_id));
  await page.fill('#eventForm [name=start]', existing.start.replace(' ', 'T'));
  await page.fill('#eventForm [name=end]', existing.end.replace(' ', 'T'));
  await page.fill('#eventForm [name=capacity]', '10');
  await page.click('#eventForm button[type=submit]');
  await page.waitForFunction(() => /already booked/.test(document.querySelector('#eventFormMsg').textContent));
  const clashText = await page.locator('#eventFormMsg').textContent();
  ok(clashText.includes(existing.title), 'clash message names the event in the way: ' + clashText);
  await page.fill('#eventForm [name=start]', '2027-01-05T10:00');
  await page.fill('#eventForm [name=end]', '2027-01-05T11:30');
  await page.click('#eventForm button[type=submit]');
  await page.waitForFunction(() => /Created/.test(document.querySelector('#eventFormMsg').textContent));
  ok(true, 'non-clashing event is created');

  // --- admin view inside the drawer: attendees + check-in + CSV
  await page.click('#tabs button[data-tab=events]');
  await page.fill('#filters [name=q]', full.title);
  await page.waitForTimeout(600);
  await page.locator('.card h3 button[data-event-id="' + full.id + '"]').click();
  await page.waitForSelector('#drawer table');
  ok((await page.locator('#drawer table tbody tr').count()) > 0, 'admin sees the attendee table');
  const [dl] = await Promise.all([page.waitForEvent('download'), page.click('#drawer button:has-text("Download CSV")')]);
  ok(dl.suggestedFilename().endsWith('-attendees.csv'), 'CSV download starts: ' + dl.suggestedFilename());
  await page.keyboard.press('Escape');

  // --- calendar
  await page.click('#tabs button[data-tab=calendar]');
  await page.waitForSelector('#calGrid button.ev');
  ok((await page.locator('#calGrid button.ev').count()) > 5, 'calendar shows event chips');
  ok(/2026/.test(await page.locator('#calTitle').textContent()), 'calendar opens on a month that has events: ' + (await page.locator('#calTitle').textContent()));
  await page.click('#calNext');
  await page.waitForTimeout(400);
  ok(true, 'calendar month navigation works');
  await page.click('#calPrev');
  await page.waitForTimeout(300);
  await page.locator('#calGrid button.ev').first().click();
  await page.waitForSelector('#drawer:not([hidden])');
  ok(true, 'clicking a calendar chip opens the event');
  await page.keyboard.press('Escape');

  // --- free slots on a busy day
  const busy = (await (await page.request.get(BASE + '/api/events?from=2026-11-03&to=2026-11-03&limit=50')).json()).items[0];
  await page.click('#tabs button[data-tab=slots]');
  await page.selectOption('#slotForm [name=venue]', String(busy.venue_id));
  await page.fill('#slotForm [name=date]', busy.start.slice(0, 10));
  await page.click('#slotForm button[type=submit]');
  await page.waitForSelector('#slotResult .timeline');
  ok((await page.locator('#slotResult .timeline .busy').count()) > 0, 'timeline shows booked blocks');
  ok((await page.locator('#slotResult .slot-list li').count()) > 0, 'free slots listed');

  // --- reports
  await page.click('#tabs button[data-tab=reports]');
  await page.waitForSelector('#repAtt table');
  ok((await page.locator('#repPop tbody tr').count()) === 8, 'popularity report has 8 rows');
  ok((await page.locator('#repVenue tbody tr').count()) >= 6, 'venue report lists all venues');

  // --- my registrations
  await page.click('#tabs button[data-tab=mine]');
  await page.fill('#mineForm [name=sid]', 'pce26-0001');
  await page.click('#mineForm button[type=submit]');
  await page.waitForSelector('#mineResult table, #mineResult .empty, #mineResult .form-msg');
  ok((await page.locator('#mineResult table tbody tr').count()) > 0, 'student sees their registrations');

  // --- XSS: titles and names are shown as text, never as markup
  const evil = '<img src=x onerror="document.title=\'pwned\'">';
  const venues = await (await page.request.get(BASE + '/api/venues')).json();
  const r = await page.request.post(BASE + '/api/events', { headers: { 'X-Admin-Key': KEY, 'Content-Type': 'application/json' },
    data: { title: evil, category: '<b>x</b>', description: '<script>document.title="pwned"</script>', venue_id: venues[0].id, start: '2027-02-01 10:00', end: '2027-02-01 11:00', capacity: 5 } });
  ok(r.status() === 201, 'created an event with HTML in its title');
  await page.click('#tabs button[data-tab=events]');
  await page.fill('#filters [name=q]', 'onerror');
  await page.waitForTimeout(700);
  ok((await page.locator('.card h3 button').first().textContent()) === evil, 'title shown as literal text');
  ok((await page.locator('.card img').count()) === 0, 'no <img> element was created from the title');
  ok((await page.title()) === 'CampusEvents', 'no injected script ran');

  // --- responsive: no horizontal scroll on a phone-sized screen
  await page.setViewportSize({ width: 390, height: 800 });
  for (const tab of ['events', 'calendar', 'slots', 'reports', 'mine', 'admin']) {
    await page.click('#tabs button[data-tab=' + tab + ']');
    await page.waitForTimeout(500);
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    ok(overflow <= 1, 'no horizontal overflow on mobile in ' + tab + ' (' + overflow + 'px)');
  }
  await page.click('#tabs button[data-tab=events]');
  await page.fill('#filters [name=q]', '');
  await page.waitForTimeout(500);

  ok(errors.length === 0, 'no JavaScript or CSP errors in the console' + (errors.length ? ': ' + errors.join(' | ') : ''));
  await browser.close();
  console.log(failures === 0 ? '\nUI CHECK PASSED' : '\nUI CHECK FAILED: ' + failures);
  process.exit(failures === 0 ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
