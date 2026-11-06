// Takes the README screenshots from a freshly seeded server.
//   BASE_URL=http://127.0.0.1:8080 ADMIN_KEY=demo-admin OUT=docs/screenshots node tests/ui/take_screenshots.js
const { chromium } = require('playwright');
const BASE = process.env.BASE_URL || 'http://127.0.0.1:8080';
const KEY = process.env.ADMIN_KEY || 'demo-admin';
const OUT = process.env.OUT || 'docs/screenshots';

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 860 }, colorScheme: 'light' });
  const page = await ctx.newPage();
  const shot = (name) => page.screenshot({ path: OUT + '/' + name });

  await page.goto(BASE + '/');
  await page.waitForSelector('.card');

  // 1. events list: upcoming events only (filter From = 28 Oct 2026)
  await page.fill('#filters [name=from]', '2026-10-28');
  await page.waitForTimeout(700);
  await shot('01-events.png');

  // 3. calendar
  await page.click('#tabs button[data-tab=calendar]');
  await page.waitForSelector('#calGrid button.ev');
  await shot('03-calendar.png');

  // 4. free slots on a busy day
  const busy = (await (await page.request.get(BASE + '/api/events?from=2026-11-03&to=2026-11-03&limit=50')).json()).items[0];
  await page.click('#tabs button[data-tab=slots]');
  await page.selectOption('#slotForm [name=venue]', String(busy.venue_id));
  await page.fill('#slotForm [name=date]', busy.start.slice(0, 10));
  await page.click('#slotForm button[type=submit]');
  await page.waitForSelector('#slotResult .timeline');
  await shot('04-free-slots.png');

  // 5. reports
  await page.click('#tabs button[data-tab=reports]');
  await page.waitForSelector('#repAtt table');
  await shot('05-reports.png');

  // 6. admin: a booking that clashes is refused and names the event in the way
  await page.click('#adminBtn');
  await page.fill('#keyForm [name=key]', KEY);
  await page.click('#keyForm button[type=submit]');
  await page.waitForSelector('#adminTab:not([hidden])');
  await page.click('#adminTab');
  await page.waitForTimeout(300);
  await page.fill('#eventForm [name=title]', 'Guest Lecture');
  await page.fill('#eventForm [name=category]', 'Academic');
  await page.selectOption('#eventForm [name=venue_id]', String(busy.venue_id));
  await page.fill('#eventForm [name=start]', busy.start.replace(' ', 'T'));
  await page.fill('#eventForm [name=end]', busy.end.replace(' ', 'T'));
  await page.fill('#eventForm [name=capacity]', '10');
  await page.click('#eventForm button[type=submit]');
  await page.waitForFunction(() => /already booked/.test(document.querySelector('#eventFormMsg').textContent));
  await page.waitForTimeout(8000); // let the sign-in toast fade so the shot shows only the refusal
  await shot('06-admin-clash.png');

  // 2. event detail: joining the waiting list of a full event
  await page.click('#adminBtn'); // sign out so the shot shows what a student sees
  await page.click('#tabs button[data-tab=events]');
  const pop = await (await page.request.get(BASE + '/api/events?sort=popularity&order=desc&limit=40')).json();
  const full = pop.items.find((e) => e.full && e.waiting > 0 && e.start > '2026-10-29');
  await page.fill('#filters [name=q]', full.title);
  await page.waitForTimeout(700);
  await page.locator('.card h3 button[data-event-id="' + full.id + '"]').click();
  await page.waitForSelector('#drawer:not([hidden])');
  await page.fill('#drawer [name=sid]', 'DEMO-0001');
  await page.fill('#drawer [name=name]', 'Demo Student');
  await page.click('#drawer button[type=submit]');
  await page.waitForSelector('#drawer .form-msg:not([hidden])');
  await shot('02-event-detail.png');
  await page.keyboard.press('Escape');

  // 7. phone layout
  await page.setViewportSize({ width: 390, height: 800 });
  await page.fill('#filters [name=q]', '');
  await page.waitForTimeout(700);
  await shot('07-mobile.png');

  await browser.close();
  console.log('screenshots written to ' + OUT);
})().catch((e) => { console.error(e); process.exit(1); });
