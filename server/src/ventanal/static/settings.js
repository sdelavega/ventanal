/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stephen de la Vega */
'use strict';
const form = document.querySelector('#settings-form');
if (form) {
  document.documentElement.classList.add('settings-enhanced');
  const timers = new WeakMap();
  const states = new WeakMap();
  const slots = form.querySelector('[name="schedule.art_slots"]');
  if (slots) slots.disabled = false;
  let browserSession;
  try {
    browserSession = sessionStorage.getItem('ventanal-settings-session') || crypto.randomUUID();
    sessionStorage.setItem('ventanal-settings-session', browserSession);
  } catch (_) { browserSession = String(Date.now()); }
  const csrf = form.elements.csrf.value;
  const screenState = document.querySelector('#screen-state');
  function chips(group) {
    const times = [...group.querySelectorAll('input[type=time]')].map(el => el.value);
    return [...new Set(times.filter(Boolean))].sort();
  }
  function canonicalChips(group) {
    const container = group.querySelector('[data-time-chips]');
    const seen = new Set();
    [...container.children].sort((a,b) => a.querySelector('input').value.localeCompare(b.querySelector('input').value)).forEach(chip => {
      const value = chip.querySelector('input').value;
      if (seen.has(value)) chip.remove();
      else { seen.add(value); container.append(chip); }
    });
  }
  function serialize(group) {
    if (group.dataset.field === 'ham.call_sign') {
      const input = group.querySelector('input'); input.value = input.value.trim().toUpperCase();
    }
    const data = new URLSearchParams({csrf, browser_session:browserSession, field: group.dataset.field});
    if (group.querySelector('[data-time-chips]')) {
      group.querySelector('input[name="schedule.art_slots"]').value = chips(group).join(',');
    }
    group.querySelectorAll('[data-date-part="month"]').forEach(month => {
      const label = month.closest('label');
      label.querySelector('input').value = month.value + '-' + label.querySelector('[data-date-part="day"]').value;
    });
    group.querySelectorAll('[name]').forEach(el => {
      if (el.dataset.datePart) return;
      if (el.name.endsWith('.present') || el.name === 'museums.present') return;
      if (el.type === 'checkbox') {
        if (el.checked) data.append(el.name, el.value);
        else if (el.name === group.dataset.field && el.name !== 'art.museums') data.append(el.name, 'no');
      } else data.append(el.name, el.value);
    });
    if (group.dataset.field === 'seasons') data.set('seasons.count', group.querySelectorAll('[data-season]').length);
    // An empty museum selection must reach the shared validator.
    if (group.dataset.field === 'art.museums' && !data.has('art.museums')) data.append('art.museums', '');
    return data;
  }
  form.querySelectorAll('[data-field]').forEach(group => {
    states.set(group, {busy:false, pending:false, version:0, saved:serialize(group).toString()});
  });
  async function save(group) {
    clearTimeout(timers.get(group));
    let state = states.get(group);
    if (!state) { state = {busy:false, pending:false, version:0}; states.set(group,state); }
    if (state.busy) { state.pending = true; return; }
    const data = serialize(group);
    if (data.toString() === state.saved) {
      if (group.querySelector('[data-time-chips]')) canonicalChips(group);
      const message = group.querySelector(':scope > .field-state') || group.querySelector('.field-state');
      if (message.textContent === 'Saving…') message.textContent = '';
      return;
    }
    state.busy = true;
    const version = state.version;
    const message = group.querySelector(':scope > .field-state') || group.querySelector('.field-state');
    message.className = 'field-state'; message.textContent = 'Saving…';
    try {
      const response = await fetch('/settings/field', {method:'POST', body:data, redirect:'error'});
      if (!response.headers.get('Content-Type')?.includes('application/json')) throw new Error();
      const result = await response.json();
      if (!response.ok) {
        message.classList.add('error'); message.textContent = result.error || 'Unable to save. Try again.';
      } else {
        state.saved = data.toString();
        if (version === state.version) {
          if (group.querySelector('[data-time-chips]')) canonicalChips(group);
          message.textContent = 'Saved ✓';
          setTimeout(() => { if (message.textContent === 'Saved ✓' && !state.busy) message.classList.add('faded'); }, 3000);
        }
      }
    } catch (_) { message.classList.add('error'); message.textContent = 'Unable to save. Check your connection and try again.'; }
    finally { state.busy = false; if (state.pending) { state.pending = false; save(group); } }
  }
  function changed(group, immediate) {
    let state = states.get(group);
    if (!state) { state={busy:false,pending:false,version:0}; states.set(group,state); }
    state.version++;
    clearTimeout(timers.get(group));
    if (immediate) save(group);
    else timers.set(group, setTimeout(() => save(group), 800));
  }
  form.addEventListener('submit', async event => {
    event.preventDefault();
    if (event.submitter?.getAttribute('formaction') !== '/ham/refresh') return;
    const button = event.submitter;
    button.disabled = true;
    try {
      const response = await fetch('/ham/refresh', {method:'POST',body:new URLSearchParams({csrf}),redirect:'error'});
      document.querySelector('#ham-progress').textContent = response.ok ? 'Refreshing licence database…' : 'Unable to start database refresh.';
    } catch (_) { document.querySelector('#ham-progress').textContent = 'Unable to start database refresh.'; }
    finally { button.disabled = false; }
  });
  form.addEventListener('input', event => {
    const group = event.target.closest('[data-field]');
    if (group && !['checkbox','time'].includes(event.target.type) && event.target.tagName !== 'SELECT') changed(group,false);
  });
  form.addEventListener('change', event => {
    const group = event.target.closest('[data-field]');
    if (group) changed(group,true);
  });
  form.addEventListener('focusout', event => {
    const group = event.target.closest('[data-field]');
    if (group && event.target.name && !event.target.readOnly && event.target.tagName === 'INPUT' && !['checkbox','time','hidden'].includes(event.target.type)) save(group);
  });
  form.addEventListener('keydown', event => {
    if (event.key === 'Enter' && event.target.tagName === 'INPUT') {
      event.preventDefault(); const group = event.target.closest('[data-field]'); if (group) save(group);
    }
  });
  form.addEventListener('click', event => {
    const group = event.target.closest('[data-field]');
    if (!group) return;
    if (event.target.closest('[data-add-season], [data-remove-season]')) {
      const seasons = form.querySelector('[data-field="seasons"]');
      const rows = seasons.querySelector('tbody');
      if (event.target.closest('[data-add-season]')) {
        if (rows.children.length >= 50) {
          const message = group.querySelector(':scope > .field-state');
          message.className = 'field-state error'; message.textContent = 'At most 50 seasons are allowed.';
          return;
        }
        const row = rows.lastElementChild.cloneNode(true);
        row.querySelector('.field-state').textContent = '';
        row.querySelector('input[name$=".name"]').value = 'New season';
        rows.append(row);
      } else {
        if (rows.children.length <= 1) {
          const message = group.querySelector('.field-state'); message.className='field-state error'; message.textContent='At least one season is required.'; return;
        }
        event.target.closest('[data-season]').remove();
      }
      [...rows.children].forEach((row,index) => {
        row.dataset.field = `art.seasons[${index}]`;
        row.querySelectorAll('[name]').forEach(el => { el.name = el.name.replace(/art.seasons\[\d+\]/,row.dataset.field); });
      });
      changed(seasons,true); return;
    }
    if (event.target.closest('[data-remove-time]')) {
      if (group.querySelectorAll('.time-chip').length <= 1) {
        const message=group.querySelector('.field-state'); message.className='field-state error'; message.textContent='At least one art change time is required.'; return;
      }
      event.target.closest('.time-chip').remove(); changed(group,true);
    }
    if (event.target.closest('[data-add-time]')) {
      const chip = document.createElement('span'); chip.className = 'time-chip';
      chip.innerHTML = '<input type="time" aria-label="Art change time"><button type="button" data-remove-time aria-label="Remove time">×</button>';
      group.querySelector('[data-time-chips]').append(chip); chip.querySelector('input').focus();
    }
  });
  async function poll() {
    try {
      const response = await fetch('/settings/state', {redirect:'error'});
      if (response.ok) {
        const data = await response.json(); screenState.textContent = data.status;
        for (const key of ['reason','refreshed','progress']) {
          const target = document.querySelector('#ham-'+key);
          if (target && data.ham) target.textContent = data.ham[key];
        }
      }
    } catch (_) { /* Individual field saves report connectivity errors. */ }
  }
  poll(); setInterval(poll,1500);
}
