/* CampusEvents front end. Plain JavaScript, no libraries.
   Every piece of server text goes through textContent (never innerHTML), so event titles or
   student names cannot inject markup or scripts. */
'use strict';

// ---------------------------------------------------------------- tiny helpers
function h(tag, props, ...kids) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    node.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return node;
}
const $ = (sel, root = document) => root.querySelector(sel);
const clear = (node) => { while (node.firstChild) node.removeChild(node.firstChild); return node; };

const CATEGORIES = ['tech', 'workshop', 'career', 'wellness', 'cultural', 'sports', 'academic'];
const catClass = (name) => {
  const k = String(name || '').toLowerCase();
  return 'cat-' + (CATEGORIES.includes(k) ? k : 'general');
};

// ---------------------------------------------------------------- state + API
const state = { adminKey: '', page: 0, pageSize: 12, total: 0, openEventId: null, lastFocus: null, month: null };

try { state.adminKey = sessionStorage.getItem('ce_admin') || ''; } catch (e) { /* storage may be blocked */ }

async function api(method, path, body) {
  const headers = { Accept: 'application/json' };
  if (state.adminKey) headers['X-Admin-Key'] = state.adminKey;
  const opts = { method, headers };
  if (method === 'POST') {
    headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body || {});
  }
  let res;
  try { res = await fetch(path, opts); } catch (e) { throw new Error('Cannot reach the server. Is it running?'); }
  let data = null;
  try { data = await res.json(); } catch (e) { /* empty or non-JSON body */ }
  if (!res.ok) {
    const err = new Error((data && data.error) || ('Request failed (' + res.status + ')'));
    err.status = res.status; err.code = data && data.code; err.details = data && data.details;
    throw err;
  }
  return data;
}

let noticeTimer = 0;
function notify(message, kind) {
  const box = $('#notice');
  box.textContent = message;
  box.className = 'notice ' + (kind || '');
  box.hidden = false;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => { box.hidden = true; }, 7000);
}
const fail = (e) => notify(e.message, 'err');
const isAdmin = () => !!state.adminKey && !$('#adminTab').hidden;

// ---------------------------------------------------------------- formatting
const fmtTime = (s) => s.slice(11);                       // "2026-11-02 10:00" -> "10:00"
const fmtRange = (e) => e.start.slice(0, 10) + '  ' + fmtTime(e.start) + ' - ' + (e.end.slice(0, 10) === e.start.slice(0, 10) ? fmtTime(e.end) : e.end);
const pct = (a, b) => (b > 0 ? Math.min(100, Math.round((100 * a) / b)) : 0);

function meter(confirmed, capacity) {
  const bar = h('span');
  bar.style.width = pct(confirmed, capacity) + '%';
  return h('div', { class: 'meter' + (confirmed >= capacity ? ' full' : ''), role: 'img', 'aria-label': confirmed + ' of ' + capacity + ' seats taken' }, bar);
}

function statusChip(e) {
  if (e.cancelled) return h('span', { class: 'chip status-cancelled', text: 'Cancelled' });
  if (e.full) return h('span', { class: 'chip status-full', text: 'Full' });
  return h('span', { class: 'chip status-open', text: e.seats_left + ' seats left' });
}

// ---------------------------------------------------------------- header stats + lookups
async function loadStats() {
  const s = await api('GET', '/api/stats');
  const list = clear($('#stats'));
  for (const [k, label] of [['events', 'Events'], ['venues', 'Venues'], ['students', 'Students'], ['confirmed', 'Registered'], ['waiting', 'On waiting lists']]) {
    list.append(h('li', {}, h('b', { text: String(s[k]) }), h('span', { text: label })));
  }
}

