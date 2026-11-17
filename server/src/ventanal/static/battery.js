/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Stephen de la Vega */
'use strict';
document.querySelectorAll('form[action="/battery"]').forEach(form => {
  let browserSession;
  try {
    browserSession = sessionStorage.getItem('ventanal-settings-session') || crypto.randomUUID();
    sessionStorage.setItem('ventanal-settings-session', browserSession);
  } catch (_) { browserSession = String(Date.now()); }
  const session = document.createElement('input');
  session.type = 'hidden'; session.name = 'browser_session'; session.value = browserSession;
  form.append(session);
  if (form.elements.action.value !== 'full') return;
  form.addEventListener('submit', event => {
    if (!window.confirm('Only do this after the frame has been charging for several hours')) {
      event.preventDefault(); return;
    }
    const confirmation = document.createElement('input');
    confirmation.type = 'hidden'; confirmation.name = 'confirm'; confirmation.value = 'yes';
    form.append(confirmation);
  });
});
