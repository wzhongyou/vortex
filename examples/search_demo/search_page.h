#pragma once

const char* const kSearchPageHTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Vortex Search</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:arial,sans-serif;background:#fff;color:#202124;min-height:100vh}

/* ── Landing (搜索首页) ─────────────────────────────── */
#landing{display:flex;flex-direction:column;align-items:center;padding-top:18vh}
.logo{font-size:92px;font-weight:700;letter-spacing:-3px;margin-bottom:28px;user-select:none;line-height:1}
.logo span:nth-child(1){color:#4285f4}
.logo span:nth-child(2){color:#ea4335}
.logo span:nth-child(3){color:#fbbc05}
.logo span:nth-child(4){color:#4285f4}
.logo span:nth-child(5){color:#34a853}
.logo span:nth-child(6){color:#ea4335}
.search-box{width:584px;max-width:92vw;position:relative}
.search-box input{width:100%;height:44px;border:1px solid #dfe1e5;border-radius:24px;padding:0 48px 0 20px;font-size:16px;outline:none;transition:box-shadow .2s}
.search-box input:focus{box-shadow:0 1px 6px rgba(32,33,36,.28);border-color:rgba(223,225,229,0)}
.search-box .search-icon{position:absolute;right:8px;top:50%;transform:translateY(-50%);width:36px;height:36px;border:none;border-radius:50%;background:#4285f4;color:#fff;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:background .15s}
.search-box .search-icon:hover{background:#1b66c9;box-shadow:0 1px 3px rgba(66,133,244,.5)}
.search-box .search-icon svg{width:20px;height:20px;fill:#fff}
.subtitle{margin-top:18px;color:#70757a;font-size:13px}

/* ── Results Page (搜索结果页) ─────────────────────── */
#results-page{display:none;min-height:100vh}
.top-bar{position:sticky;top:0;background:#fff;padding:12px 24px;display:flex;align-items:center;gap:20px;z-index:100}
.logo-small{font-size:26px;font-weight:700;cursor:pointer;user-select:none;flex-shrink:0;line-height:1}
.logo-small span:nth-child(1){color:#4285f4}
.logo-small span:nth-child(2){color:#ea4335}
.logo-small span:nth-child(3){color:#fbbc05}
.logo-small span:nth-child(4){color:#4285f4}
.logo-small span:nth-child(5){color:#34a853}
.logo-small span:nth-child(6){color:#ea4335}
.top-bar .search-box{flex:1;max-width:692px}
.top-bar .search-box input{height:44px;font-size:16px}

.results-container{max-width:652px;padding:16px 24px 40px 148px;margin:0 auto}
@media(max-width:900px){.results-container{padding-left:24px}}

.stats{color:#70757a;font-size:12px;margin-bottom:16px;line-height:1.5}
.result{margin-bottom:25px}
.result-cite{display:flex;align-items:center;gap:6px;margin-bottom:4px}
.result-cite .favicon{width:16px;height:16px;border-radius:50%;background:#4285f4;color:#fff;font-size:9px;display:flex;align-items:center;justify-content:center;font-weight:700;flex-shrink:0}
.result-cite .site{font-size:12px;color:#202124;line-height:1.3}
.result-cite .site span{color:#70757a}
.result-title a{color:#1a0dab;font-size:20px;text-decoration:none;line-height:1.3;cursor:pointer}
.result-title a:hover{text-decoration:underline}
.result-snippet{color:#4d5156;font-size:14px;line-height:1.58;margin-top:3px}
.result-meta{display:inline-flex;align-items:center;gap:6px;margin-left:8px;vertical-align:middle}
.score-badge{font-size:11px;color:#70757a;background:#f1f3f4;padding:1px 6px;border-radius:3px}
.category-tag{font-size:12px;color:#1a0dab}

/* ── Pagination (搜索分页) ──────────────────────────── */
.pagination{display:flex;align-items:center;justify-content:center;gap:6px;margin-top:30px;padding-bottom:40px}
.pagination a,.pagination span{padding:4px 10px;border-radius:4px;font-size:14px;text-decoration:none;color:#1a0dab;cursor:pointer}
.pagination a:hover{background:#eff5ff}
.pagination .current{color:#202124;font-weight:600;cursor:default;background:#eff5ff}
.pagination .disabled{color:#bbb;cursor:default;pointer-events:none}
.pagination .dots{color:#70757a;cursor:default}

/* ── Modal ─────────────────────────────────────────────── */
.modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.4);z-index:200;justify-content:center;align-items:center}
.modal-overlay.active{display:flex}
.modal{background:#fff;border-radius:8px;padding:24px 28px;max-width:560px;width:90vw;max-height:80vh;overflow-y:auto;box-shadow:0 4px 24px rgba(0,0,0,.15)}
.modal h2{font-size:18px;margin-bottom:14px;color:#202124;font-weight:400}
.modal .field{margin-bottom:10px}
.modal .field-label{font-size:11px;color:#70757a;text-transform:uppercase;letter-spacing:.5px;margin-bottom:2px}
.modal .field-value{font-size:14px;color:#202124;line-height:1.5}
.modal .close-btn{float:right;background:none;border:none;font-size:20px;cursor:pointer;color:#70757a;padding:0 4px;line-height:1}
.modal .close-btn:hover{color:#202124}

/* ── Empty ─────────────────────────────────────────────── */
.empty{text-align:center;padding:60px 20px;color:#70757a;font-size:15px}
.empty .icon{font-size:48px;margin-bottom:12px}

/* ── Add Doc ───────────────────────────────────────────── */
.add-btn{position:fixed;bottom:24px;right:24px;width:48px;height:48px;border-radius:50%;background:#4285f4;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 2px 8px rgba(0,0,0,.15);z-index:90;transition:box-shadow .2s}
.add-btn:hover{box-shadow:0 4px 12px rgba(66,133,244,.4)}
.add-form h3{font-size:18px;margin-bottom:14px;font-weight:400}
.add-form label{display:block;font-size:13px;color:#5f6368;margin-bottom:4px;margin-top:10px}
.add-form input,.add-form textarea{width:100%;border:1px solid #dfe1e5;border-radius:4px;padding:8px 12px;font-size:14px;font-family:inherit;outline:none}
.add-form input:focus,.add-form textarea:focus{border-color:#4285f4;box-shadow:0 0 0 2px rgba(66,133,244,.2)}
.add-form textarea{resize:vertical;min-height:80px}
.add-form .form-actions{margin-top:16px;display:flex;gap:8px;justify-content:flex-end}
.add-form .btn{padding:8px 20px;border-radius:4px;border:none;font-size:14px;cursor:pointer;font-weight:500}
.add-form .btn-primary{background:#4285f4;color:#fff}
.add-form .btn-primary:hover{background:#1b66c9}
.add-form .btn-cancel{background:#f8f9fa;color:#3c4043;border:1px solid #f8f9fa}
.add-form .btn-cancel:hover{background:#f1f3f4;border-color:#f1f3f4}
.add-form .msg{margin-top:8px;font-size:13px;padding:6px 10px;border-radius:4px}
.add-form .msg-ok{background:#e6f4ea;color:#137333}
.add-form .msg-err{background:#fce8e6;color:#c5221f}
</style>
</head>
<body>

<div id="landing">
  <div class="logo">
    <span>V</span><span>o</span><span>r</span><span>t</span><span>e</span><span>x</span>
  </div>
  <form class="search-box" id="form-landing">
    <input type="text" id="q-landing" placeholder="" autofocus>
    <button type="submit" class="search-icon">
      <svg viewBox="0 0 24 24"><path d="M15.5 14h-.79l-.28-.27A6.47 6.47 0 0016 9.5 6.5 6.5 0 109.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z"/></svg>
    </button>
  </form>
  <div class="subtitle">Vortex 倒排索引引擎演示</div>
</div>

<div id="results-page">
  <div class="top-bar">
    <div class="logo-small" id="home-btn">
      <span>V</span><span>o</span><span>r</span><span>t</span><span>e</span><span>x</span>
    </div>
    <form class="search-box" id="form-top">
      <input type="text" id="q-top">
      <button type="submit" class="search-icon">
        <svg viewBox="0 0 24 24"><path d="M15.5 14h-.79l-.28-.27A6.47 6.47 0 0016 9.5 6.5 6.5 0 109.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z"/></svg>
      </button>
    </form>
  </div>
  <div class="results-container">
    <div class="stats" id="stats"></div>
    <div id="results-list"></div>
    <div class="pagination" id="pagination"></div>
  </div>
</div>

<div class="modal-overlay" id="modal-overlay">
  <div class="modal" id="modal">
    <button class="close-btn" id="modal-close">&times;</button>
    <div id="modal-content"></div>
  </div>
</div>

<button class="add-btn" id="add-btn" title="添加文档">+</button>

<div class="modal-overlay" id="add-overlay">
  <div class="modal add-form">
    <button class="close-btn" id="add-close">&times;</button>
    <h3>添加文档</h3>
    <label for="add-title">标题</label>
    <input type="text" id="add-title" placeholder="文档标题">
    <label for="add-content">内容</label>
    <textarea id="add-content" placeholder="文档正文内容..."></textarea>
    <label for="add-category">分类</label>
    <input type="text" id="add-category" placeholder="如：Technology, AI, Programming...">
    <div id="add-msg"></div>
    <div class="form-actions">
      <button class="btn btn-cancel" id="add-cancel-btn">取消</button>
      <button class="btn btn-primary" id="add-submit-btn">添加</button>
    </div>
  </div>
</div>

<script>
const PAGE_SIZE = 10;
let currentQuery = '';
let currentPage = 1;

function showLanding() {
  document.getElementById('landing').style.display = 'flex';
  document.getElementById('results-page').style.display = 'none';
  document.getElementById('q-landing').focus();
  history.pushState(null, '', '/');
}

function showResultsPage() {
  document.getElementById('landing').style.display = 'none';
  document.getElementById('results-page').style.display = 'block';
}

async function doSearch(query, page) {
  if (!query.trim()) return;
  currentQuery = query;
  currentPage = page || 1;

  const url = `/api/search?q=${encodeURIComponent(query)}&page=${currentPage}`;
  try {
    const resp = await fetch(url);
    if (!resp.ok) throw new Error(resp.statusText);
    const data = await resp.json();
    renderResults(data);
    showResultsPage();
    document.getElementById('q-top').value = query;
    history.pushState({query, page: currentPage}, '', `?q=${encodeURIComponent(query)}&page=${currentPage}`);
  } catch (e) {
    document.getElementById('results-list').innerHTML =
      '<div class="empty"><div class="icon">&#9888;</div>搜索出错，请重试</div>';
    document.getElementById('stats').textContent = '';
    document.getElementById('pagination').innerHTML = '';
    showResultsPage();
  }
}

function truncate(s, n) {
  if (!s) return '';
  return s.length > n ? s.slice(0, n) + '...' : s;
}

function renderResults(data) {
  const {query, page, page_size, total_hits, elapsed_ms, results} = data;
  const statsEl = document.getElementById('stats');
  statsEl.textContent = `找到约 ${total_hits} 条结果（${elapsed_ms} 毫秒）`;

  const listEl = document.getElementById('results-list');
  if (!results || results.length === 0) {
    listEl.innerHTML = '<div class="empty">未找到相关结果</div>';
    document.getElementById('pagination').innerHTML = '';
    return;
  }

  listEl.innerHTML = results.map(r => `
    <div class="result">
      <div class="result-cite">
        <div class="favicon">${esc(r.site || r.category || '').charAt(0).toUpperCase()}</div>
        <div class="site">${esc(r.site || r.category || '')} <span>${esc(r.url || '')}</span></div>
      </div>
      <div class="result-title">
        <a href="#" onclick="showDoc('${r.id}');return false">${esc(r.title)}</a>
        <span class="result-meta">
          <span class="score-badge">BM25F ${r.score.toFixed(2)}</span>
        </span>
      </div>
      <div class="result-snippet">${esc(truncate(r.description || '', 200))}</div>
    </div>
  `).join('');

  renderPagination(total_hits, page, page_size);
}

function renderPagination(total, page, pageSize) {
  const el = document.getElementById('pagination');
  const totalPages = Math.max(1, Math.ceil(total / pageSize));
  if (totalPages <= 1) { el.innerHTML = ''; return; }

  let html = '';
  if (page > 1) {
    html += `<a onclick="doSearch(currentQuery, ${page-1})">上一页</a>`;
  } else {
    html += `<span class="disabled">上一页</span>`;
  }

  const range = 2;
  let lastShown = 0;
  for (let i = 1; i <= totalPages; i++) {
    if (i === page) {
      html += `<span class="current">${i}</span>`;
      lastShown = i;
    } else if (Math.abs(i - page) <= range || i === 1 || i === totalPages) {
      if (lastShown && i - lastShown > 1) html += `<span class="dots">...</span>`;
      html += `<a onclick="doSearch(currentQuery, ${i})">${i}</a>`;
      lastShown = i;
    }
  }

  if (page < totalPages) {
    html += `<a onclick="doSearch(currentQuery, ${page+1})">下一页</a>`;
  } else {
    html += `<span class="disabled">下一页</span>`;
  }
  el.innerHTML = html;
}

async function showDoc(id) {
  try {
    const resp = await fetch(`/api/document/${encodeURIComponent(id)}`);
    if (!resp.ok) throw new Error(resp.statusText);
    const doc = await resp.json();
    const content = document.getElementById('modal-content');
    const fields = doc.fields || {};
    let html = `<h2>${esc(fields.title || id)}</h2>`;
    for (const [k, v] of Object.entries(fields)) {
      if (k === 'title') continue;
      html += `<div class="field"><div class="field-label">${esc(k)}</div><div class="field-value">${esc(v)}</div></div>`;
    }
    content.innerHTML = html;
    document.getElementById('modal-overlay').classList.add('active');
  } catch (e) {
    alert('获取文档失败');
  }
}

function closeModal() {
  document.getElementById('modal-overlay').classList.remove('active');
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

// Events
document.getElementById('form-landing').addEventListener('submit', e => {
  e.preventDefault();
  doSearch(document.getElementById('q-landing').value, 1);
});
document.getElementById('form-top').addEventListener('submit', e => {
  e.preventDefault();
  doSearch(document.getElementById('q-top').value, 1);
});
document.getElementById('home-btn').addEventListener('click', showLanding);
document.getElementById('modal-close').addEventListener('click', closeModal);
document.getElementById('modal-overlay').addEventListener('click', e => {
  if (e.target === e.currentTarget) closeModal();
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') { closeModal(); closeAddForm(); }
});

// Add doc form
function openAddForm() {
  document.getElementById('add-overlay').classList.add('active');
  document.getElementById('add-title').focus();
}
function closeAddForm() {
  document.getElementById('add-overlay').classList.remove('active');
  document.getElementById('add-msg').innerHTML = '';
}
document.getElementById('add-btn').addEventListener('click', openAddForm);
document.getElementById('add-close').addEventListener('click', closeAddForm);
document.getElementById('add-cancel-btn').addEventListener('click', closeAddForm);
document.getElementById('add-overlay').addEventListener('click', e => {
  if (e.target === e.currentTarget) closeAddForm();
});

async function submitDoc() {
  const title = document.getElementById('add-title').value.trim();
  const content = document.getElementById('add-content').value.trim();
  const category = document.getElementById('add-category').value.trim();
  const msgEl = document.getElementById('add-msg');

  if (!title && !content) {
    msgEl.innerHTML = '<div class="msg msg-err">请至少填写标题或内容</div>';
    return;
  }

  try {
    const resp = await fetch('/api/document', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({title, content, category})
    });
    const data = await resp.json();
    if (!resp.ok) {
      msgEl.innerHTML = '<div class="msg msg-err">' + esc(data.error || '添加失败') + '</div>';
      return;
    }
    msgEl.innerHTML = '<div class="msg msg-ok">添加成功！ID: ' + esc(data.id) + '</div>';
    document.getElementById('add-title').value = '';
    document.getElementById('add-content').value = '';
    document.getElementById('add-category').value = '';
    setTimeout(() => { closeAddForm(); }, 800);
  } catch (e) {
    msgEl.innerHTML = '<div class="msg msg-err">网络错误</div>';
  }
}
document.getElementById('add-submit-btn').addEventListener('click', submitDoc);

// Browser history
window.addEventListener('popstate', () => {
  const params = new URLSearchParams(location.search);
  const q = params.get('q');
  const p = parseInt(params.get('page')) || 1;
  if (q) {
    doSearch(q, p);
  } else {
    showLanding();
  }
});

// Init from URL
(function() {
  const params = new URLSearchParams(location.search);
  const q = params.get('q');
  if (q) doSearch(q, parseInt(params.get('page')) || 1);
})();
</script>
</body>
</html>
)rawliteral";