let venues = [];
async function loadLookups() {
  venues = await api('GET', '/api/venues');
  const cats = await api('GET', '/api/categories');
  for (const sel of [$('#filters [name=venue_id]'), $('#calVenue')]) {
    const keep = sel.value;
    while (sel.options.length > 1) sel.remove(1);
    for (const v of venues) sel.append(h('option', { value: v.id, text: v.name + ' (' + v.capacity + ')' }));
    sel.value = keep;
  }
  for (const sel of [$('#slotForm [name=venue]'), $('#eventForm [name=venue_id]')]) {
    const keep = sel.value;
    clear(sel);
    for (const v of venues) sel.append(h('option', { value: v.id, text: v.name + ' (' + v.capacity + ' seats)' }));
    if (keep) sel.value = keep;
  }
  const cs = $('#filters [name=category]');
  const keepCat = cs.value;
  while (cs.options.length > 1) cs.remove(1);
  for (const c of cats) cs.append(h('option', { value: c, text: c }));
  cs.value = keepCat;
  const dl = clear($('#catList'));
  for (const c of cats) dl.append(h('option', { value: c }));
  const legend = clear($('#legend'));
  for (const c of cats) legend.append(h('li', { class: catClass(c), text: c }));
}

// ---------------------------------------------------------------- events list
function filterQuery() {
  const f = $('#filters');
  const p = new URLSearchParams();
  for (const name of ['q', 'venue_id', 'category', 'from', 'to', 'sort', 'order']) {
    const v = f.elements[name].value.trim();
    if (v) p.set(name, v);
  }
  if (f.elements.cancelled.checked) p.set('status', 'all');
  if (f.elements.upcoming.checked) {
    const n = new Date();
    const today = n.getFullYear() + '-' + pad(n.getMonth() + 1) + '-' + pad(n.getDate());
    if (!p.get('from') || p.get('from') < today) p.set('from', today);
  }
  return p;
}

async function loadEvents() {
  const p = filterQuery();
  p.set('limit', state.pageSize);
  p.set('offset', state.page * state.pageSize);
  const data = await api('GET', '/api/events?' + p.toString());
  state.total = data.total;
  if (!state.firstDate && data.items.length) state.firstDate = data.items[Math.floor(data.items.length / 2)].start.slice(0, 10);
  $('#eventCount').textContent = data.total + (data.total === 1 ? ' event found' : ' events found');
  const list = clear($('#eventList'));
  if (!data.items.length) list.append(h('li', { class: 'empty', text: 'No events match these filters.' }));
  for (const e of data.items) list.append(eventCard(e));
  const pages = Math.max(1, Math.ceil(data.total / state.pageSize));
  $('#pageInfo').textContent = 'Page ' + (state.page + 1) + ' of ' + pages;
  $('#prevPage').disabled = state.page === 0;
  $('#nextPage').disabled = state.page + 1 >= pages;
}

function eventCard(e) {
  const open = h('button', { type: 'button', text: e.title, 'data-event-id': e.id });
  open.addEventListener('click', () => openEvent(e.id, open));
  return h('li', { class: 'card ' + catClass(e.category) + (e.cancelled ? ' cancelled' : '') },
    h('div', { class: 'meta' }, h('span', { class: 'chip', text: e.category }), statusChip(e)),
    h('h3', {}, open),
    h('div', { class: 'meta' }, h('span', { text: fmtRange(e) }), h('span', { text: e.venue_name })),
    meter(e.confirmed, e.capacity),
    h('div', { class: 'seat-line' },
      h('span', { text: e.confirmed + ' / ' + e.capacity + ' registered' }),
      h('span', { text: e.waiting ? e.waiting + ' waiting' : '' })));
}

// ---------------------------------------------------------------- event drawer
async function openEvent(id, trigger) {
  state.lastFocus = trigger || document.activeElement;
  state.openEventId = id;
  const drawer = $('#drawer');
  drawer.hidden = false;
  $('#scrim').hidden = false;
  drawer.focus();
  await renderDrawer();
}

