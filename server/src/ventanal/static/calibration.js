// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
'use strict';
(() => {
  function setupUpload() {
    const form = document.getElementById('photo-upload');
    if (!form) return;
    const button = form.querySelector('button');
    const status = document.getElementById('upload-status');
    form.addEventListener('submit', async event => {
      event.preventDefault();
      button.disabled = true;
      status.textContent = 'Uploading photo…';
      try {
        const response = await fetch(form.action, {
          method: 'POST',
          headers: {'X-CSRF-Token': form.dataset.csrf},
          body: new FormData(form),
          redirect: 'manual'
        });
        if (response.type === 'opaqueredirect' || response.status === 403) {
          window.location.assign('/calibration');
          return;
        }
        if (response.status === 413) {
          status.textContent = document.getElementById('upload-hint').textContent;
          return;
        }
        if (!response.ok && response.status !== 400) throw new Error();
        const page = new DOMParser().parseFromString(await response.text(), 'text/html');
        if (!page.getElementById('photo-upload')) throw new Error();
        document.querySelector('main').replaceWith(page.querySelector('main'));
        document.title = page.title;
        // Initialize the new controls explicitly because parsed scripts are inert.
        setupUpload();
        setupPicker();
      } catch {
        status.textContent = 'Unable to upload the photo. Please try again.';
      } finally {
        button.disabled = false;
      }
    });
    button.disabled = false;
  }

  setupUpload();
  setupPicker();

  function setupPicker() {
    const canvas = document.getElementById('chart-picker');
    if (!canvas) return;
    const ctx = canvas.getContext('2d'), photo = new Image();
    const status = document.getElementById('selection-status');
    const labels = ['TL', 'TR', 'BR', 'BL'];
    let points = [], paper = null, mode = 'corners', start = null, cursor = null;
    function draw() {
      ctx.drawImage(photo, 0, 0);
      ctx.lineWidth = 3; ctx.strokeStyle = '#ff7700'; ctx.fillStyle = '#ff7700';
      points.forEach(([x,y],i) => {
        ctx.beginPath(); ctx.arc(x,y,10,0,Math.PI*2); ctx.stroke();
        ctx.font = 'bold 22px sans-serif'; ctx.strokeStyle = '#000'; ctx.lineWidth = 5;
        const tx=Math.max(4,Math.min(canvas.width-65,x+13)), ty=Math.max(25,Math.min(canvas.height-8,y-13));
        ctx.strokeText(`${i+1} ${labels[i]}`,tx,ty); ctx.fillText(`${i+1} ${labels[i]}`,tx,ty);
        ctx.strokeStyle = '#ff7700'; ctx.lineWidth = 3;
      });
      if (paper) { ctx.strokeStyle='#aaaaff'; ctx.strokeRect(...paper); }
      if (cursor) { ctx.strokeStyle='#ffffff'; ctx.strokeRect(cursor[0]-8,cursor[1]-8,16,16); }
      document.getElementById('corners').value = points.map(p=>p.join(',')).join(' ');
      document.getElementById('paper').value = paper ? paper.join(',') : '';
      document.getElementById('measure').disabled = points.length !== 4;
      status.textContent = mode === 'paper' ? 'Drag over white paper. Select Chart corners to return.' : points.length<4 ? `Select corner ${points.length+1}: ${labels[points.length]}` : 'Four corners selected. Preview, or optionally select white paper.';
    }
    function position(event) {
      const rect=canvas.getBoundingClientRect();
      return [Math.max(0,Math.min(canvas.width-1,(event.clientX-rect.left)*canvas.width/rect.width)), Math.max(0,Math.min(canvas.height-1,(event.clientY-rect.top)*canvas.height/rect.height))];
    }
    canvas.addEventListener('pointerdown', event => {
      event.preventDefault(); cursor=null;
      if (mode==='corners') { if (points.length<4) points.push(position(event)); draw(); }
      else { start=position(event); canvas.setPointerCapture(event.pointerId); }
    });
    canvas.addEventListener('pointermove', event => {
      if (!start) return;
      const end=position(event);
      paper=[Math.min(start[0],end[0]),Math.min(start[1],end[1]),Math.abs(end[0]-start[0]),Math.abs(end[1]-start[1])]; draw();
    });
    canvas.addEventListener('pointerup', () => { start=null; if (paper && (paper[2]<1 || paper[3]<1)) paper=null; draw(); });
    canvas.addEventListener('pointercancel', () => { start=null; draw(); });
    canvas.addEventListener('keydown', event => {
      if (!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Enter'].includes(event.key)) return;
      event.preventDefault(); cursor=cursor || [canvas.width/2,canvas.height/2];
      const step=event.shiftKey ? 10 : 1;
      if(event.key==='ArrowLeft') cursor[0]-=step;
      if(event.key==='ArrowRight') cursor[0]+=step;
      if(event.key==='ArrowUp') cursor[1]-=step;
      if(event.key==='ArrowDown') cursor[1]+=step;
      cursor=[Math.max(0,Math.min(canvas.width-1,cursor[0])),Math.max(0,Math.min(canvas.height-1,cursor[1]))];
      if(event.key==='Enter' && points.length<4) points.push([...cursor]);
      draw();
    });
    document.getElementById('corner-mode').onclick=()=>{mode='corners'; draw();};
    document.getElementById('paper-mode').onclick=()=>{mode='paper'; draw();};
    document.getElementById('undo').onclick=()=>{if(mode==='paper') paper=null; else points.pop(); draw();};
    document.getElementById('reset').onclick=()=>{points=[]; paper=null; mode='corners'; cursor=null; draw();};
    photo.onload=()=>{canvas.width=photo.width; canvas.height=photo.height; draw();};
    photo.src=canvas.dataset.photo;
  }
})();
