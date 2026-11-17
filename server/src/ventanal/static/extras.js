// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
'use strict';
for (const form of document.querySelectorAll('[data-extra]')) {
  let pending = false, running = false, timer;
  const state = form.querySelector('[role=status]');
  async function save() {
    pending = true;
    if (running) return;
    running = true;
    while (pending) {
      pending = false;
      state.textContent = 'Saving…';
      try {
        const response = await fetch(form.action, {method: 'POST', body: new URLSearchParams(new FormData(form))});
        if (!response.ok) throw new Error();
        state.textContent = 'Saved ✓';
        const result = await response.json();
        form.closest('article').querySelector('[data-extra-status]').textContent = result.active ? 'Active' : 'Inactive — requires a licence and enablement';
      } catch { state.textContent = 'Unable to save. Please try again.'; }
    }
    running = false;
  }
  form.addEventListener('submit', event => {event.preventDefault(); save();});
  form.addEventListener('change', save);
  form.addEventListener('input', event => {
    if (event.target.type === 'checkbox') return;
    clearTimeout(timer); timer = setTimeout(save, 800);
  });
}