function closeDrawer() {
  const id = state.openEventId;
  $('#drawer').hidden = true;
  $('#scrim').hidden = true;
  state.openEventId = null;
  // The lists are re-drawn after a registration, so the original button may be gone:
  // fall back to the same event's button, then to the active tab.
  let target = state.lastFocus && document.contains(state.lastFocus) ? state.lastFocus : null;
  if (!target && id) target = document.querySelector('[data-event-id="' + id + '"]');
  if (!target) target = document.querySelector('#tabs button[aria-selected="true"]');
  if (target) target.focus();
}

async function renderDrawer() {
  const id = state.openEventId;
  if (!id) return;
  const body = $('#drawerBody');
  let e;
  try { e = await api('GET', '/api/events/' + id); } catch (err) { clear(body).append(h('p', { text: err.message })); return; }
  clear(body);
  body.append(
    h('div', { class: 'meta ' + catClass(e.category) }, h('span', { class: 'chip', text: e.category }), statusChip(e)),
    h('h2', { id: 'drawerTitle', text: e.title }),
    e.description ? h('p', { class: 'muted', text: e.description }) : null,
    h('dl', { class: 'kv' },
      h('dt', { text: 'When' }), h('dd', { text: fmtRange(e) }),
      h('dt', { text: 'Where' }), h('dd', { text: e.venue_name + ' (venue seats ' + e.venue_capacity + ')' }),
      h('dt', { text: 'Seats' }), h('dd', { text: e.confirmed + ' of ' + e.capacity + ' taken, ' + e.seats_left + ' left' }),
      h('dt', { text: 'Waiting list' }), h('dd', { text: e.waiting + (e.waiting === 1 ? ' student' : ' students') }),
      e.checked_in ? h('dt', { text: 'Checked in' }) : null, e.checked_in ? h('dd', { text: String(e.checked_in) }) : null),
    meter(e.confirmed, e.capacity));

  if (!e.cancelled) body.append(registerSection(e));
  if (isAdmin()) body.append(await adminSection(e));
}

function registerSection(e) {
  const sid = h('input', { name: 'sid', maxlength: '20', required: true, placeholder: 'PCE26-0001', autocomplete: 'off' });
  const name = h('input', { name: 'name', maxlength: '60', required: true, autocomplete: 'off' });
  const msg = h('p', { class: 'form-msg', role: 'alert', hidden: true });
  const show = (text, ok) => { msg.textContent = text; msg.className = 'form-msg' + (ok ? ' ok' : ''); msg.hidden = false; };
  const form = h('form', { class: 'stack', autocomplete: 'off' },
    h('h3', { text: e.full ? 'Join the waiting list' : 'Register' }),
    h('label', {}, 'Student ID', sid), h('label', {}, 'Full name', name),
    h('div', { class: 'actions' },
      h('button', { class: 'btn danger', type: 'button', text: 'Cancel my registration' }),
      h('button', { class: 'btn primary', type: 'submit', text: e.full ? 'Join waiting list' : 'Register' })),
    msg);
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    try {
      const r = await api('POST', '/api/events/' + e.id + '/register', { student_id: sid.value, name: name.value });
      const text = r.status === 'confirmed' ? 'You are registered.' : 'Event is full - you are number ' + r.waiting_position + ' on the waiting list.';
      show(text, true);
      notify(text, 'ok');
      refreshAll(false);
      await renderDrawerKeepMessage(text);
    } catch (err) { show(err.message, false); }
  });
  $('button.danger', form).addEventListener('click', async () => {
    if (!sid.value.trim()) { show('Enter your student ID first.', false); return; }
    try {
      const r = await api('DELETE', '/api/events/' + e.id + '/registrations/' + encodeURIComponent(sid.value.trim()));
      const text = 'Registration cancelled.' + (r.promoted ? ' ' + r.promoted.name + ' was moved up from the waiting list.' : '');
      notify(text, 'ok');
      refreshAll(false);
      await renderDrawerKeepMessage(text);
    } catch (err) { show(err.message, false); }
  });
  return h('section', {}, form);
}

async function renderDrawerKeepMessage(text) {
  await renderDrawer();
  const form = $('#drawerBody form');
  if (form) { const m = $('.form-msg', form); if (m) { m.textContent = text; m.className = 'form-msg ok'; m.hidden = false; } }
}

