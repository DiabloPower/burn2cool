async function api(path, method='GET', body=null){
  const opts = {method, headers:{}};
  if(body!==null){ opts.headers['Content-Type']='application/json'; opts.body=JSON.stringify(body); }
  const r = await fetch('/api'+path, opts);
  return r.json();
}

let profilesCache = [];

async function refresh(){
  const data = await api('/profiles');
  if(!data.ok){ alert('Failed to load profiles'); return; }
  profilesCache = data.profiles;
  renderList();
}

function renderList(){
  const list = document.getElementById('list');
  const filter = document.getElementById('filter').value.trim().toLowerCase();
  list.innerHTML = '';
  for(const p of profilesCache){
    if(filter && !p.name.toLowerCase().includes(filter) && !p.content.toLowerCase().includes(filter)) continue;
    const el = document.createElement('div');
    el.className = 'profile';
    el.innerHTML = `<strong>${p.name}</strong><div class='preview'>${escapeHtml(p.content).replace(/\n/g,'<br>')}</div>`;
    el.addEventListener('click', ()=>{
      document.getElementById('pname').value = p.name;
      document.getElementById('pcontent').value = p.content;
    });
    const loadBtn = document.createElement('button'); loadBtn.textContent='Load';
    loadBtn.addEventListener('click', async (ev)=>{ ev.stopPropagation(); await loadProfile(p.name); });
    el.appendChild(loadBtn);
    list.appendChild(el);
  }
}

function escapeHtml(str){ return (str||'').replace(/[&<>\"]/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

async function createProfile(){
  const name = document.getElementById('pname').value.trim();
  const content = document.getElementById('pcontent').value;
  if(!name){ alert('Enter profile filename'); return; }
  const r = await api('/profiles', 'POST', {name, content});
  if(!r.ok) alert('Error: '+(r.error||'unknown'));
  else await refresh();
}

async function saveProfile(){
  const name = document.getElementById('pname').value.trim();
  const content = document.getElementById('pcontent').value;
  if(!name){ alert('Enter profile filename'); return; }
  const r = await api('/profiles/'+encodeURIComponent(name), 'PUT', {content});
  if(!r.ok) alert('Error: '+(r.error||'unknown'));
  else await refresh();
}

async function deleteProfile(){
  const name = document.getElementById('pname').value.trim();
  if(!name){ alert('Enter profile filename'); return; }
  if(!confirm('Delete profile '+name+' ?')) return;
  const r = await api('/profiles/'+encodeURIComponent(name), 'DELETE');
  if(!r.ok) alert('Error: '+(r.error||'unknown'));
  else { document.getElementById('pname').value=''; document.getElementById('pcontent').value=''; await refresh(); }
}

async function loadProfile(name){
  // ask server to send load-profile NAME
  const r = await api('/command','POST',{cmd:`load-profile ${name}`});
  if(!r.ok) alert('Error sending load-profile');
  else alert('Daemon response:\n'+r.resp);
}

async function sendCmd(){
  const cmd = document.getElementById('cmdInput').value.trim();
  if(!cmd) return;
  const r = await api('/command','POST',{cmd});
  if(!r.ok) alert('Error');
  else document.getElementById('statusBox').textContent = r.resp;
}

window.addEventListener('DOMContentLoaded', ()=>{
  document.getElementById('refresh').addEventListener('click', refresh);
  document.getElementById('create').addEventListener('click', createProfile);
  document.getElementById('save').addEventListener('click', saveProfile);
  document.getElementById('delete').addEventListener('click', deleteProfile);
  document.getElementById('load').addEventListener('click', ()=>{ const n=document.getElementById('pname').value.trim(); if(n) loadProfile(n); });
  document.getElementById('sendCmd').addEventListener('click', sendCmd);
  document.getElementById('filter').addEventListener('keydown', (e)=>{ if(e.key==='Enter') refresh(); });
  document.getElementById('clearFilter').addEventListener('click', ()=>{ document.getElementById('filter').value=''; refresh(); });
  refresh();
});