async function adminSection(e) {
  const wrap = h('section', {}, h('h3', { text: 'Admin: attendees' }));
  let rows = [];
  try { rows = await api('GET', '/api/events/' + e.id + '/attendees'); } catch (err) { wrap.append(h('p', { class: 'form-msg', text: err.message })); return wrap; }
  const table = h('table', {},
    h('thead', {}, h('tr', {}, h('th', { text: 'Student' }), h('th', { text: 'Status' }), h('th', { text: '' }))),
    h('tbody', {}, rows.slice(0, 200).map((r) => {
      const act = h('td');
      if (r.status === 'confirmed' && !e.cancelled) {
        if (r.checked_in) act.append(h('span', { class: 'muted small', text: 'Checked in' }));
        else {
          const b = h('button', { class: 'btn small', type: 'button', text: 'Check in' });
          b.addEventListener('click', async () => {
            try { await api('POST', '/api/events/' + e.id + '/checkin', { student_id: r.student_id }); refreshAll(false); await renderDrawer(); } catch (err) { fail(err); }
          });
          act.append(b);
        }
      }
      return h('tr', {},
        h('td', {}, h('span', { text: r.name }), ' ', h('span', { class: 'muted mono', text: r.student_id })),
        h('td', { text: r.status === 'waiting' ? 'waiting #' + r.waiting_position : r.status }), act);
    })));
  wrap.append(rows.length ? table : h('p', { class: 'muted', text: 'Nobody has registered yet.' }));
  if (rows.length > 200) wrap.append(h('p', { class: 'muted small', text: 'Showing the first 200 of ' + rows.length + '. Download the CSV for everyone.' }));
  const csv = h('button', { class: 'btn', type: 'button', text: 'Download CSV' });
  csv.addEventListener('click', () => downloadCsv(e.id));
  const actions = h('div', { class: 'actions' }, csv);
  if (!e.cancelled) {
    const cancel = h('button', { class: 'btn danger', type: 'button', text: 'Cancel this event' });
    cancel.addEventListener('click', async () => {
      if (!window.confirm('Cancel "' + e.title + '"? This frees the venue slot.')) return;
      try { await api('POST', '/api/events/' + e.id + '/cancel', {}); notify('Event cancelled.', 'ok'); refreshAll(false); await renderDrawer(); } catch (err) { fail(err); }
    });
    actions.append(cancel);
  }
  wrap.append(actions);
  return wrap;
}

async function downloadCsv(id) {
  try {
    const res = await fetch('/api/events/' + id + '/attendees.csv', { headers: { 'X-Admin-Key': state.adminKey } });
    if (!res.ok) throw new Error('Download failed (' + res.status + ')');
    const url = URL.createObjectURL(await res.blob());
    const a = h('a', { href: url, download: 'event-' + id + '-attendees.csv' });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  } catch (err) { fail(err); }
}

// ---------------------------------------------------------------- calendar
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const pad = (n) => String(n).padStart(2, '0');

async function loadCalendar() {
  const { y, m } = state.month;     // m: 0-11
  $('#calTitle').textContent = MONTHS[m] + ' ' + y;
  const last = new Date(y, m + 1, 0).getDate();
  const p = new URLSearchParams({ from: y + '-' + pad(m + 1) + '-01', to: y + '-' + pad(m + 1) + '-' + pad(last), limit: '200', sort: 'start' });
  if ($('#calVenue').value) p.set('venue_id', $('#calVenue').value);
  const data = await api('GET', '/api/events?' + p.toString());
  const byDay = {};
  for (const e of data.items) (byDay[Number(e.start.slice(8, 10))] ||= []).push(e);

  const grid = clear($('#calGrid'));
  for (const d of ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']) grid.append(h('div', { class: 'dow', role: 'columnheader', text: d }));
  const lead = (new Date(y, m, 1).getDay() + 6) % 7;   // Monday-first
  for (let i = 0; i < lead; i++) grid.append(h('div', { class: 'day out' }));
  const now = new Date();
  for (let d = 1; d <= last; d++) {
    const isToday = now.getFullYear() === y && now.getMonth() === m && now.getDate() === d;
    const cell = h('div', { class: 'day' + (isToday ? ' today' : ''), role: 'gridcell' }, h('span', { class: 'dnum', text: String(d) }));
    const list = byDay[d] || [];
    for (const e of list.slice(0, 3)) {
      const b = h('button', { type: 'button', class: 'ev ' + catClass(e.category), title: e.title + ' - ' + e.venue_name, text: fmtTime(e.start) + ' ' + e.title, 'data-event-id': e.id });
      b.addEventListener('click', () => openEvent(e.id, b));
      cell.append(b);
    }
    if (list.length > 3) {
      const more = h('button', { type: 'button', class: 'ev', text: '+' + (list.length - 3) + ' more' });
      more.addEventListener('click', () => { const f = $('#filters'); f.reset(); f.elements.from.value = y + '-' + pad(m + 1) + '-' + pad(d); f.elements.to.value = f.elements.from.value; showTab('events'); applyFilters(); });
      cell.append(more);
    }
    grid.append(cell);
  }
  while (grid.children.length % 7 !== 0) grid.append(h('div', { class: 'day out' }));
  if (data.total > data.items.length) notify('This month has more than 200 events; only the first 200 are shown.', '');
}

async function initCalendar() {
  const now = new Date();
  state.month = { y: now.getFullYear(), m: now.getMonth() };
  // If this month has no events (e.g. a demo database), jump to the month of the latest event.
  const p = new URLSearchParams({ from: state.month.y + '-' + pad(state.month.m + 1) + '-01', to: state.month.y + '-' + pad(state.month.m + 1) + '-28', limit: '1' });
  const here = await api('GET', '/api/events?' + p.toString());
  if (here.total === 0) {
    const latest = await api('GET', '/api/events?sort=start&order=desc&limit=1');
    if (latest.items.length) state.month = { y: Number(latest.items[0].start.slice(0, 4)), m: Number(latest.items[0].start.slice(5, 7)) - 1 };
  }
}

// ---------------------------------------------------------------- free slots
const toMin = (s) => Number(s.slice(11, 13)) * 60 + Number(s.slice(14, 16));

async function findSlots(ev) {
  ev.preventDefault();
  const f = $('#slotForm').elements;
  const out = clear($('#slotResult'));
  try {
    const q = new URLSearchParams({ date: f.date.value, day_start: f.day_start.value, day_end: f.day_end.value, min_minutes: f.min.value });
    const r = await api('GET', '/api/venues/' + f.venue.value + '/free-slots?' + q.toString());
    const from = Number(r.day_start.slice(0, 2)) * 60 + Number(r.day_start.slice(3, 5));
    const to = Number(r.day_end.slice(0, 2)) * 60 + Number(r.day_end.slice(3, 5));
    const span = to - from;
    const place = (node, a, b) => { node.style.left = ((a - from) / span) * 100 + '%'; node.style.width = ((b - a) / span) * 100 + '%'; return node; };
    const line = h('div', { class: 'timeline', role: 'img', 'aria-label': 'Day timeline: red is booked, green is free' });
    for (const s of r.free_slots) line.append(place(h('div', { class: 'free' }), toMin(s.start), toMin(s.end) === 0 ? to : toMin(s.end)));
    for (const b of r.booked) line.append(place(h('div', { class: 'busy', title: b.title }), b.start.slice(0, 10) === r.date ? Math.max(from, toMin(b.start)) : from, Math.min(to, b.end.slice(0, 10) === r.date ? toMin(b.end) : to)));
    const hours = h('div', { class: 'hours' });
    for (let t = Math.ceil(from / 60) * 60; t <= to; t += 120) { const lab = h('span', { text: pad(Math.floor(t / 60)) + ':00' }); lab.style.left = ((t - from) / span) * 100 + '%'; if (t === from) lab.style.transform = 'none'; else if (t === to) lab.style.transform = 'translateX(-100%)'; hours.append(lab); }
    out.append(h('h3', { text: r.venue.name + ' on ' + r.date }), line, hours);
    if (r.booked.length) out.append(h('p', { class: 'muted small', text: 'Booked: ' + r.booked.map((b) => b.title + ' ' + fmtTime(b.start) + '-' + fmtTime(b.end)).join('; ') }));
    if (!r.free_slots.length) out.append(h('p', { class: 'empty', text: 'No free slot of at least ' + r.min_minutes + ' minutes.' }));
    else {
      out.append(h('ul', { class: 'slot-list' }, r.free_slots.map((s) => h('li', {}, h('span', { text: fmtTime(s.start) + ' - ' + fmtTime(s.end) }), h('span', { class: 'muted', text: Math.floor(s.minutes / 60) + ' h ' + (s.minutes % 60) + ' min free' })))));
    }
  } catch (err) { out.append(h('p', { class: 'form-msg', text: err.message })); }
}

// ---------------------------------------------------------------- reports
function simpleTable(headers, rows) {
  return h('table', {}, h('thead', {}, h('tr', {}, headers.map((x) => h('th', { class: x.num ? 'num' : '', text: x.label })))), h('tbody', {}, rows));
}

async function loadReports() {
  const [pop, ven, wait, att] = await Promise.all([
    api('GET', '/api/reports/popularity?limit=8'), api('GET', '/api/reports/venues'),
    api('GET', '/api/reports/waitlist'), api('GET', '/api/reports/attendance')]);

  clear($('#repPop')).append(simpleTable([{ label: 'Event' }, { label: 'Registered', num: true }, { label: 'Waiting', num: true }],
    pop.map((r, i) => h('tr', {}, h('td', {}, h('span', { class: 'rank', text: (i + 1) + '.' }), r.title, ' ', h('span', { class: 'muted small', text: r.start.slice(0, 10) })), h('td', { class: 'num', text: r.confirmed + '/' + r.capacity }), h('td', { class: 'num', text: String(r.waiting) })))));

  clear($('#repVenue')).append(simpleTable([{ label: 'Venue' }, { label: 'Events', num: true }, { label: 'Booked h', num: true }, { label: 'Avg fill' }],
    ven.map((r) => { const m = meter(Math.round(r.avg_fill_pct), 100); return h('tr', {}, h('td', { text: r.name }), h('td', { class: 'num', text: String(r.events) }), h('td', { class: 'num', text: String(r.booked_hours) }), h('td', {}, m, h('span', { class: 'muted small', text: r.avg_fill_pct + '%' }))); })));

  const w = clear($('#repWait'));
  if (!wait.length) w.append(h('p', { class: 'empty', text: 'No waiting lists right now.' }));
  else w.append(simpleTable([{ label: 'Event' }, { label: 'Date' }, { label: 'Waiting', num: true }],
    wait.slice(0, 10).map((r) => h('tr', {}, h('td', { text: r.title }), h('td', { class: 'nowrap', text: r.start.slice(0, 10) }), h('td', { class: 'num', text: String(r.waiting) })))));

  clear($('#repAtt')).append(simpleTable([{ label: 'Event' }, { label: 'Date' }, { label: 'Filled' }, { label: 'Showed up', num: true }],
    att.map((r) => h('tr', {}, h('td', { text: r.title }), h('td', { class: 'nowrap', text: r.start.slice(0, 10) }), h('td', {}, meter(r.confirmed, r.capacity), h('span', { class: 'muted small', text: r.fill_pct + '% (' + r.confirmed + '/' + r.capacity + ')' })),
      h('td', { class: 'num', text: r.checked_in ? r.show_up_pct + '%' : '-' })))));
}

// ---------------------------------------------------------------- my registrations
async function showMine(ev) {
  ev.preventDefault();
  const sid = $('#mineForm').elements.sid.value.trim();
  const out = clear($('#mineResult'));
  try {
    const rows = await api('GET', '/api/students/' + encodeURIComponent(sid) + '/registrations');
    if (!rows.length) { out.append(h('p', { class: 'empty', text: 'No registrations found for ' + sid.toUpperCase() + '.' })); return; }
    out.append(simpleTable([{ label: 'Event' }, { label: 'When' }, { label: 'Where' }, { label: 'Status' }, { label: '' }],
      rows.map((r) => {
        const cancel = h('button', { class: 'btn small danger', type: 'button', text: 'Cancel' });
        cancel.addEventListener('click', async () => {
          try { const x = await api('DELETE', '/api/events/' + r.event_id + '/registrations/' + encodeURIComponent(sid)); notify('Registration cancelled.' + (x.promoted ? ' ' + x.promoted.name + ' was moved up.' : ''), 'ok'); refreshAll(false); showMine(ev); } catch (err) { fail(err); }
        });
        const label = r.event_status === 'cancelled' ? 'event cancelled' : r.status === 'waiting' ? 'waiting #' + r.waiting_position : r.checked_in ? 'attended' : 'confirmed';
        return h('tr', {}, h('td', { text: r.title }), h('td', { text: r.start + ' - ' + fmtTime(r.end) }), h('td', { text: r.venue_name }), h('td', { text: label }), h('td', {}, r.event_status === 'cancelled' ? null : cancel));
      })));
  } catch (err) { out.append(h('p', { class: 'form-msg', text: err.message })); }
}

// ---------------------------------------------------------------- admin
function showFormMsg(el, text, ok) { el.textContent = text; el.className = 'form-msg' + (ok ? ' ok' : ''); el.hidden = false; }

async function createVenue(ev) {
  ev.preventDefault();
  const f = ev.target.elements;
  try {
    await api('POST', '/api/venues', { name: f.name.value, capacity: Number(f.capacity.value), location: f.location.value });
    notify('Venue added.', 'ok'); ev.target.reset(); await loadLookups(); refreshAll(false);
  } catch (err) { fail(err); }
}

async function createEvent(ev) {
  ev.preventDefault();
  const f = ev.target.elements;
  const msg = $('#eventFormMsg');
  try {
    const e = await api('POST', '/api/events', { title: f.title.value, category: f.category.value, description: f.description.value, venue_id: Number(f.venue_id.value), start: f.start.value, end: f.end.value, capacity: Number(f.capacity.value) });
    showFormMsg(msg, 'Created "' + e.title + '".', true);
    notify('Event created.', 'ok'); ev.target.reset(); await loadLookups(); refreshAll(false);
  } catch (err) {
    // A clash names the event that is in the way - show that, it is the useful part.
    showFormMsg(msg, err.message, false);
  }
}

// ---------------------------------------------------------------- admin key dialog
function setAdmin(on) {
  $('#adminTab').hidden = !on;
  $('#adminBtn').textContent = on ? 'Sign out' : 'Admin sign-in';
  if (!on && !$('#tab-admin').hidden) showTab('events');
}

async function checkSavedKey() {
  if (!state.adminKey) return;
  try { await api('GET', '/api/auth/check'); setAdmin(true); } catch (e) { state.adminKey = ''; setAdmin(false); }
}

function openKeyDialog() {
  const dlg = $('#keyDialog');
  state.lastFocus = document.activeElement;
  $('#keyMsg').hidden = true;
  dlg.hidden = false;
  $('#keyForm').elements.key.focus();
}
function closeKeyDialog() { $('#keyDialog').hidden = true; $('#keyForm').reset(); if (state.lastFocus) state.lastFocus.focus(); }

// ---------------------------------------------------------------- tabs + wiring
const loaders = { events: loadEvents, calendar: loadCalendar, reports: loadReports, slots: async () => {}, mine: async () => {}, admin: async () => {} };
let currentTab = 'events';

function showTab(name) {
  currentTab = name;
  $('#notice').hidden = true;
  for (const b of document.querySelectorAll('#tabs button')) b.setAttribute('aria-selected', String(b.dataset.tab === name));
  for (const p of document.querySelectorAll('.panel')) p.hidden = p.id !== 'tab-' + name;
  loaders[name]().catch(fail);
}

function applyFilters() { state.page = 0; loadEvents().catch(fail); }

async function refreshAll(withLookups) {
  try {
    await loadStats();
    if (withLookups) await loadLookups();
    if (!$('#tab-events').hidden) await loadEvents();
    if (!$('#tab-calendar').hidden) await loadCalendar();
    if (!$('#tab-reports').hidden) await loadReports();
  } catch (e) { fail(e); }
}

function init() {
  for (const b of document.querySelectorAll('#tabs button')) b.addEventListener('click', () => showTab(b.dataset.tab));
  const filters = $('#filters');
  let t = 0;
  filters.addEventListener('input', (e) => { clearTimeout(t); t = setTimeout(applyFilters, e.target.name === 'q' ? 250 : 0); });
  filters.addEventListener('submit', (e) => e.preventDefault());
  $('#clearFilters').addEventListener('click', () => { filters.reset(); applyFilters(); });
  $('#prevPage').addEventListener('click', () => { state.page = Math.max(0, state.page - 1); loadEvents().catch(fail); });
  $('#nextPage').addEventListener('click', () => { state.page += 1; loadEvents().catch(fail); });
  $('#calPrev').addEventListener('click', () => { const s = state.month; state.month = s.m === 0 ? { y: s.y - 1, m: 11 } : { y: s.y, m: s.m - 1 }; loadCalendar().catch(fail); });
  $('#calNext').addEventListener('click', () => { const s = state.month; state.month = s.m === 11 ? { y: s.y + 1, m: 0 } : { y: s.y, m: s.m + 1 }; loadCalendar().catch(fail); });
  $('#calVenue').addEventListener('change', () => loadCalendar().catch(fail));
  $('#slotForm').addEventListener('submit', findSlots);
  $('#mineForm').addEventListener('submit', showMine);
  $('#venueForm').addEventListener('submit', createVenue);
  $('#eventForm').addEventListener('submit', createEvent);
  $('#drawerClose').addEventListener('click', closeDrawer);
  $('#scrim').addEventListener('click', closeDrawer);
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    if (!$('#keyDialog').hidden) closeKeyDialog(); else if (!$('#drawer').hidden) closeDrawer();
  });
  $('#adminBtn').addEventListener('click', () => {
    if (!$('#adminTab').hidden) {   // sign out
      state.adminKey = ''; try { sessionStorage.removeItem('ce_admin'); } catch (e) { /* ignore */ }
      setAdmin(false); if (state.openEventId) renderDrawer(); return;
    }
    openKeyDialog();
  });
  $('#keyCancel').addEventListener('click', closeKeyDialog);
  $('#keyForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const key = $('#keyForm').elements.key.value;
    state.adminKey = key;
    try {
      await api('GET', '/api/auth/check');
      try { sessionStorage.setItem('ce_admin', key); } catch (x) { /* ignore */ }
      setAdmin(true); closeKeyDialog(); notify('Signed in as admin.', 'ok');
      if (state.openEventId) renderDrawer();
    } catch (err) { state.adminKey = ''; showFormMsg($('#keyMsg'), err.status === 401 ? 'That key is not correct.' : err.message, false); }
  });

  const today = new Date();
  $('#slotForm').elements.date.value = today.getFullYear() + '-' + pad(today.getMonth() + 1) + '-' + pad(today.getDate());

  (async () => {
    try {
      await checkSavedKey();
      await loadLookups();
      await loadStats();
      await initCalendar();
      await loadEvents();
      const first = $('#eventList .card h3 button');
      if (first && state.firstDate) $('#slotForm').elements.date.value = state.firstDate;
    } catch (e) { fail(e); }
  })();
}

init();
