// MindBridge Demo Dashboard — app.js
// All requests use Content-Type: text/plain to avoid CORS preflight (no OPTIONS handler).

const urlParams = new URLSearchParams(window.location.search);
const pageProtocol = window.location.protocol === 'https:' ? 'https:' : 'http:';
const pageHost = window.location.hostname || '127.0.0.1';
const pageOrigin = window.location.origin || `${pageProtocol}//${pageHost}:5173`;
const GATEWAY = urlParams.get('gateway') || pageOrigin;
const ORCHESTRATOR = urlParams.get('orchestrator') || `${pageOrigin}/proxy/orchestrator`;
const COUNSELOR = urlParams.get('counselor') || `${pageOrigin}/proxy/counselor`;
const EVALUATOR = urlParams.get('evaluator') || `${pageOrigin}/proxy/evaluator`;
const PLATFORM = urlParams.get('platform') || sessionStorage.getItem('mindbridge_platform_url') || `${pageProtocol}//${pageHost}:8077`;
if (urlParams.get('gateway')) {
  sessionStorage.setItem('mindbridge_gateway_url', GATEWAY);
}
if (urlParams.get('platform')) {
  sessionStorage.setItem('mindbridge_platform_url', PLATFORM);
}

let requestId = 0;
let isSending = false;
let authToken = sessionStorage.getItem('mindbridge_auth_token') || '';
let currentUser = JSON.parse(sessionStorage.getItem('mindbridge_user') || 'null');
let authRequired = false;
let videoStream = null;
let audioStream = null;
let mediaRecorder = null;
let audioChunks = [];
let recordedAudioMime = 'audio/webm';
let selectedImageDataUrl = '';
let selectedAudioDataUrl = '';
let latestSnapshotDataUrl = '';
let lastRoundImageDataUrl = '';
let lastRoundAudioDataUrl = '';
let videoCallActive = false;
let snapshotTimer = null;
let audioCaptureStartedAt = 0;
let lastAudioSendAt = 0;
let ttsAudio = null;
let audioContext = null;
let analyserNode = null;
let audioProcessorNode = null;
let audioCaptureSourceNode = null;
let audioMuteGainNode = null;
let audioSampleChunks = [];
let ttsPlaybackContext = null;
let ttsQueue = null;
let asrPausedForTts = false;
let silenceTimer = null;
let monitorTimer = null;
let speechSeen = false;
let lastVoiceAutoSentAt = 0;
let lastMicLevel = 0;
const silenceThreshold = 0.012;
const silenceMsBeforeSend = 1200;
const voiceSendWindowMs = 6000;
const voiceWavSampleRate = 16000;
const storageConversationId = 'demo-session';
const storageChunkSize = 10 * 1024 * 1024;
const storageChunkThreshold = 10 * 1024 * 1024;
let activeView = 'cockpit';
let storageFiles = [];
let storageLoading = false;
let selectedConversationId = '';
let observabilityData = null;
let observabilityTimelineZoom = 1;
let platformWorkspaces = [];
let platformSessions = [];
let selectedPlatformWorkspaceId = '';
let selectedPlatformSessionId = '';

function authQuery() {
  return authToken ? `?auth_token=${encodeURIComponent(authToken)}` : '';
}

function applyAuthUi() {
  const overlay = document.getElementById('login-overlay');
  const label = document.getElementById('auth-user-label');
  const logoutBtn = document.getElementById('logout-btn');
  if (!authRequired) {
    overlay.classList.add('hidden');
    label.textContent = 'Auth optional';
    logoutBtn.disabled = true;
    return;
  }
  if (authToken && currentUser) {
    overlay.classList.add('hidden');
    label.textContent = `${currentUser.display_name || currentUser.user_id} (${currentUser.role || 'user'})`;
    logoutBtn.disabled = false;
  } else {
    overlay.classList.remove('hidden');
    label.textContent = 'Not signed in';
    logoutBtn.disabled = true;
  }
}

async function loadAuthStatus() {
  try {
    const r = await fetch(`${GATEWAY}/api/auth/status`, { mode: 'cors' });
    const data = await r.json();
    authRequired = Boolean(data.required);
    if (authRequired && authToken) {
      try {
        const me = await fetch(`${GATEWAY}/api/auth/me${authQuery()}`, { mode: 'cors' });
        const meData = await me.json();
        if (!me.ok || !meData.ok) throw new Error(meData.error || `HTTP ${me.status}`);
        currentUser = meData.user || currentUser;
        sessionStorage.setItem('mindbridge_user', JSON.stringify(currentUser));
      } catch {
        authToken = '';
        currentUser = null;
        sessionStorage.removeItem('mindbridge_auth_token');
        sessionStorage.removeItem('mindbridge_user');
      }
    }
  } catch {
    authRequired = false;
  }
  applyAuthUi();
}

async function login() {
  const accessKey = document.getElementById('access-key-input').value.trim();
  const secretKey = document.getElementById('secret-key-input').value;
  const errorEl = document.getElementById('login-error');
  errorEl.textContent = '';
  try {
    const r = await fetch(`${GATEWAY}/api/auth/login`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body: JSON.stringify({ access_key_id: accessKey, secret_key: secretKey }),
    });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    authToken = data.token;
    currentUser = data.user || {};
    sessionStorage.setItem('mindbridge_auth_token', authToken);
    sessionStorage.setItem('mindbridge_user', JSON.stringify(currentUser));
    document.getElementById('secret-key-input').value = '';
    applyAuthUi();
    loadConversationHistory();
    loadStorageFiles();
    appendMsg(`Signed in as ${currentUser.display_name || currentUser.user_id}`, 'system');
  } catch (err) {
    errorEl.textContent = err.message;
  }
}

async function logout() {
  if (authToken) {
    try {
      await fetch(`${GATEWAY}/api/auth/logout${authQuery()}`, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain;charset=utf-8' },
        mode: 'cors',
        body: JSON.stringify({ auth_token: authToken }),
      });
    } catch {
      // Local logout still clears the browser session.
    }
  }
  authToken = '';
  currentUser = null;
  sessionStorage.removeItem('mindbridge_auth_token');
  sessionStorage.removeItem('mindbridge_user');
  selectedConversationId = '';
  applyAuthUi();
  loadConversationHistory();
  loadStorageFiles();
}

// ── Health Checks ──

async function checkHealth(baseUrl, chipId) {
  const chip = document.getElementById(chipId);
  try {
    const r = await fetch(`${baseUrl}/api/health`, {
      method: 'GET',
      mode: 'cors',
    });
    if (r.ok) {
      chip.className = 'status-chip ok';
    } else {
      chip.className = 'status-chip err';
    }
  } catch {
    chip.className = 'status-chip err';
  }
}

function refreshHealth() {
  checkHealth(GATEWAY, 'st-gateway');
  checkHealth(ORCHESTRATOR, 'st-orchestrator');
  checkHealth(COUNSELOR, 'st-counselor');
  checkHealth(EVALUATOR, 'st-evaluator');
}

// ── Feature Status ──

async function loadFeatureStatus() {
  const tbody = document.getElementById('feature-tbody');
  try {
    const r = await fetch(`${GATEWAY}/api/demo/feature-status`, { mode: 'cors' });
    if (!r.ok) throw new Error('not ok');
    const data = await r.json();
    const features = data.features || [];
    tbody.innerHTML = '';
    for (const f of features) {
      const tr = document.createElement('tr');
      const statusClass = f.status === 'implemented' ? 'implemented' : 'partial';
      tr.innerHTML = `
        <td style="font-family:var(--font-mono);font-size:0.75rem;color:var(--text-primary)">${esc(f.name)}</td>
        <td><span class="feature-status-badge ${statusClass}">${esc(f.status)}</span></td>
        <td style="font-size:0.72rem">${esc(f.next_step || '-')}</td>
      `;
      tbody.appendChild(tr);
    }
  } catch {
    tbody.innerHTML = '<tr><td colspan="3" style="color:var(--text-muted);text-align:center;">Could not load feature status</td></tr>';
  }
}

// ── Chat Helpers ──

function appendMsg(text, cls) {
  const box = document.getElementById('chat-messages');
  const div = document.createElement('div');
  div.className = `chat-msg ${cls}`;
  div.textContent = text;
  box.appendChild(div);
  box.scrollTop = box.scrollHeight;
  return div;
}

function formatAgentError(error) {
  const raw = typeof error === 'string' ? error : JSON.stringify(error || {});
  if (raw.includes('InvalidApiKey') || raw.includes('invalid_api_key') ||
      (raw.includes('dashscope HTTP 401') && raw.includes('API'))) {
    return 'DashScope API key is invalid. Generate a new DashScope/Bailian API key, then restart with scripts/start_demo_dashscope.sh. The key is read from environment input and is not saved.';
  }
  if (typeof error === 'object' && error && error.message) {
    return `Error: ${error.message}`;
  }
  return `Error: ${raw}`;
}

function displayConversationId(id) {
  if (!id) return 'default';
  if (id.startsWith('anonymous__')) return id.slice('anonymous__'.length);
  if (currentUser?.user_id && id.startsWith(`${currentUser.user_id}__`)) {
    return id.slice(currentUser.user_id.length + 2);
  }
  return id;
}

function formatHistoryTime(value) {
  const n = Number(value || 0);
  if (!n) return '';
  const ms = n < 1000000000000 ? n * 1000 : n;
  return new Date(ms).toLocaleString();
}

async function loadConversationHistory() {
  const list = document.getElementById('conversation-list');
  const turns = document.getElementById('conversation-turns');
  if (!list || !turns) return;
  try {
    const r = await fetch(`${GATEWAY}/api/conversations${authQuery()}`, { mode: 'cors' });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    const conversations = data.conversations || [];
    if (conversations.length === 0) {
      list.innerHTML = '<div class="history-empty">No stored conversations yet</div>';
      turns.innerHTML = '';
      selectedConversationId = '';
      return;
    }
    if (!selectedConversationId || !conversations.some((item) => item.conversation_id === selectedConversationId)) {
      selectedConversationId = conversations[0].conversation_id;
    }
    list.innerHTML = '';
    for (const item of conversations) {
      const btn = document.createElement('button');
      btn.className = `history-item${item.conversation_id === selectedConversationId ? ' active' : ''}`;
      btn.onclick = () => loadConversationTurns(item.conversation_id);
      btn.innerHTML = `
        <div>${esc(displayConversationId(item.conversation_id))}</div>
        <div class="history-meta">${Number(item.turns || 0)} turns · ${esc(formatHistoryTime(item.updated_at))}</div>
      `;
      list.appendChild(btn);
    }
    await loadConversationTurns(selectedConversationId, false);
  } catch (err) {
    list.innerHTML = `<div class="history-empty">History unavailable: ${esc(err.message)}</div>`;
    turns.innerHTML = '';
  }
}

async function loadConversationTurns(conversationId, refreshList = true) {
  selectedConversationId = conversationId;
  if (refreshList) {
    document.querySelectorAll('.history-item').forEach((item) => item.classList.remove('active'));
    const buttons = Array.from(document.querySelectorAll('.history-item'));
    const index = buttons.findIndex((item) => item.textContent.includes(displayConversationId(conversationId)));
    if (index >= 0) buttons[index].classList.add('active');
  }
  const turns = document.getElementById('conversation-turns');
  if (!turns || !conversationId) return;
  try {
    const r = await fetch(`${GATEWAY}/api/conversations/${encodeURIComponent(conversationId)}${authQuery()}`, { mode: 'cors' });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    const history = data.turns || [];
    if (history.length === 0) {
      turns.innerHTML = '<div class="history-empty">No turns in this conversation</div>';
      return;
    }
    turns.innerHTML = history.slice(-8).map((turn) => `
      <div class="history-turn">
        <span class="history-role">${esc(turn.role || '-')}</span>
        ${esc(turn.content || '')}
      </div>
    `).join('');
    turns.scrollTop = turns.scrollHeight;
  } catch (err) {
    turns.innerHTML = `<div class="history-empty">Could not load turns: ${esc(err.message)}</div>`;
  }
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function base64ToBytes(base64) {
  const binary = atob(base64 || '');
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function bytesToBase64(bytes) {
  let binary = '';
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

function extensionForMime(mime) {
  const clean = String(mime || '').split(';')[0].toLowerCase();
  if (clean === 'image/jpeg') return 'jpg';
  if (clean === 'image/png') return 'png';
  if (clean === 'audio/wav' || clean === 'audio/wave' || clean === 'audio/x-wav') return 'wav';
  if (clean === 'audio/mpeg') return 'mp3';
  const tail = clean.includes('/') ? clean.split('/').pop() : 'bin';
  return (tail || 'bin').replace(/[^a-z0-9]+/g, '') || 'bin';
}

function formatBytes(bytes) {
  const n = Number(bytes || 0);
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(2)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function storageUserId() {
  return currentUser?.user_id || 'anonymous';
}

function storageQuery() {
  const prefix = authToken ? `${authQuery()}&` : '?';
  return `${prefix}user=${encodeURIComponent(storageUserId())}&conversation_id=${encodeURIComponent(storageConversationId)}`;
}

function fileDisplayName(file) {
  return file.file_name || file.filename || file.name || file.md5 || 'file';
}

function fileType(file) {
  const raw = String(file.type || file.mime_type || '');
  if (raw.includes('/')) return raw.split('/').pop() || raw;
  const name = fileDisplayName(file);
  const ext = name.includes('.') ? name.split('.').pop() : raw;
  return (ext || 'file').toLowerCase();
}

function isImageStorageFile(file) {
  const type = String(file.type || file.mime_type || '').toLowerCase();
  const ext = fileType(file);
  return type.startsWith('image/') || ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp', 'svg', 'ico'].includes(ext);
}

function storageUploadedAt(file) {
  return file.create_time || file.created_at || file.updated_at || file.upload_time || '';
}

function setStorageStatus(text) {
  const sidebar = document.getElementById('storage-status');
  const dashboard = document.getElementById('storage-dashboard-status');
  if (sidebar) sidebar.textContent = text;
  if (dashboard) dashboard.textContent = text;
}

function setStorageProgress(percent) {
  const bar = document.getElementById('storage-progress-bar');
  if (bar) bar.style.width = `${Math.max(0, Math.min(100, percent))}%`;
}

function switchView(view) {
  activeView = ['platform', 'storage', 'observability'].includes(view) ? view : 'cockpit';
  document.getElementById('cockpit-view')?.classList.toggle('hidden', activeView !== 'cockpit');
  document.getElementById('platform-dashboard')?.classList.toggle('hidden', activeView !== 'platform');
  document.getElementById('storage-dashboard')?.classList.toggle('hidden', activeView !== 'storage');
  document.getElementById('observability-dashboard')?.classList.toggle('hidden', activeView !== 'observability');
  document.getElementById('tab-cockpit')?.classList.toggle('active', activeView === 'cockpit');
  document.getElementById('tab-platform')?.classList.toggle('active', activeView === 'platform');
  document.getElementById('tab-storage')?.classList.toggle('active', activeView === 'storage');
  document.getElementById('tab-observability')?.classList.toggle('active', activeView === 'observability');
  if (activeView === 'platform') loadPlatformDashboard();
  if (activeView === 'storage') loadStorageFiles();
  if (activeView === 'observability') loadObservability();
}

function platformUserId() {
  return currentUser?.user_id || storageUserId();
}

function platformConversationId() {
  if (!selectedPlatformSessionId) return storageConversationId;
  const session = platformSessions.find((item) => item.session_id === selectedPlatformSessionId);
  return session?.conversation_id || storageConversationId;
}

function setPlatformStatus(text) {
  const el = document.getElementById('platform-status');
  if (el) el.textContent = text;
}

function savePlatformUrl() {
  const value = document.getElementById('platform-url-input')?.value?.trim();
  if (!value) {
    setPlatformStatus('enter a platform URL first');
    return;
  }
  sessionStorage.setItem('mindbridge_platform_url', value);
  setPlatformStatus('platform URL saved; reload the page to apply it');
}

async function platformFetch(path, options = {}) {
  const r = await fetch(`${PLATFORM}${path}`, {
    mode: 'cors',
    headers: { 'Content-Type': 'text/plain;charset=utf-8', ...(options.headers || {}) },
    ...options,
  });
  const data = await r.json();
  if (!r.ok || data.ok === false) {
    throw new Error(data.error?.message || data.error || `HTTP ${r.status}`);
  }
  return data.data || data;
}

async function loadPlatformDashboard() {
  const urlInput = document.getElementById('platform-url-input');
  if (urlInput && !urlInput.value) urlInput.value = PLATFORM;
  setPlatformStatus('loading platform inventory');
  try {
    const [workspaceData, sessionData] = await Promise.all([
      platformFetch('/api/platform/workspaces'),
      platformFetch('/api/platform/sessions'),
    ]);
    platformWorkspaces = workspaceData.workspaces || [];
    platformSessions = sessionData.sessions || [];
    if (!selectedPlatformWorkspaceId && platformWorkspaces.length) {
      selectedPlatformWorkspaceId = platformWorkspaces[0].workspace_id;
    }
    if (!selectedPlatformSessionId && platformSessions.length) {
      selectedPlatformSessionId = platformSessions[0].session_id;
    }
    renderPlatformDashboard();
    if (selectedPlatformSessionId) {
      await loadPlatformSession(selectedPlatformSessionId);
    } else {
      renderPlatformEvents([]);
      renderPlatformArtifacts([]);
    }
    setPlatformStatus('platform inventory loaded');
  } catch (err) {
    setPlatformStatus(`platform unavailable: ${err.message}`);
    renderPlatformError(err.message);
  }
}

function renderPlatformDashboard() {
  document.getElementById('platform-workspace-count').textContent = String(platformWorkspaces.length);
  document.getElementById('platform-session-count').textContent = String(platformSessions.length);
  renderPlatformWorkspaces();
  renderPlatformSessions();
}

function renderPlatformError(message) {
  const workspaceList = document.getElementById('platform-workspace-list');
  const sessionList = document.getElementById('platform-session-list');
  if (workspaceList) workspaceList.innerHTML = `<div class="storage-empty">Platform unavailable: ${esc(message)}</div>`;
  if (sessionList) sessionList.innerHTML = '<div class="storage-empty">No sessions loaded</div>';
  document.getElementById('platform-event-count').textContent = '0';
  document.getElementById('platform-artifact-count').textContent = '0';
}

function renderPlatformWorkspaces() {
  const list = document.getElementById('platform-workspace-list');
  if (!list) return;
  if (!platformWorkspaces.length) {
    list.innerHTML = '<div class="storage-empty">No workspaces yet</div>';
    return;
  }
  list.innerHTML = '';
  for (const workspace of platformWorkspaces) {
    const row = document.createElement('button');
    row.className = `platform-session-row${workspace.workspace_id === selectedPlatformWorkspaceId ? ' active' : ''}`;
    row.onclick = () => {
      selectedPlatformWorkspaceId = workspace.workspace_id;
      renderPlatformDashboard();
    };
    row.innerHTML = `
      <div>
        <div class="platform-row-title">${esc(workspace.workspace_id || '-')}</div>
        <div class="platform-muted">${esc(workspace.namespace || workspace.namespace_name || '-')}</div>
      </div>
      <span class="feature-status-badge implemented">${esc(workspace.owner_user_id || 'anonymous')}</span>
    `;
    list.appendChild(row);
  }
}

function renderPlatformSessions() {
  const list = document.getElementById('platform-session-list');
  if (!list) return;
  const sessions = platformSessions.filter((session) => !selectedPlatformWorkspaceId || session.workspace_id === selectedPlatformWorkspaceId);
  if (!sessions.length) {
    list.innerHTML = '<div class="storage-empty">No sessions for this workspace</div>';
    return;
  }
  list.innerHTML = '';
  for (const session of sessions) {
    const row = document.createElement('button');
    row.className = `platform-session-row${session.session_id === selectedPlatformSessionId ? ' active' : ''}`;
    row.onclick = () => loadPlatformSession(session.session_id);
    row.innerHTML = `
      <div>
        <div class="platform-row-title">${esc(session.session_id || '-')}</div>
        <div class="platform-muted">${esc(session.conversation_id || '-')} · run ${esc(session.last_run_id || 'none')}</div>
      </div>
      <span class="feature-status-badge ${session.status === 'running' ? 'implemented' : 'partial'}">${esc(session.status || '-')}</span>
    `;
    list.appendChild(row);
  }
}

async function createPlatformWorkspace() {
  const input = document.getElementById('platform-workspace-id');
  const ownerInput = document.getElementById('platform-owner-id');
  const workspaceId = input?.value?.trim() || `ws-${Date.now()}`;
  const owner = ownerInput?.value?.trim() || platformUserId();
  setPlatformStatus('creating workspace');
  try {
    await platformFetch('/api/platform/workspaces', {
      method: 'POST',
      body: JSON.stringify({ workspace_id: workspaceId, owner_user_id: owner }),
    });
    selectedPlatformWorkspaceId = workspaceId;
    if (input) input.value = '';
    await loadPlatformDashboard();
  } catch (err) {
    setPlatformStatus(`workspace create failed: ${err.message}`);
  }
}

async function createPlatformSession() {
  if (!selectedPlatformWorkspaceId) {
    setPlatformStatus('create or select a workspace first');
    return;
  }
  const sessionInput = document.getElementById('platform-session-id');
  const conversationInput = document.getElementById('platform-conversation-id');
  const sessionId = sessionInput?.value?.trim() || `session-${Date.now()}`;
  const conversationId = conversationInput?.value?.trim() || selectedConversationId || storageConversationId;
  setPlatformStatus('starting platform session');
  try {
    await platformFetch('/api/platform/sessions', {
      method: 'POST',
      body: JSON.stringify({
        session_id: sessionId,
        workspace_id: selectedPlatformWorkspaceId,
        user_id: platformUserId(),
        conversation_id: conversationId,
      }),
    });
    selectedPlatformSessionId = sessionId;
    if (sessionInput) sessionInput.value = '';
    await loadPlatformDashboard();
  } catch (err) {
    setPlatformStatus(`session start failed: ${err.message}`);
  }
}

async function loadPlatformSession(sessionId) {
  selectedPlatformSessionId = sessionId;
  renderPlatformSessions();
  const label = document.getElementById('platform-selected-session');
  if (label) label.textContent = `Session ${sessionId}`;
  try {
    const [eventData, artifactData] = await Promise.all([
      platformFetch(`/api/platform/sessions/${encodeURIComponent(sessionId)}/events`),
      platformFetch(`/api/platform/sessions/${encodeURIComponent(sessionId)}/artifacts`),
    ]);
    renderPlatformEvents(eventData.events || []);
    renderPlatformArtifacts(artifactData.artifacts || []);
  } catch (err) {
    renderPlatformEvents([]);
    renderPlatformArtifacts([]);
    setPlatformStatus(`session load failed: ${err.message}`);
  }
}

async function attachPlatformRun(runId) {
  if (!selectedPlatformSessionId || !runId) return;
  try {
    const data = await platformFetch(`/api/platform/sessions/${encodeURIComponent(selectedPlatformSessionId)}/attach-run`, {
      method: 'POST',
      body: JSON.stringify({ run_id: runId }),
    });
    const updated = data.session;
    if (updated) {
      const index = platformSessions.findIndex((item) => item.session_id === updated.session_id);
      if (index >= 0) {
        platformSessions[index] = updated;
      } else {
        platformSessions.unshift(updated);
      }
      renderPlatformSessions();
    }
    if (activeView === 'platform') {
      await loadPlatformSession(selectedPlatformSessionId);
    }
  } catch (err) {
    setPlatformStatus(`run attach failed: ${err.message}`);
  }
}

async function refreshSelectedPlatformSession() {
  if (!selectedPlatformSessionId) {
    setPlatformStatus('select a session first');
    return;
  }
  await loadPlatformSession(selectedPlatformSessionId);
}

function renderPlatformEvents(events) {
  const list = document.getElementById('platform-event-list');
  document.getElementById('platform-event-count').textContent = String(events.length);
  if (!list) return;
  if (!events.length) {
    list.innerHTML = '<div class="storage-empty">No Universal Events for this session yet</div>';
    return;
  }
  list.innerHTML = '';
  for (const event of events.slice(-80)) {
    const row = document.createElement('div');
    row.className = 'platform-event-row';
    row.innerHTML = `
      <div>
        <div class="platform-row-title">${esc(event.type || '-')}</div>
        <div class="platform-muted">${esc(event.source || '-')} · ${esc(event.run_id || 'no run')}</div>
      </div>
      <span class="platform-event-time">${event.timestamp_ms ? esc(new Date(event.timestamp_ms).toLocaleTimeString()) : '-'}</span>
    `;
    list.appendChild(row);
  }
}

function renderPlatformArtifacts(artifacts) {
  const list = document.getElementById('platform-artifact-list');
  document.getElementById('platform-artifact-count').textContent = String(artifacts.length);
  if (!list) return;
  if (!artifacts.length) {
    list.innerHTML = '<div class="storage-empty">No controlled artifacts for this session yet</div>';
    return;
  }
  list.innerHTML = '';
  for (const artifact of artifacts) {
    const row = document.createElement('div');
    row.className = 'platform-artifact-row';
    row.innerHTML = `
      <div>
        <div class="platform-row-title">${esc(artifact.name || '-')}</div>
        <div class="platform-muted">${esc(artifact.path || '')}</div>
      </div>
      <span class="platform-event-time">${esc(formatBytes(artifact.size_bytes || 0))}</span>
    `;
    list.appendChild(row);
  }
}

async function storageDigest(file) {
  const buffer = await file.arrayBuffer();
  const bytes = new Uint8Array(buffer);
  if (window.crypto?.subtle) {
    const digest = await crypto.subtle.digest('SHA-256', bytes);
    return {
      md5: Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, '0')).join(''),
      bytes,
    };
  }
  return {
    md5: `${file.name}-${file.size}-${file.lastModified}`.replace(/[^a-zA-Z0-9_-]/g, '_'),
    bytes,
  };
}

async function digestBytes(bytes, fallbackName) {
  if (window.crypto?.subtle) {
    const digest = await crypto.subtle.digest('SHA-256', bytes);
    return Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, '0')).join('');
  }
  return `${fallbackName}-${bytes.length}`.replace(/[^a-zA-Z0-9_-]/g, '_');
}

async function uploadAttachment() {
  const input = document.getElementById('storage-file-input');
  const file = input?.files?.[0];
  if (!file) {
    setStorageStatus('choose a file first');
    return;
  }
  await uploadStorageFile(file, document.getElementById('storage-upload-btn'));
  input.value = '';
}

async function uploadDashboardAttachment(event) {
  const file = event?.target?.files?.[0];
  if (!file) return;
  await uploadStorageFile(file);
  event.target.value = '';
}

async function uploadStorageFile(file, button = null) {
  if (button) button.disabled = true;
  setStorageProgress(0);
  setStorageStatus('hashing');
  try {
    const { md5, bytes } = await storageDigest(file);
    if (file.size > storageChunkThreshold) {
      await uploadChunkedStorageFile(file, md5, bytes);
    } else {
      await uploadBase64StorageFile(file, md5, bytes);
    }
    setStorageProgress(100);
    await loadStorageFiles();
  } catch (err) {
    setStorageStatus(`upload failed: ${err.message}`);
  } finally {
    if (button) button.disabled = false;
    setTimeout(() => setStorageProgress(0), 1400);
  }
}

async function uploadBase64StorageFile(file, md5, bytes) {
  setStorageStatus('uploading');
  setStorageProgress(45);
  const r = await fetch(`${GATEWAY}/api/storage/upload${authQuery()}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain;charset=utf-8' },
    mode: 'cors',
    body: JSON.stringify({
      user: storageUserId(),
      conversation_id: storageConversationId,
      filename: file.name,
      md5,
      type: file.type || 'application/octet-stream',
      data_base64: bytesToBase64(bytes),
    }),
  });
  const data = await r.json();
  if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
  setStorageStatus(data.instant ? 'instant upload complete' : 'upload complete');
}

async function uploadGeneratedStorageAsset(part, prefix) {
  if (!part?.data_base64) return;
  try {
    const bytes = base64ToBytes(part.data_base64);
    if (!bytes.length) return;
    const stamp = new Date().toISOString().replace(/[:.]/g, '-');
    const type = part.mime_type || 'application/octet-stream';
    const filename = `mindbridge-${prefix}-${stamp}.${extensionForMime(type)}`;
    const md5 = await digestBytes(bytes, filename);
    setStorageStatus(`saving ${prefix}`);
    const r = await fetch(`${GATEWAY}/api/storage/upload${authQuery()}`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body: JSON.stringify({
        user: storageUserId(),
        conversation_id: storageConversationId,
        filename,
        md5,
        type,
        data_base64: part.data_base64,
      }),
    });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    await loadStorageFiles();
  } catch (err) {
    setStorageStatus(`${prefix} save failed: ${err.message}`);
  }
}

async function uploadChunkedStorageFile(file, md5, bytes) {
  const chunkCount = Math.ceil(file.size / storageChunkSize);
  setStorageStatus(`starting ${chunkCount} chunks`);
  const init = await fetch(`${GATEWAY}/api/storage/chunks/init${authQuery()}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain;charset=utf-8' },
    mode: 'cors',
    body: JSON.stringify({
      user: storageUserId(),
      conversation_id: storageConversationId,
      filename: file.name,
      md5,
      type: file.type || 'application/octet-stream',
      size: file.size,
      chunkCount,
    }),
  });
  const initData = await init.json();
  if (!init.ok || !initData.ok) throw new Error(initData.error || `HTTP ${init.status}`);
  const uploadId = initData.upload_id || '';
  const uploaded = new Set(String(initData.uploadedChunks || initData.uploaded || '')
    .split(',')
    .map((x) => Number.parseInt(x, 10))
    .filter((x) => Number.isInteger(x)));
  for (let i = 0; i < chunkCount; i++) {
    if (!uploaded.has(i)) {
      const start = i * storageChunkSize;
      const end = Math.min(file.size, start + storageChunkSize);
      const uploadUrl = `${GATEWAY}/api/storage/chunks/upload${authQuery()}${authToken ? '&' : '?'}upload_id=${encodeURIComponent(uploadId)}&md5=${encodeURIComponent(md5)}&index=${i}`;
      const r = await fetch(uploadUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        mode: 'cors',
        body: bytes.subarray(start, end),
      });
      const data = await r.json();
      if (!r.ok || !data.ok) throw new Error(data.error || `chunk ${i} failed`);
    }
    const progress = Math.round(((i + 1) / chunkCount) * 88);
    setStorageProgress(progress);
    setStorageStatus(`uploaded chunk ${i + 1}/${chunkCount}`);
  }
  const merge = await fetch(`${GATEWAY}/api/storage/chunks/merge${authQuery()}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain;charset=utf-8' },
    mode: 'cors',
    body: JSON.stringify({
      user: storageUserId(),
      conversation_id: storageConversationId,
      upload_id: uploadId,
      md5,
      filename: file.name,
      type: file.type || 'application/octet-stream',
    }),
  });
  const mergeData = await merge.json();
  if (!merge.ok || !mergeData.ok) throw new Error(mergeData.error || `HTTP ${merge.status}`);
  setStorageStatus('chunked upload complete');
}

async function loadStorageFiles() {
  const list = document.getElementById('storage-list');
  if (storageLoading) return;
  storageLoading = true;
  setStorageStatus('loading files');
  try {
    const r = await fetch(`${GATEWAY}/api/storage/files${storageQuery()}`, {
      mode: 'cors',
    });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    storageFiles = data.files || [];
    renderStorageSidebar(list);
    renderStorageDashboard();
    setStorageStatus(storageFiles.length ? `${storageFiles.length} files loaded` : 'No attachments');
  } catch (err) {
    if (list) list.innerHTML = `<div class="storage-meta">Storage unavailable: ${esc(err.message)}</div>`;
    storageFiles = [];
    renderStorageDashboard();
    setStorageStatus(`storage unavailable: ${err.message}`);
  } finally {
    storageLoading = false;
  }
}

function renderStorageSidebar(list = document.getElementById('storage-list')) {
  if (!list) return;
  list.innerHTML = '';
  for (const file of storageFiles.slice(0, 6)) {
    const item = document.createElement('div');
    item.className = 'storage-item';
    item.innerHTML = `
      <div>
        <div class="storage-name">${esc(fileDisplayName(file))}</div>
        <div class="storage-meta">${esc(formatBytes(file.size))}</div>
      </div>
      <button class="media-btn" onclick="downloadAttachment('${esc(file.md5)}')">Open</button>
    `;
    list.appendChild(item);
  }
  if (!list.children.length) {
    list.innerHTML = '<div class="storage-meta">No attachments</div>';
  }
}

function renderStorageDashboard() {
  const totalFiles = document.getElementById('storage-total-files');
  if (!totalFiles) return;
  const query = (document.getElementById('storage-search-input')?.value || '').trim().toLowerCase();
  const files = storageFiles.filter((file) => {
    if (!query) return true;
    return `${fileDisplayName(file)} ${fileType(file)} ${file.md5 || ''}`.toLowerCase().includes(query);
  });
  const totalSize = storageFiles.reduce((sum, file) => sum + Number(file.size || 0), 0);
  const imageCount = storageFiles.filter(isImageStorageFile).length;
  totalFiles.textContent = String(storageFiles.length);
  document.getElementById('storage-total-size').textContent = formatBytes(totalSize);
  document.getElementById('storage-image-count').textContent = String(imageCount);
  document.getElementById('storage-other-count').textContent = String(Math.max(0, storageFiles.length - imageCount));
  renderStorageRecent(files);
  renderStorageTable(files);
}

function renderStorageRecent(files) {
  const recent = document.getElementById('storage-recent-list');
  if (!recent) return;
  recent.innerHTML = '';
  const ordered = [...files].sort((a, b) => String(storageUploadedAt(b)).localeCompare(String(storageUploadedAt(a)))).slice(0, 5);
  if (!ordered.length) {
    recent.innerHTML = '<div class="storage-empty">No matching uploads</div>';
    return;
  }
  for (const file of ordered) {
    const row = document.createElement('div');
    row.className = 'storage-file-row';
    row.innerHTML = storageFileRowHtml(file, false);
    recent.appendChild(row);
  }
}

function renderStorageTable(files) {
  const table = document.getElementById('storage-file-table');
  if (!table) return;
  table.innerHTML = '';
  if (!files.length) {
    table.innerHTML = '<div class="storage-empty">No files match this view</div>';
    return;
  }
  const header = document.createElement('div');
  header.className = 'storage-file-row header';
  header.innerHTML = '<div></div><div>Name</div><div>Type</div><div>Size</div><div>Action</div>';
  table.appendChild(header);
  for (const file of files) {
    const row = document.createElement('div');
    row.className = 'storage-file-row';
    row.innerHTML = storageFileRowHtml(file, true);
    table.appendChild(row);
  }
}

function storageFileRowHtml(file, includeType) {
  const name = fileDisplayName(file);
  const type = fileType(file);
  const thumb = isImageStorageFile(file) && file.url
    ? `<img src="${esc(file.url)}" alt="">`
    : esc(type.slice(0, 4).toUpperCase());
  return `
    <div class="storage-file-icon">${thumb}</div>
    <div>
      <div class="storage-file-name">${esc(name)}</div>
      <div class="storage-file-sub">${esc(file.md5 || storageUploadedAt(file) || '-')}</div>
    </div>
    <div class="storage-file-sub storage-file-type">${includeType ? esc(type.toUpperCase()) : esc(storageUploadedAt(file) || 'recent')}</div>
    <div class="storage-file-sub storage-file-size">${esc(formatBytes(file.size))}</div>
    <button class="storage-action-btn" onclick="downloadAttachment('${esc(file.md5)}')">Open</button>
  `;
}

async function downloadAttachment(md5) {
  try {
    const r = await fetch(`${GATEWAY}/api/storage/files/${encodeURIComponent(md5)}/download${storageQuery()}`, {
      mode: 'cors',
    });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    const bytes = base64ToBytes(data.data_base64);
    const blob = new Blob([bytes], { type: data.mime_type || data.file?.type || 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    window.open(url, '_blank', 'noopener');
    setStorageStatus('file opened');
    setTimeout(() => URL.revokeObjectURL(url), 60000);
  } catch (err) {
    setStorageStatus(`download failed: ${err.message}`);
  }
}

function initStorageDashboard() {
  const dropZone = document.getElementById('storage-drop-zone');
  if (!dropZone) return;
  for (const eventName of ['dragenter', 'dragover']) {
    dropZone.addEventListener(eventName, (event) => {
      event.preventDefault();
      dropZone.classList.add('dragover');
    });
  }
  for (const eventName of ['dragleave', 'drop']) {
    dropZone.addEventListener(eventName, (event) => {
      event.preventDefault();
      dropZone.classList.remove('dragover');
    });
  }
  dropZone.addEventListener('drop', (event) => {
    const file = event.dataTransfer?.files?.[0];
    if (file) uploadStorageFile(file);
  });
}

async function readSse(response, onEvent) {
  const reader = response.body?.getReader?.();
  if (!reader) throw new Error('streaming response unavailable');
  const decoder = new TextDecoder();
  let pending = '';
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    pending += decoder.decode(value, { stream: true });
    let idx;
    while ((idx = pending.indexOf('\n\n')) !== -1) {
      const block = pending.slice(0, idx);
      pending = pending.slice(idx + 2);
      const payload = block.split(/\r?\n/)
        .filter((line) => line.startsWith('data:'))
        .map((line) => line.slice(5).trimStart())
        .join('\n');
      if (!payload) continue;
      await onEvent(JSON.parse(payload));
    }
  }
}

function pauseAsrForTts() {
  asrPausedForTts = true;
  if (silenceTimer) {
    clearTimeout(silenceTimer);
    silenceTimer = null;
  }
  speechSeen = false;
  updateCallStatus('tts speaking');
}

function resumeAsrAfterTts() {
  setTimeout(() => {
    asrPausedForTts = false;
    updateCallStatus('tts finished');
  }, 1000);
}

class TtsPlaybackQueue {
  constructor() {
    this.items = [];
    this.processing = false;
    this.scheduleTime = 0;
  }

  enqueue(text) {
    const clean = String(text || '').trim();
    if (!videoCallActive || !clean) return;
    this.items.push(clean);
    this.process();
  }

  async process() {
    if (this.processing) return;
    this.processing = true;
    pauseAsrForTts();
    try {
      while (this.items.length > 0) {
        const text = this.items.shift();
        try {
          await this.streamOne(text);
        } catch (err) {
          updateCallStatus(`tts failed: ${err.message}`);
          break;
        }
      }
    } finally {
      this.processing = false;
      resumeAsrAfterTts();
    }
  }

  async streamOne(text) {
    updateCallStatus('tts streaming');
    const r = await fetch(`${GATEWAY}/api/demo/tts/stream`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body: JSON.stringify({ text, format: 'pcm' }),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    await readSse(r, async (event) => {
      if (event.event === 'audio' && event.data_base64) {
        this.playPcm(base64ToBytes(event.data_base64), event.sample_rate || 22050);
      } else if (event.event === 'tts_error') {
        throw new Error(event.message || 'tts failed');
      }
    });
  }

  playPcm(bytes, sampleRate) {
    if (!bytes.length) return;
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!ttsPlaybackContext && AudioContextClass) {
      ttsPlaybackContext = new AudioContextClass();
    }
    if (!ttsPlaybackContext) return;
    ttsPlaybackContext.resume?.().catch(() => {});
    const sampleCount = Math.floor(bytes.length / 2);
    const buffer = ttsPlaybackContext.createBuffer(1, sampleCount, sampleRate);
    const channel = buffer.getChannelData(0);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (let i = 0; i < sampleCount; i++) {
      channel[i] = view.getInt16(i * 2, true) / 32768;
    }
    const source = ttsPlaybackContext.createBufferSource();
    source.buffer = buffer;
    source.connect(ttsPlaybackContext.destination);
    this.scheduleTime = Math.max(this.scheduleTime, ttsPlaybackContext.currentTime + 0.02);
    source.start(this.scheduleTime);
    this.scheduleTime += buffer.duration;
  }
}

function enqueueStreamingTts(text) {
  if (!ttsQueue) ttsQueue = new TtsPlaybackQueue();
  ttsQueue.enqueue(text);
}

function takeSpeakablePrefix(buffer, force = false) {
  const match = buffer.match(/^([\s\S]*?[。！？!?；;]\s*)/);
  if (match) return match[1];
  if (force && buffer.trim()) return buffer;
  if (buffer.length >= 60) return buffer.slice(0, 60);
  return '';
}

// ── Media Capture ──

function setMediaStatus(text) {
  const el = document.getElementById('media-status');
  if (el) el.textContent = text;
}

function callStatusText(extra = '') {
  const camera = videoStream ? 'camera ready' : 'camera off';
  const mic = audioStream && mediaRecorder?.state === 'recording' ? 'mic recording' : 'mic off';
  const seconds = audioCaptureStartedAt ? Math.max(0, Math.round((Date.now() - audioCaptureStartedAt) / 1000)) : 0;
  const snapshot = latestSnapshotDataUrl ? `snapshot ${new Date().toLocaleTimeString()}` : 'no snapshot';
  return `${camera} / ${mic} / ${seconds}s / mic ${lastMicLevel.toFixed(3)} / ${snapshot}${extra ? ' / ' + extra : ''}`;
}

function updateCallStatus(extra = '') {
  setMediaStatus(videoCallActive ? callStatusText(extra) : (extra || 'Video call idle'));
}

async function startVideoCall() {
  if (!window.isSecureContext) {
    setMediaStatus('Camera requires HTTPS or localhost. Open the forwarded localhost URL or enable HTTPS.');
    return;
  }
  if (!navigator.mediaDevices?.getUserMedia) {
    setMediaStatus('Media capture unavailable in this browser. Check camera permission and secure origin.');
    return;
  }
  videoCallActive = true;
  audioChunks = [];
  audioSampleChunks = [];
  latestSnapshotDataUrl = '';
  lastAudioSendAt = Date.now();
  audioCaptureStartedAt = 0;
  try {
    videoStream = await navigator.mediaDevices.getUserMedia({ video: true });
    const preview = document.getElementById('camera-preview');
    preview.srcObject = videoStream;
    scheduleSnapshots();
  } catch (err) {
    videoStream = null;
    updateCallStatus(`camera unavailable: ${err.message}`);
  }
  try {
    audioStream = await navigator.mediaDevices.getUserMedia({ audio: true });
    startContinuousAudioRecorder();
    startVoiceActivityMonitor();
  } catch (err) {
    audioStream = null;
    updateCallStatus(`mic unavailable: ${err.message}`);
  }
  updateCallStatus('capturing');
}

function endVideoCall() {
  videoCallActive = false;
  if (snapshotTimer) {
    clearInterval(snapshotTimer);
    snapshotTimer = null;
  }
  if (mediaRecorder && mediaRecorder.state !== 'inactive') {
    try { mediaRecorder.stop(); } catch {}
  }
  stopVoiceActivityMonitor();
  for (const track of videoStream?.getTracks?.() || []) track.stop();
  for (const track of audioStream?.getTracks?.() || []) track.stop();
  videoStream = null;
  audioStream = null;
  mediaRecorder = null;
  const preview = document.getElementById('camera-preview');
  if (preview) preview.srcObject = null;
  updateCallStatus('Video call ended');
}

async function initMedia() {
  await startVideoCall();
}

function toggleRecording() {
  if (videoCallActive) {
    endVideoCall();
  } else {
    startVideoCall();
  }
}

function pickAudioMime() {
  const candidates = ['audio/webm;codecs=opus', 'audio/webm'];
  for (const mime of candidates) {
    if (window.MediaRecorder && MediaRecorder.isTypeSupported(mime)) return mime;
  }
  return '';
}

function scheduleSnapshots() {
  captureLatestSnapshot();
  if (snapshotTimer) clearInterval(snapshotTimer);
  snapshotTimer = setInterval(captureLatestSnapshot, 5000);
}

function startContinuousAudioRecorder() {
  if (!audioStream) return;
  audioChunks = [];
  recordedAudioMime = pickAudioMime();
  try {
    mediaRecorder = recordedAudioMime
      ? new MediaRecorder(audioStream, { mimeType: recordedAudioMime })
      : new MediaRecorder(audioStream);
  } catch {
    recordedAudioMime = '';
    mediaRecorder = new MediaRecorder(audioStream);
  }
  mediaRecorder.ondataavailable = (event) => {
    if (event.data && event.data.size > 0) {
      audioChunks.push({ blob: event.data, at: Date.now() });
      const cutoff = Date.now() - 60000;
      audioChunks = audioChunks.filter((item) => item.at >= cutoff);
    }
  };
  mediaRecorder.onerror = (event) => {
    updateCallStatus(`recorder error: ${event.error?.message || 'unknown'}`);
  };
  mediaRecorder.start(1000);
  audioCaptureStartedAt = Date.now();
}

function startVoiceActivityMonitor() {
  stopVoiceActivityMonitor();
  const AudioContextClass = window.AudioContext || window.webkitAudioContext;
  if (!audioStream || !AudioContextClass) {
    updateCallStatus('audio meter unavailable');
    return;
  }
  audioContext = new AudioContextClass();
  audioContext.resume?.().catch(() => {});
  audioCaptureSourceNode = audioContext.createMediaStreamSource(audioStream);
  analyserNode = audioContext.createAnalyser();
  analyserNode.fftSize = 2048;
  audioProcessorNode = audioContext.createScriptProcessor(4096, 1, 1);
  audioMuteGainNode = audioContext.createGain();
  audioMuteGainNode.gain.value = 0;
  audioProcessorNode.onaudioprocess = (event) => {
    if (!videoCallActive || asrPausedForTts) return;
    const input = event.inputBuffer.getChannelData(0);
    audioSampleChunks.push({
      samples: new Float32Array(input),
      sampleRate: event.inputBuffer.sampleRate,
      at: Date.now(),
    });
    const cutoff = Date.now() - 60000;
    audioSampleChunks = audioSampleChunks.filter((item) => item.at >= cutoff);
  };
  audioCaptureSourceNode.connect(analyserNode);
  audioCaptureSourceNode.connect(audioProcessorNode);
  audioProcessorNode.connect(audioMuteGainNode);
  audioMuteGainNode.connect(audioContext.destination);
  const samples = new Uint8Array(analyserNode.fftSize);
  monitorTimer = setInterval(() => {
    if (!videoCallActive || !analyserNode || asrPausedForTts) return;
    analyserNode.getByteTimeDomainData(samples);
    let sum = 0;
    for (const value of samples) {
      const normalized = (value - 128) / 128;
      sum += normalized * normalized;
    }
    const rms = Math.sqrt(sum / samples.length);
    lastMicLevel = rms;
    if (rms > silenceThreshold) {
      speechSeen = true;
      if (silenceTimer) {
        clearTimeout(silenceTimer);
        silenceTimer = null;
      }
      updateCallStatus('speaking');
      return;
    }
    if (speechSeen && !silenceTimer && Date.now() - lastVoiceAutoSentAt > 3000) {
      silenceTimer = setTimeout(() => {
        silenceTimer = null;
        if (speechSeen) {
          speechSeen = false;
          autoSendVoiceTurn();
        }
      }, silenceMsBeforeSend);
      updateCallStatus('silence timer');
    }
  }, 250);
}

function stopVoiceActivityMonitor() {
  if (monitorTimer) {
    clearInterval(monitorTimer);
    monitorTimer = null;
  }
  if (silenceTimer) {
    clearTimeout(silenceTimer);
    silenceTimer = null;
  }
  speechSeen = false;
  if (audioContext) {
    try { audioProcessorNode?.disconnect(); } catch {}
    try { audioCaptureSourceNode?.disconnect(); } catch {}
    try { audioMuteGainNode?.disconnect(); } catch {}
    audioContext.close().catch(() => {});
    audioContext = null;
  }
  audioProcessorNode = null;
  audioCaptureSourceNode = null;
  audioMuteGainNode = null;
  analyserNode = null;
}

function clearCallMedia() {
  selectedAudioDataUrl = '';
  selectedImageDataUrl = '';
  latestSnapshotDataUrl = '';
  lastRoundImageDataUrl = '';
  lastRoundAudioDataUrl = '';
  audioChunks = [];
  audioSampleChunks = [];
  lastAudioSendAt = Date.now();
  updateCallStatus('cleared');
}

function clearRecordedAudio() {
  clearCallMedia();
}

function blobToDataUrl(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ''));
    reader.onerror = reject;
    reader.readAsDataURL(blob);
  });
}

function resampleMono(buffer, targetSampleRate) {
  const sourceRate = buffer.sampleRate;
  const sourceLength = buffer.length;
  const targetLength = Math.max(1, Math.round(sourceLength * targetSampleRate / sourceRate));
  const channels = buffer.numberOfChannels || 1;
  const source = [];
  for (let ch = 0; ch < channels; ch++) source.push(buffer.getChannelData(ch));
  const output = new Float32Array(targetLength);
  for (let i = 0; i < targetLength; i++) {
    const pos = i * (sourceLength - 1) / Math.max(1, targetLength - 1);
    const left = Math.floor(pos);
    const right = Math.min(sourceLength - 1, left + 1);
    const frac = pos - left;
    let sample = 0;
    for (let ch = 0; ch < channels; ch++) {
      sample += (source[ch][left] || 0) * (1 - frac) + (source[ch][right] || 0) * frac;
    }
    output[i] = sample / channels;
  }
  return output;
}

function resampleFloat32(samples, sourceRate, targetSampleRate) {
  if (!samples.length || sourceRate === targetSampleRate) return samples;
  const targetLength = Math.max(1, Math.round(samples.length * targetSampleRate / sourceRate));
  const output = new Float32Array(targetLength);
  for (let i = 0; i < targetLength; i++) {
    const pos = i * (samples.length - 1) / Math.max(1, targetLength - 1);
    const left = Math.floor(pos);
    const right = Math.min(samples.length - 1, left + 1);
    const frac = pos - left;
    output[i] = (samples[left] || 0) * (1 - frac) + (samples[right] || 0) * frac;
  }
  return output;
}

function audioSampleWindowToWavDataUrl(since) {
  const chunks = audioSampleChunks.filter((item) => item.at >= since && item.samples.length > 0);
  if (chunks.length === 0) return null;
  const total = chunks.reduce((sum, item) => sum + item.samples.length, 0);
  const merged = new Float32Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    merged.set(chunk.samples, offset);
    offset += chunk.samples.length;
  }
  const sourceRate = chunks[0].sampleRate || voiceWavSampleRate;
  const mono16k = resampleFloat32(merged, sourceRate, voiceWavSampleRate);
  return blobToDataUrl(pcmToWavBlob(mono16k, voiceWavSampleRate));
}

function pcmToWavBlob(samples, sampleRate) {
  const channels = 1;
  const bytesPerSample = 2;
  const blockAlign = channels * bytesPerSample;
  const dataSize = samples.length * blockAlign;
  const arrayBuffer = new ArrayBuffer(44 + dataSize);
  const view = new DataView(arrayBuffer);
  let offset = 0;

  function writeString(value) {
    for (let i = 0; i < value.length; i++) view.setUint8(offset++, value.charCodeAt(i));
  }

  writeString('RIFF');
  view.setUint32(offset, 36 + dataSize, true); offset += 4;
  writeString('WAVE');
  writeString('fmt ');
  view.setUint32(offset, 16, true); offset += 4;
  view.setUint16(offset, 1, true); offset += 2;
  view.setUint16(offset, channels, true); offset += 2;
  view.setUint32(offset, sampleRate, true); offset += 4;
  view.setUint32(offset, sampleRate * blockAlign, true); offset += 4;
  view.setUint16(offset, blockAlign, true); offset += 2;
  view.setUint16(offset, 16, true); offset += 2;
  writeString('data');
  view.setUint32(offset, dataSize, true); offset += 4;

  for (let i = 0; i < samples.length; i++) {
    const sample = Math.max(-1, Math.min(1, samples[i] || 0));
    view.setInt16(offset, sample < 0 ? sample * 0x8000 : sample * 0x7fff, true);
    offset += 2;
  }
  return new Blob([arrayBuffer], { type: 'audio/wav' });
}

async function blobToWavDataUrl(blob) {
  const AudioContextClass = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextClass) return blobToDataUrl(blob);
  const bytes = await blob.arrayBuffer();
  const ctx = new AudioContextClass();
  try {
    const audioBuffer = await ctx.decodeAudioData(bytes.slice(0));
    const mono16k = resampleMono(audioBuffer, voiceWavSampleRate);
    return await blobToDataUrl(pcmToWavBlob(mono16k, voiceWavSampleRate));
  } finally {
    ctx.close?.().catch(() => {});
  }
}

function splitDataUrl(dataUrl) {
  const match = /^data:([^,]+),(.*)$/s.exec(dataUrl || '');
  if (!match) return null;
  const header = match[1];
  if (!header.includes(';base64')) return null;
  return { mime_type: header.split(';')[0], data_base64: match[2] };
}

async function loadImageFile(event) {
  const file = event.target.files?.[0];
  if (!file) return;
  selectedImageDataUrl = await blobToDataUrl(file);
  setMediaStatus(`Image file ready (${Math.round(file.size / 1024)} KB)`);
}

async function loadAudioFile(event) {
  const file = event.target.files?.[0];
  if (!file) return;
  selectedAudioDataUrl = await blobToDataUrl(file);
  setMediaStatus(`Audio file ready (${Math.round(file.size / 1024)} KB)`);
}

function captureSnapshotPart(options = {}) {
  const preferSelected = options.preferSelected !== false;
  if (preferSelected && selectedImageDataUrl) {
    const selected = splitDataUrl(selectedImageDataUrl);
    if (selected) return { kind: 'image', ...selected };
  }
  const video = document.getElementById('camera-preview');
  if (!video || !video.srcObject || !video.videoWidth || !video.videoHeight) {
    const latest = splitDataUrl(latestSnapshotDataUrl);
    return latest ? { kind: 'image', ...latest } : null;
  }
  const canvas = document.getElementById('snapshot-canvas');
  const maxWidth = 640;
  const scale = Math.min(1, maxWidth / video.videoWidth);
  canvas.width = Math.round(video.videoWidth * scale);
  canvas.height = Math.round(video.videoHeight * scale);
  const ctx = canvas.getContext('2d');
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  const image = splitDataUrl(canvas.toDataURL('image/jpeg', 0.78));
  if (!image) return null;
  return { kind: 'image', ...image };
}

function captureLatestSnapshot() {
  const image = captureSnapshotPart({ preferSelected: false });
  if (!image) return;
  latestSnapshotDataUrl = `data:${image.mime_type};base64,${image.data_base64}`;
  updateCallStatus('capturing');
}

async function currentAudioWindowPart() {
  if (selectedAudioDataUrl) {
    const selected = splitDataUrl(selectedAudioDataUrl);
    return selected ? { kind: 'audio', ...selected } : null;
  }
  const since = Math.max(lastAudioSendAt || 0, Date.now() - voiceSendWindowMs);
  const pcmDataUrl = await audioSampleWindowToWavDataUrl(since);
  if (pcmDataUrl) {
    updateCallStatus('encoding wav');
    lastRoundAudioDataUrl = pcmDataUrl;
    const audio = splitDataUrl(pcmDataUrl);
    return audio ? { kind: 'audio', ...audio } : null;
  }
  const chunks = audioChunks.filter((item) => item.at >= since);
  if (chunks.length === 0) return null;
  const mime = recordedAudioMime || chunks[0].blob.type || 'audio/webm';
  const blob = new Blob(chunks.map((item) => item.blob), { type: mime });
  if (blob.size === 0) return null;
  updateCallStatus('encoding wav');
  const dataUrl = await blobToWavDataUrl(blob);
  lastRoundAudioDataUrl = dataUrl;
  const audio = splitDataUrl(dataUrl);
  return audio ? { kind: 'audio', ...audio } : null;
}

async function recognizeAudioPart(audio) {
  if (!audio?.data_base64) throw new Error('empty audio');
  const r = await fetch(`${GATEWAY}/api/demo/asr`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain;charset=utf-8' },
    mode: 'cors',
    body: JSON.stringify({
      audio: {
        mime_type: audio.mime_type || 'audio/wav',
        data_base64: audio.data_base64,
      },
      format: 'wav',
      sample_rate: voiceWavSampleRate,
    }),
  });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const data = await r.json();
  const payload = data.data || data;
  if (payload.disabled) throw new Error(payload.reason || 'ASR disabled');
  if (!data.ok && (data.error || payload.error)) throw new Error(data.error || payload.error);
  const transcript = String(payload.transcript || '').trim();
  if (!transcript) throw new Error(payload.error || 'no speech recognized');
  return transcript;
}

function hasAudioSinceLastSend() {
  const since = Math.max(lastAudioSendAt || 0, Date.now() - voiceSendWindowMs);
  return audioSampleChunks.some((item) => item.at >= since) ||
    audioChunks.some((item) => item.at >= since) ||
    Boolean(selectedAudioDataUrl);
}

function markAudioWindowSent() {
  lastAudioSendAt = Date.now();
}

async function buildMessageParts(userText, options = {}) {
  const parts = [{ kind: 'text', text: userText }];
  const image = captureSnapshotPart();
  if (image) {
    parts.push(image);
    lastRoundImageDataUrl = `data:${image.mime_type};base64,${image.data_base64}`;
    await uploadGeneratedStorageAsset(image, 'snapshot');
  }
  if (options.includeAudio) {
    const audio = await currentAudioWindowPart();
    if (audio) {
      parts.push(audio);
      await uploadGeneratedStorageAsset(audio, 'audio');
    }
  }
  return parts;
}

async function speakAgentText(text) {
  if (!videoCallActive || !text) return;
  try {
    updateCallStatus('tts requested');
    const r = await fetch(`${GATEWAY}/api/demo/tts`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body: JSON.stringify({ text }),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const data = await r.json();
    const payload = data.data || data;
    if (payload.disabled) {
      updateCallStatus('tts disabled');
      return;
    }
    if (!payload.data_base64 || !payload.mime_type) {
      throw new Error(payload.error || data.error || 'empty tts audio');
    }
    if (ttsAudio) ttsAudio.pause();
    ttsAudio = new Audio(`data:${payload.mime_type};base64,${payload.data_base64}`);
    ttsAudio.onended = () => updateCallStatus('tts finished');
    updateCallStatus('tts playing');
    await ttsAudio.play();
  } catch (err) {
    updateCallStatus(`tts failed: ${err.message}`);
  }
}

async function autoSendVoiceTurn() {
  if (isSending || !videoCallActive || !hasAudioSinceLastSend()) return;
  lastVoiceAutoSentAt = Date.now();
  updateCallStatus('auto sending');
  await sendRecognizedVoiceTurn('auto');
}

async function sendVoiceNow() {
  if (isSending) return;
  if (!hasAudioSinceLastSend()) {
    updateCallStatus('no voice audio yet');
    appendMsg('No voice audio captured yet.', 'system');
    return;
  }
  lastVoiceAutoSentAt = Date.now();
  updateCallStatus('manual voice send');
  await sendRecognizedVoiceTurn('manual');
}

async function sendRecognizedVoiceTurn(mode) {
  if (isSending) return;
  isSending = true;
  document.getElementById('send-btn').disabled = true;
  const userDiv = appendMsg('Recognizing voice...', 'user');
  const loadingDiv = appendMsg(mode === 'auto' ? 'Voice turn detected, recognizing...' : 'Recognizing voice...', 'system');
  try {
    updateCallStatus('asr requested');
    const audio = await currentAudioWindowPart();
    if (!audio) throw new Error('no voice audio captured');
    await uploadGeneratedStorageAsset(audio, 'voice');
    const transcript = await recognizeAudioPart(audio);
    userDiv.textContent = transcript;
    updateCallStatus('asr finished');
    loadingDiv.remove();
    isSending = false;
    document.getElementById('send-btn').disabled = false;
    await doRequest(transcript, 'user', { userDiv, includeAudio: false });
  } catch (err) {
    loadingDiv.remove();
    appendMsg(`ASR failed: ${err.message}`, 'error');
    updateCallStatus(`asr failed: ${err.message}`);
    isSending = false;
    document.getElementById('send-btn').disabled = false;
  }
}

// ── Send Message ──

async function sendUserMessage() {
  const input = document.getElementById('chat-input');
  const text = input.value.trim();
  if (!text || isSending) return;
  input.value = '';
  await doRequest(text, 'user');
}

function onInputKey(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendUserMessage();
  }
}

async function sendDemo(type) {
  if (isSending) return;
  if (type === 'arch') {
    showArchDemo();
    return;
  }
  const texts = {
    normal: '你好，我最近压力有点大，想聊聊。',
    risk: '我不想活了，感觉活着没有意义。',
  };
  await doRequest(texts[type], type === 'risk' ? 'user risk' : 'user');
}

// ── Core Request ──

async function doRequest(userText, userMsgClass, options = {}) {
  isSending = true;
  document.getElementById('send-btn').disabled = true;
  const userDiv = options.userDiv || appendMsg(options.displayText || userText || 'Voice input', userMsgClass);

  const loadingDiv = appendMsg('Processing...', 'system');
  resetTimeline();
  resetDetails();

  requestId++;
  const parts = await buildMessageParts(userText, { includeAudio: Boolean(options.includeAudio) });
  const makeBody = (method) => JSON.stringify({
    jsonrpc: '2.0',
    id: String(requestId),
    method,
    params: {
      message: {
        role: 'user',
        contextId: platformConversationId(),
        parts,
      },
      historyLength: 5,
    },
    metadata: {
      auth_token: authToken,
    },
  });

  try {
    if (window.ReadableStream) {
      const streamed = await doStreamingRequest(makeBody('message/stream'), {
        loadingDiv,
        userDiv,
        userText,
      });
      if (streamed) return;
    }
    await doSyncRequest(makeBody('message/send'), { loadingDiv, userDiv, userText });
  } catch (err) {
    loadingDiv.remove();
    appendMsg(`Request failed: ${err.message}`, 'error');
  } finally {
    isSending = false;
    document.getElementById('send-btn').disabled = false;
  }
}

async function doSyncRequest(body, { loadingDiv, userDiv, userText }) {
    const r = await fetch(`${GATEWAY}/`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body,
    });

    loadingDiv.remove();

    if (!r.ok) {
      appendMsg(`HTTP ${r.status}: ${r.statusText}`, 'error');
      return;
    }

    const data = await r.json();

    if (data.error) {
      appendMsg(formatAgentError(data.error), 'error');
      return;
    }

    const result = data.result || {};
    const orchestration = result.orchestration || {};
    const multimodal = result.multimodal_emotion || orchestration.multimodal_emotion || {};
    const effectiveUserText = result.effective_user_text || orchestration.effective_user_text || multimodal.transcript || '';
    if (!userText && effectiveUserText && effectiveUserText !== '（语音输入未识别出文字）') {
      userDiv.textContent = effectiveUserText;
    }
    const agentText = result.message?.parts?.[0]?.text || '(no text)';
    appendMsg(agentText, 'agent');
    markAudioWindowSent();

    // Build timeline from response
    buildTimeline(result, userText);

    // Build details
    buildDetails(result);
    speakAgentText(agentText);

    // Fetch run inspector data
    const runId = result.run_id;
    if (runId) {
      attachPlatformRun(runId);
      fetchRunDetails(runId);
    } else {
      fetchLatestRun();
    }
    loadConversationHistory();
}

async function doStreamingRequest(body, { loadingDiv, userDiv, userText }) {
  const r = await fetch(`${GATEWAY}/`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain;charset=utf-8' },
    mode: 'cors',
    body,
  });
  if (!r.ok || !r.body) return false;

  let agentDiv = null;
  let finalResult = null;
  let runId = '';
  let ttsBuffer = '';
  let sawToken = false;
  let streamError = '';
  const flushTts = (force = false) => {
    let chunk;
    while ((chunk = takeSpeakablePrefix(ttsBuffer, force))) {
      ttsBuffer = ttsBuffer.slice(chunk.length);
      enqueueStreamingTts(chunk);
      if (!force) continue;
      break;
    }
  };

  await readSse(r, async (event) => {
    if (event.event === 'orchestration') {
      return;
    }
    if (event.event === 'run_started') {
      runId = event.run_id || runId;
      return;
    }
    if (event.event === 'token') {
      if (!sawToken) {
        sawToken = true;
        loadingDiv.remove();
        agentDiv = appendMsg('', 'agent');
      }
      const text = event.text || '';
      agentDiv.textContent += text;
      ttsBuffer += text;
      flushTts(false);
      return;
    }
    if (event.event === 'message_done') {
      runId = event.run_id || runId;
      if (!agentDiv) {
        loadingDiv.remove();
        agentDiv = appendMsg(event.text || '', 'agent');
      } else if (event.text && !agentDiv.textContent) {
        agentDiv.textContent = event.text;
      }
      flushTts(true);
      return;
    }
    if (event.event === 'final_result') {
      finalResult = event.result || null;
      return;
    }
    if (event.event === 'error') {
      streamError = event.message || 'stream failed';
    }
  });

  if (streamError) {
    loadingDiv.remove();
    appendMsg(formatAgentError(streamError), 'error');
    return true;
  }
  if (!sawToken && !finalResult) return false;

  loadingDiv.remove();
  markAudioWindowSent();
  if (finalResult) {
    buildTimeline(finalResult, userText);
    buildDetails(finalResult);
    runId = finalResult.run_id || runId;
  }
  if (runId) {
    attachPlatformRun(runId);
    fetchRunDetails(runId);
  } else {
    fetchLatestRun();
  }
  loadConversationHistory();
  return true;
}

async function endSession() {
  if (isSending) return;
  isSending = true;
  document.getElementById('send-btn').disabled = true;
  const loadingDiv = appendMsg('Ending session and preparing assessment...', 'system');
  resetTimeline();
  resetDetails();

  requestId++;
  const body = JSON.stringify({
    jsonrpc: '2.0',
    id: String(requestId),
    method: 'session/end',
    params: {
      contextId: platformConversationId(),
    },
    metadata: {
      auth_token: authToken,
    },
  });

  try {
    const r = await fetch(`${GATEWAY}/`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      mode: 'cors',
      body,
    });

    loadingDiv.remove();
    if (!r.ok) {
      appendMsg(`HTTP ${r.status}: ${r.statusText}`, 'error');
      return;
    }

    const data = await r.json();
    if (data.error) {
      appendMsg(formatAgentError(data.error), 'error');
      return;
    }

    const result = data.result || {};
    const assessment = result.evaluator?.response?.result || {};
    const risk = assessment.riskLevel || 'unknown';
    const email = result.email || assessment.email || {};
    const emailText = email.dispatched ? 'email dispatched' : `email not dispatched: ${email.reason || email.error || 'not configured'}`;
    appendMsg(`Session assessment completed. Risk: ${risk}; ${emailText}.`, 'system');
    buildSessionTimeline(result);
    buildSessionDetails(result);

    const runId = result.evaluator?.response?.run_id;
    if (runId) {
      attachPlatformRun(runId);
      fetchRunDetails(runId);
    }
  } catch (err) {
    loadingDiv.remove();
    appendMsg(`Request failed: ${err.message}`, 'error');
  } finally {
    isSending = false;
    document.getElementById('send-btn').disabled = false;
  }
}

// ── Architecture Demo (no backend call) ──

function showArchDemo() {
  appendMsg('Architecture & Harness Constraints', 'user');
  resetTimeline();

  const steps = [
    { name: 'Gateway (:8090)', tag: 'entry', body: 'Accepts JSON-RPC, injects request_id & gateway metadata, enforces gateway_token auth.' },
    { name: 'Orchestrator (:5009)', tag: 'routing', body: 'Heuristic intent + risk classification. <code>is_high_risk()</code> triggers Evaluator dispatch.' },
    { name: 'Evaluator (:5011)', tag: 'guard', body: 'Standalone risk/emotion assessment. Called only when risk_level is high (or medium with flag).' },
    { name: 'Counselor (:5010)', tag: 'agent', body: 'Full agent loop: risk assessment, RAG retrieval, conversation memory, prompt prefix, model generation.' },
    { name: 'ToolRegistry', tag: 'tools', body: 'Controls callable MCP tools, validates schemas, enforces permission checks before execution.' },
    { name: 'MCP Tools', tag: 'mcp', body: '<code>mcp_excel_write</code> / <code>mcp_email_alert</code> — triggered by high-risk or data scenarios.' },
    { name: 'RunStore', tag: 'persist', body: 'Persists task_state.json, report.json, trace.jsonl per run under <code>.mindbridge/runs/</code>.' },
  ];

  const timeline = document.getElementById('timeline');
  document.getElementById('timeline-empty').classList.add('hidden');

  for (const s of steps) {
    const node = document.createElement('div');
    node.className = 'timeline-node done';
    node.innerHTML = `
      <div class="timeline-node-header">
        <span class="timeline-node-name">${s.name}</span>
        <span class="timeline-node-tag">${s.tag}</span>
      </div>
      <div class="timeline-node-body">${s.body}</div>
    `;
    timeline.appendChild(node);
  }

  // Show feature status
  loadFeatureStatus();
}

// ── Timeline ──

function resetTimeline() {
  const timeline = document.getElementById('timeline');
  timeline.innerHTML = '';
  const empty = document.createElement('div');
  empty.className = 'empty-state';
  empty.id = 'timeline-empty';
  empty.innerHTML = '<div class="empty-state-icon">&#9671;</div><div class="empty-state-text">Processing...</div>';
  timeline.appendChild(empty);
}

function buildTimeline(result, userText) {
  const timeline = document.getElementById('timeline');
  timeline.innerHTML = '';
  document.getElementById('timeline-empty')?.remove();

  const runtime = result.runtime || {};
  const orchestration = result.orchestration || {};
  const riskLevel = runtime.risk_level || orchestration.preliminary_risk_level || orchestration.risk_level || 'unknown';
  const intent = runtime.intent || orchestration.preliminary_intent || orchestration.intent || 'unknown';
  const evaluatorCalled = Boolean(orchestration.evaluator?.called || orchestration.evaluator_called);
  const runId = result.run_id || '-';
  const promptMeta = result.prompt_metadata || {};
  const multimodal = result.multimodal_emotion || orchestration.multimodal_emotion || {};

  // Gateway
  addTimelineNode(timeline, 'Gateway (:8090)', 'entry', 'done',
    `Accepted request, injected <code>request_id</code>. Forwarded to Orchestrator.`);

  // Orchestrator
  addTimelineNode(timeline, 'Orchestrator (:5009)', 'routing', 'done',
    `Intent: <code>${esc(intent)}</code> &middot; Risk: <code>${esc(riskLevel)}</code>. ${evaluatorCalled ? 'Dispatched to Evaluator.' : 'Skipped Evaluator.'}`);

  if (multimodal && !multimodal.disabled && multimodal.final_emotion) {
    addTimelineNode(timeline, 'MiMo Multimodal Tool', 'vision/audio', 'done',
      `Emotion: <code>${esc(multimodal.final_emotion)}</code> &middot; Score: <code>${Number(multimodal.weighted_score || 0).toFixed(2)}</code> &middot; Risk: <code>${esc(multimodal.risk_level || 'low')}</code>.`);
  } else if (multimodal && multimodal.disabled === true) {
    addTimelineNode(timeline, 'MiMo Multimodal Tool', 'vision/audio', 'skipped',
      `Disabled or no non-text media was provided.`);
  }

  // Evaluator
  if (evaluatorCalled) {
    const evaluatorResult = orchestration.evaluator?.response?.result || orchestration.evaluator?.response || {};
    const evaluatorRisk = evaluatorResult.riskLevel ? ` Risk: <code>${esc(evaluatorResult.riskLevel)}</code>.` : '';
    addTimelineNode(timeline, 'Evaluator (:5011)', 'guard', 'active',
      `Risk assessment completed.${evaluatorRisk} Result merged into orchestration.`);
  } else {
    addTimelineNode(timeline, 'Evaluator (:5011)', 'guard', 'skipped',
      `Not called — risk level did not require evaluation.`);
  }

  // Counselor
  const tokInfo = promptMeta.model_metadata
    ? `Tokens: ${promptMeta.model_metadata.input_tokens || '?'} in / ${promptMeta.model_metadata.output_tokens || '?'} out`
    : '';
  addTimelineNode(timeline, 'Counselor (:5010)', 'agent', 'done',
    `Agent loop completed. ${tokInfo}`);

  // ToolRegistry
  const toolSteps = runtime.tool_steps || result.tool_steps || 0;
  if (toolSteps > 0) {
    addTimelineNode(timeline, 'ToolRegistry', 'tools', 'active',
      `${toolSteps} tool step(s) executed.`);
  } else {
    addTimelineNode(timeline, 'ToolRegistry', 'tools', 'skipped',
      `No tools invoked.`);
  }

  // MCP
  const mcpTriggered = checkMcpTriggered(result);
  if (mcpTriggered) {
    addTimelineNode(timeline, 'MCP Tools', 'mcp', 'active',
      `MCP tool triggered: <code>${esc(mcpTriggered)}</code>`);
  } else {
    addTimelineNode(timeline, 'MCP Tools', 'mcp', 'skipped',
      `No MCP tools called.`);
  }

  // RunStore
  addTimelineNode(timeline, 'RunStore', 'persist', 'done',
    `Run <code>${esc(runId)}</code> saved. Dir: <code>${esc(result.run_dir || '.mindbridge/runs/')}</code>`);
}

function buildSessionTimeline(result) {
  const timeline = document.getElementById('timeline');
  timeline.innerHTML = '';
  document.getElementById('timeline-empty')?.remove();

  const evaluatorCalled = Boolean(result.evaluator?.called);
  const assessment = result.evaluator?.response?.result || {};
  const risk = assessment.riskLevel || 'unknown';
  const email = result.email || assessment.email || {};
  const emailState = email.dispatched ? 'dispatched' : (email.reason || email.error || 'not configured');

  addTimelineNode(timeline, 'Gateway (:8090)', 'entry', 'done',
    `Accepted <code>session/end</code> request and forwarded it to Orchestrator.`);
  addTimelineNode(timeline, 'Orchestrator (:5009)', 'routing', 'done',
    `Loaded recent transcript for <code>${esc(result.conversation_id || 'demo-session')}</code>.`);
  addTimelineNode(timeline, 'Evaluator (:5011)', 'summary', evaluatorCalled ? 'active' : 'skipped',
    evaluatorCalled ? `Session assessment completed. Risk: <code>${esc(risk)}</code>.` : `No conversation history was available.`);
  addTimelineNode(timeline, 'MCP Tools', 'mcp', email.dispatched ? 'active' : 'skipped',
    `Session assessment email: <code>${esc(emailState)}</code>.`);
}

function addTimelineNode(container, name, tag, status, bodyHtml) {
  const node = document.createElement('div');
  node.className = `timeline-node ${status}`;
  node.innerHTML = `
    <div class="timeline-node-header">
      <span class="timeline-node-name">${name}</span>
      <span class="timeline-node-tag">${tag}</span>
    </div>
    <div class="timeline-node-body">${bodyHtml}</div>
  `;
  container.appendChild(node);
}

function checkMcpTriggered(result) {
  // Check if any MCP tool was called — look in orchestration or runtime
  const orch = result.orchestration || {};
  if (orch.mcp_tool) return orch.mcp_tool;
  if (orch.evaluator?.response?.result?.riskLevel === 'high') return 'mcp_email_alert';
  if (result.runtime?.risk_level === 'high') return 'mcp_email_alert';
  if (orch.tool_calls) {
    for (const tc of orch.tool_calls) {
      if (tc.name && tc.name.startsWith('mcp_')) return tc.name;
    }
  }
  return null;
}

// ── Run Details Panel ──

function resetDetails() {
  document.getElementById('details-empty').classList.remove('hidden');
  document.getElementById('details-content').classList.add('hidden');
  renderMultimodal({});
}

function buildDetails(result) {
  document.getElementById('details-empty').classList.add('hidden');
  document.getElementById('details-content').classList.remove('hidden');

  const runtime = result.runtime || {};
  const orchestration = result.orchestration || {};
  const intent = runtime.intent || orchestration.preliminary_intent || orchestration.intent || '-';
  const riskLevel = runtime.risk_level || orchestration.preliminary_risk_level || orchestration.risk_level || '-';
  const evaluatorCalled = Boolean(orchestration.evaluator?.called || orchestration.evaluator_called);
  const multimodal = result.multimodal_emotion || orchestration.multimodal_emotion || {};

  setDetailVal('d-intent', intent, intentColor(intent));
  setDetailVal('d-risk', riskLevel, riskColor(riskLevel));
  setDetailVal('d-evaluator', evaluatorCalled ? 'CALLED' : 'not called', evaluatorCalled ? 'highlight-yellow' : 'highlight-green');

  const runId = result.run_id || '-';
  setDetailVal('d-runid', runId, '');
  setDetailVal('d-status', runtime.status || '-', 'highlight-green');
  setDetailVal('d-stopreason', runtime.stop_reason || '-', '');
  setDetailVal('d-attempts', String(runtime.attempts || result.attempts || '-'), '');
  setDetailVal('d-toolsteps', String(runtime.tool_steps || result.tool_steps || 0), '');

  // Prompt metadata
  const pm = result.prompt_metadata || {};
  setDetailVal('d-promptchars', String(pm.prompt_chars || '-'), '');
  const mm = pm.model_metadata || {};
  setDetailVal('d-inputtok', String(mm.input_tokens || '-'), '');
  setDetailVal('d-outputtok', String(mm.output_tokens || '-'), '');
  setDetailVal('d-cachehit', mm.cache_hit ? 'YES' : 'no', mm.cache_hit ? 'highlight-green' : '');
  setDetailVal('d-prefixhash', pm.prefix_hash || '-', '');

  // MCP triggers
  const mcpDiv = document.getElementById('d-mcp-triggers');
  const mcpTool = checkMcpTriggered(result);
  if (mcpTool) {
    mcpDiv.innerHTML = `<span style="color:var(--accent-orange);font-family:var(--font-mono);font-size:0.8rem">${esc(mcpTool)}</span>`;
  } else {
    mcpDiv.textContent = 'None';
  }

  renderMultimodal(multimodal);
}

function buildSessionDetails(result) {
  document.getElementById('details-empty').classList.add('hidden');
  document.getElementById('details-content').classList.remove('hidden');

  const assessment = result.evaluator?.response?.result || {};
  const risk = assessment.riskLevel || '-';
  const email = result.email || assessment.email || {};

  setDetailVal('d-intent', 'SESSION_SUMMARY', 'highlight-blue');
  setDetailVal('d-risk', risk, riskColor(risk));
  setDetailVal('d-evaluator', result.evaluator?.called ? 'SESSION SUMMARY' : 'not called', result.evaluator?.called ? 'highlight-yellow' : '');
  setDetailVal('d-runid', result.evaluator?.response?.run_id || '-', '');
  setDetailVal('d-status', result.ended ? 'completed' : 'stopped', result.ended ? 'highlight-green' : 'highlight-red');
  setDetailVal('d-stopreason', result.ended ? 'session_end' : (result.message || '-'), '');
  setDetailVal('d-attempts', '-', '');
  setDetailVal('d-toolsteps', '-', '');

  const mcpDiv = document.getElementById('d-mcp-triggers');
  if (email.dispatched) {
    mcpDiv.innerHTML = '<span style="color:var(--accent-orange);font-family:var(--font-mono);font-size:0.8rem">mcp_email_alert</span>';
  } else {
    mcpDiv.textContent = email.reason || email.error || 'Email endpoint not configured';
  }

  const traceDiv = document.getElementById('d-trace');
  traceDiv.textContent = JSON.stringify(assessment, null, 2);
  renderMultimodal(assessment.multimodal_emotion || {});
}

function renderMultimodal(multimodal) {
  const div = document.getElementById('d-multimodal');
  if (!div) return;
  if (!multimodal || Object.keys(multimodal).length === 0) {
    div.textContent = 'None';
    return;
  }
  if (multimodal.disabled) {
    div.textContent = 'Disabled or no media submitted';
    return;
  }
  const score = Number(multimodal.weighted_score || 0).toFixed(2);
  const textLabel = multimodal.text_emotion?.label || '-';
  const visualLabel = multimodal.visual_emotion?.label || '-';
  const audioLabel = multimodal.audio_emotion?.label || '-';
  const transcript = multimodal.transcript || '';
  div.innerHTML = `
    <div><span style="color:var(--accent-blue);font-family:var(--font-mono)">${esc(multimodal.final_emotion || '-')}</span>
    &middot; risk <span style="font-family:var(--font-mono)">${esc(multimodal.risk_level || '-')}</span>
    &middot; score <span style="font-family:var(--font-mono)">${esc(score)}</span></div>
    <div style="margin-top:6px">text ${esc(textLabel)} / visual ${esc(visualLabel)} / audio ${esc(audioLabel)}</div>
    ${transcript ? `<div style="margin-top:6px;color:var(--accent-cyan)">ASR: ${esc(transcript)}</div>` : ''}
    <div style="margin-top:6px;color:var(--text-muted)">${esc(multimodal.evidence_summary || '')}</div>
  `;
}

function setDetailVal(id, val, colorClass) {
  const el = document.getElementById(id);
  el.textContent = val;
  el.className = 'detail-val';
  if (colorClass) el.classList.add(colorClass);
}

function intentColor(intent) {
  if (intent === 'CRISIS' || intent === 'HIGH_RISK' || intent === 'RISK') return 'highlight-red';
  if (intent === 'CONSULT') return 'highlight-blue';
  return '';
}

function riskColor(risk) {
  if (risk === 'high' || risk === 'critical') return 'highlight-red';
  if (risk === 'medium') return 'highlight-yellow';
  if (risk === 'low') return 'highlight-green';
  return '';
}

// ── Boundary Observability ──

async function loadObservability(runId = '') {
  const endpoint = runId
    ? `${GATEWAY}/api/demo/runs/${encodeURIComponent(runId)}/observability${authQuery()}`
    : `${GATEWAY}/api/demo/runs/latest/observability${authQuery()}`;
  try {
    const r = await fetch(endpoint, { mode: 'cors' });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    observabilityData = data;
  } catch (err) {
    observabilityData = {
      error: err.message,
      boundary_trace: [],
      ebpf_events_tail: [],
      observability_report: { summary: {} },
    };
  }
  renderObservabilityDashboard();
}

function filteredObservabilityEvents() {
  const term = (document.getElementById('observability-filter-input')?.value || '').toLowerCase();
  const source = document.getElementById('obs-source-filter')?.value || '';
  const pid = document.getElementById('obs-pid-filter')?.value || '';
  const events = observabilityData?.boundary_trace || [];
  return events.filter((event) => {
    if (source && String(event.source || '') !== source) return false;
    if (pid && String(event.pid ?? event.action?.pid ?? '') !== pid) return false;
    if (!term) return true;
    return JSON.stringify(event).toLowerCase().includes(term);
  });
}

function renderObservabilityDashboard() {
  const data = observabilityData || {};
  const report = data.observability_report || {};
  const summary = report.summary || {};
  const ebpfEvents = data.ebpf_events_tail || [];
  const tlsEvents = observabilityTlsEvents([...(data.boundary_trace || []), ...ebpfEvents]);
  document.getElementById('obs-intent-events').textContent = String(summary.intent_events || 0);
  document.getElementById('obs-action-events').textContent = String(summary.action_events || 0);
  document.getElementById('obs-resource-events').textContent = String(summary.resource_events || 0);
  document.getElementById('obs-warnings').textContent = String(summary.warnings || 0);
  document.getElementById('obs-tls-events').textContent = String(tlsEvents.length);
  renderBoundaryCards(report.capture_boundaries || data.capture_boundaries || []);
  renderObservabilityFilterOptions([...(data.boundary_trace || []), ...ebpfEvents]);
  renderObservabilityPerformance(data);
  const events = filteredObservabilityEvents();
  renderObservabilityTimeline(events);
  renderObservabilityProcessTree(ebpfEvents);
  renderObservabilityResources(events, ebpfEvents);
  renderObservabilityTls(tlsEvents);
  const log = document.getElementById('obs-event-log');
  if (log) {
    log.textContent = data.error
      ? JSON.stringify({ error: data.error }, null, 2)
      : JSON.stringify(events.slice(-120), null, 2);
  }
}

function setSelectOptions(selectId, values, allLabel) {
  const select = document.getElementById(selectId);
  if (!select) return;
  const selected = select.value;
  select.innerHTML = `<option value="">${esc(allLabel)}</option>`;
  for (const value of values) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = value;
    select.appendChild(option);
  }
  select.value = values.includes(selected) ? selected : '';
}

function renderObservabilityFilterOptions(events) {
  const sources = Array.from(new Set((events || []).map((event) => String(event.source || '')).filter(Boolean))).sort();
  const pids = Array.from(new Set((events || [])
    .map((event) => event.pid ?? event.action?.pid)
    .filter((pid) => pid !== undefined && pid !== null && pid !== '')
    .map((pid) => String(pid)))).sort((a, b) => Number(a) - Number(b));
  setSelectOptions('obs-source-filter', sources, 'All Sources');
  setSelectOptions('obs-pid-filter', pids, 'All PIDs');
}

function fmtPercent(value) {
  const n = Number(value || 0);
  return `${n.toFixed(n >= 10 ? 1 : 2)}%`;
}

function fmtMemoryMb(value) {
  const n = Number(value || 0);
  if (n >= 1024) return `${(n / 1024).toFixed(2)} GB`;
  return `${Math.round(n)} MB`;
}

function resourceSamplesFromEvents(events) {
  const samples = [];
  let prevTime = 0;
  let prevCpuMs = 0;
  for (const event of (events || []).filter((item) => String(item.event || '').includes('RESOURCE_SAMPLE'))) {
    const memoryMb = Number(event.total_rss_kb || 0) > 0
      ? Number(event.total_rss_kb || 0) / 1024
      : Number(event.cgroup_memory_bytes || 0) / (1024 * 1024);
    const cumulativeCpuMs = Number(event.total_cpu_user_ms || 0) + Number(event.total_cpu_sys_ms || 0);
    let cpu = Number(event.total_cpu_percent);
    if (!Number.isFinite(cpu)) {
      cpu = 0;
      const time = Number(event.wall_time_ns || event.timestamp || 0);
      if (prevTime > 0 && time > prevTime && cumulativeCpuMs >= prevCpuMs) {
        cpu = ((cumulativeCpuMs - prevCpuMs) / ((time - prevTime) / 1000000)) * 100;
      }
      prevTime = time;
      prevCpuMs = cumulativeCpuMs;
    }
    samples.push({ memory_mb: memoryMb, cpu_percent: cpu, wall_time_ns: Number(event.wall_time_ns || 0) });
  }
  return samples;
}

function renderObservabilityPerformance(data) {
  const reportMetrics = data?.observability_report?.performance_metrics || {};
  const samples = reportMetrics.samples?.length
    ? reportMetrics.samples
    : resourceSamplesFromEvents([...(data?.boundary_trace || []), ...(data?.ebpf_events_tail || [])]);
  const avg = (values) => values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : 0;
  const peak = (values) => values.length ? Math.max(...values) : 0;
  const cpus = samples.map((sample) => Number(sample.cpu_percent || 0));
  const memories = samples.map((sample) => Number(sample.memory_mb || 0));
  const avgCpu = Number(reportMetrics.avg_cpu_percent ?? avg(cpus));
  const peakCpu = Number(reportMetrics.peak_cpu_percent ?? peak(cpus));
  const avgMemory = Number(reportMetrics.avg_memory_mb ?? avg(memories));
  const peakMemory = Number(reportMetrics.peak_memory_mb ?? peak(memories));
  document.getElementById('obs-avg-cpu').textContent = fmtPercent(avgCpu);
  document.getElementById('obs-peak-cpu').textContent = fmtPercent(peakCpu);
  document.getElementById('obs-avg-memory').textContent = fmtMemoryMb(avgMemory);
  document.getElementById('obs-peak-memory').textContent = fmtMemoryMb(peakMemory);
  document.getElementById('obs-resource-sample-count').textContent = `${samples.length} samples`;

  const chart = document.getElementById('obs-memory-chart');
  if (!chart) return;
  if (!samples.length) {
    chart.innerHTML = '<div class="storage-empty">Enable MINDBRIDGE_EBPF_TRACE_RESOURCES=1 to collect CPU and memory samples.</div>';
    return;
  }
  const maxMemory = Math.max(...memories, 1);
  chart.innerHTML = '';
  for (const sample of samples.slice(-80)) {
    const bar = document.createElement('div');
    bar.className = 'obs-memory-bar';
    const height = Math.max(4, Math.round((Number(sample.memory_mb || 0) / maxMemory) * 100));
    bar.style.height = `${height}%`;
    bar.title = `${fmtMemoryMb(sample.memory_mb)} · ${fmtPercent(sample.cpu_percent)}`;
    chart.appendChild(bar);
  }
}

function renderBoundaryCards(boundaries) {
  const active = new Set(boundaries || []);
  document.querySelectorAll('.boundary-card').forEach((card) => {
    const boundary = card.getAttribute('data-boundary');
    card.classList.toggle('active', active.has(boundary));
  });
}

function isTlsObservabilityEvent(event) {
  const name = String(event.event || event.action_event || event.function || '');
  return name.startsWith('TLS_') || Boolean(event.function);
}

function observabilityTlsEvents(events) {
  return (events || []).filter(isTlsObservabilityEvent);
}

function renderObservabilityTimeline(events) {
  const box = document.getElementById('obs-timeline');
  if (!box) return;
  const timelineEvents = events.filter((event) => {
    if (event.observer_noise || event.action?.observer_noise) return false;
    if (event.kind === 'resource_metric') return false;
    if (event.kind === 'action' && String(event.event || '').includes('FILE_OPEN')) return false;
    if (event.kind === 'correlated_span' && String(event.action_event || '').includes('FILE_OPEN')) {
      const action = event.action || {};
      const path = String(action.filepath || action.filename || '');
      return path && !path.startsWith('/sys/kernel') && !path.startsWith('/proc/');
    }
    return true;
  });
  if (!timelineEvents.length) {
    box.innerHTML = '<div class="storage-empty">No boundary trace loaded</div>';
    return;
  }
  box.innerHTML = '';
  for (const event of timelineEvents.slice(-80)) {
    const row = document.createElement('div');
    const kind = event.kind || 'event';
    row.className = `obs-timeline-row ${kind}`;
    row.style.minHeight = `${Math.round(42 * observabilityTimelineZoom)}px`;
    row.style.fontSize = `${Math.max(0.72, 0.82 * observabilityTimelineZoom)}rem`;
    const action = event.action || {};
    const whenNs = event.wall_time_ns || action.wall_time_ns || event.intent?.wall_time_ns;
    const when = whenNs ? new Date(Number(whenNs) / 1000000).toLocaleTimeString() : '';
    const score = event.correlation_score !== undefined ? ` score=${Number(event.correlation_score).toFixed(2)}` : '';
    const excerpt = event.response_excerpt || event.user_message_excerpt || event.prompt_excerpt || '';
    const summary = observabilityTimelineSummary(event);
    row.innerHTML = `
      <div class="obs-timeline-kind">${esc(kind)}</div>
      <div class="obs-timeline-main">
        <div class="obs-timeline-title">${esc(summary.title)}</div>
        <div class="obs-timeline-meta">${esc(summary.meta)}
          ${summary.pid ? ` pid=${esc(String(summary.pid))}` : ''}
          ${when ? ` · ${esc(when)}` : ''}${score}</div>
        ${excerpt ? `<div class="obs-timeline-excerpt">${esc(String(excerpt).slice(0, 240))}</div>` : ''}
      </div>
    `;
    box.appendChild(row);
  }
  box.onwheel = (event) => {
    if (!event.ctrlKey && !event.metaKey) return;
    event.preventDefault();
    adjustObservabilityZoom(event.deltaY < 0 ? 0.1 : -0.1);
  };
}

function observabilityTimelineSummary(event) {
  const name = String(event.event || event.action_event || event.intent_event || 'event');
  const action = event.action || event;
  if (event.kind === 'intent') {
    if (name === 'run_started') return { title: 'Run started', meta: event.task_id || event.conversation_id || 'MindBridge runtime' };
    if (name === 'model_requested') return { title: 'Model request sent', meta: `attempt ${event.attempt || 1}${event.stream ? ' · streaming' : ''}` };
    if (name === 'model_responded') return { title: 'Model response received', meta: `${event.response_chars || 0} chars` };
    if (name === 'run_finished') return { title: 'Run finished', meta: `${event.status || ''} ${event.stop_reason || ''}`.trim() };
    return { title: name.replaceAll('_', ' '), meta: event.source || 'runtime' };
  }
  if (event.kind === 'correlated_span') {
    const actionName = String(event.action_event || '');
    if (actionName.startsWith('TLS_WRITE')) return { title: 'HTTPS request bytes sent', meta: action.comm || 'TLS plaintext', pid: action.pid };
    if (actionName.startsWith('TLS_READ')) return { title: 'HTTPS response bytes received', meta: action.comm || 'TLS plaintext', pid: action.pid };
    if (actionName === 'EXEC') return { title: `Command executed: ${action.full_command || action.filename || action.comm || 'process'}`, meta: event.rule || '', pid: action.pid };
    if (actionName.includes('NET_CONNECT')) return { title: `Network connection: ${action.remote_addr || 'remote'}:${action.remote_port || ''}`, meta: action.comm || '', pid: action.pid };
    if (actionName.includes('FILE') || actionName === 'SUMMARY') {
      return { title: `File activity: ${action.filepath || action.filename || action.detail || actionName}`, meta: action.comm || event.rule || '', pid: action.pid };
    }
    return { title: `System activity: ${actionName}`, meta: event.rule || '', pid: action.pid };
  }
  if (name.startsWith('TLS_WRITE')) return { title: 'HTTPS request bytes sent', meta: action.comm || 'TLS plaintext', pid: action.pid };
  if (name.startsWith('TLS_READ')) return { title: 'HTTPS response bytes received', meta: action.comm || 'TLS plaintext', pid: action.pid };
  if (name === 'EXEC') return { title: `Command executed: ${action.full_command || action.filename || action.comm || 'process'}`, meta: action.comm || '', pid: action.pid };
  return { title: name.replaceAll('_', ' '), meta: action.comm || event.source || '', pid: action.pid };
}

function adjustObservabilityZoom(delta) {
  observabilityTimelineZoom = Math.min(1.8, Math.max(0.65, observabilityTimelineZoom + delta));
  renderObservabilityDashboard();
}

function resetObservabilityZoom() {
  observabilityTimelineZoom = 1;
  renderObservabilityDashboard();
}

function renderObservabilityProcessTree(ebpfEvents) {
  const box = document.getElementById('obs-process-tree');
  if (!box) return;
  const processes = new Map();
  for (const event of ebpfEvents) {
    if (event.pid === undefined) continue;
    const pid = Number(event.pid);
    if (!processes.has(pid)) {
      processes.set(pid, { pid, ppid: event.ppid, comm: event.comm || 'process', events: [] });
    }
    processes.get(pid).events.push(event.event || event.function || 'event');
  }
  if (processes.size === 0) {
    box.innerHTML = '<div class="storage-empty">No process events loaded</div>';
    return;
  }
  box.innerHTML = '';
  for (const proc of Array.from(processes.values()).slice(0, 80)) {
    const row = document.createElement('div');
    row.className = 'obs-process-row';
    row.innerHTML = `
      <div class="obs-process-pid">${esc(String(proc.pid))}</div>
      <div class="obs-process-main">
        <div>${esc(proc.comm)}</div>
        <div class="obs-muted">ppid ${esc(String(proc.ppid || '-'))} · ${esc(proc.events.slice(-4).join(', '))}</div>
      </div>
    `;
    box.appendChild(row);
  }
}

function renderObservabilityResources(boundaryEvents, ebpfEvents) {
  const box = document.getElementById('obs-resource-metrics');
  if (!box) return;
  const resources = [...boundaryEvents, ...ebpfEvents].filter((event) =>
    String(event.kind || '').includes('resource') || String(event.event || '').includes('RESOURCE'));
  if (!resources.length) {
    box.innerHTML = '<div class="storage-empty">No resource samples loaded</div>';
    return;
  }
  const reportSamples = observabilityData?.observability_report?.performance_metrics?.samples || [];
  const derivedSamples = reportSamples.length
    ? reportSamples
    : resourceSamplesFromEvents([...boundaryEvents, ...ebpfEvents]);
  const samplesByTime = new Map();
  for (const sample of derivedSamples) {
    if (sample.wall_time_ns !== undefined) {
      samplesByTime.set(String(sample.wall_time_ns), sample);
    }
  }
  box.innerHTML = '';
  for (const event of resources.slice(-40)) {
    const row = document.createElement('div');
    row.className = 'obs-resource-row';
    const sample = samplesByTime.get(String(event.wall_time_ns || '')) || {};
    const memoryMb = sample.memory_mb ?? (event.total_rss_kb ? Number(event.total_rss_kb) / 1024 : Number(event.rss_kb || 0) / 1024);
    const cpuPercent = sample.cpu_percent ?? event.total_cpu_percent ?? event.cpu_percent ?? 0;
    row.innerHTML = `
      <span>${esc(event.event || 'RESOURCE')}</span>
      <span class="obs-muted">rss ${fmtMemoryMb(memoryMb)} · cpu ${fmtPercent(cpuPercent)}</span>
    `;
    box.appendChild(row);
  }
}

function renderObservabilityTls(events) {
  const box = document.getElementById('obs-tls-events-list');
  if (!box) return;
  if (!events.length) {
    box.innerHTML = '<div class="storage-empty">No TLS plaintext events loaded</div>';
    return;
  }
  box.innerHTML = '';
  for (const event of events.slice(-40)) {
    const row = document.createElement('div');
    row.className = 'obs-resource-row';
    const payload = event.data === null || event.data === undefined ? '' : String(event.data);
    row.innerHTML = `
      <span>${esc(event.function || event.event || 'TLS')}</span>
      <span class="obs-muted">${esc(event.comm || '')} pid=${esc(String(event.pid || '-'))} · ${esc(payload.slice(0, 180))}</span>
    `;
    box.appendChild(row);
  }
}

// ── Run Inspector ──

async function fetchRunDetails(runId) {
  try {
    const r = await fetch(`${GATEWAY}/api/demo/runs/${encodeURIComponent(runId)}${authQuery()}`, { mode: 'cors' });
    if (!r.ok) return;
    const data = await r.json();
    if (!data.ok) return;
    renderRunInspector(data);
    if (activeView === 'observability') loadObservability(runId);
  } catch {
    // Inspector not available — that's fine
  }
}

async function fetchLatestRun() {
  try {
    const r = await fetch(`${GATEWAY}/api/demo/runs/latest${authQuery()}`, { mode: 'cors' });
    if (!r.ok) return;
    const data = await r.json();
    if (!data.ok) return;
    renderRunInspector(data);
    if (activeView === 'observability') loadObservability(data.run_id || '');
  } catch {
    // Not available
  }
}

function renderRunInspector(data) {
  // Merge inspector data into details
  const ts = data.task_state || {};
  const report = data.report || {};

  if (ts.status) setDetailVal('d-status', ts.status, statusColor(ts.status));
  if (ts.stop_reason) setDetailVal('d-stopreason', ts.stop_reason, '');
  if (ts.attempts) setDetailVal('d-attempts', String(ts.attempts), '');
  if (ts.tool_steps !== undefined) setDetailVal('d-toolsteps', String(ts.tool_steps), '');
  if (ts.risk_level) setDetailVal('d-risk', ts.risk_level, riskColor(ts.risk_level));

  // Report prompt metadata
  const pm = report.prompt_metadata || {};
  if (pm.prompt_chars) setDetailVal('d-promptchars', String(pm.prompt_chars), '');
  const mm = pm.model_metadata || {};
  if (mm.input_tokens) setDetailVal('d-inputtok', String(mm.input_tokens), '');
  if (mm.output_tokens) setDetailVal('d-outputtok', String(mm.output_tokens), '');
  if (mm.cache_hit !== undefined) setDetailVal('d-cachehit', mm.cache_hit ? 'YES' : 'no', mm.cache_hit ? 'highlight-green' : '');
  if (pm.prefix_hash) setDetailVal('d-prefixhash', pm.prefix_hash, '');

  // Trace
  const traceDiv = document.getElementById('d-trace');
  if (data.trace && data.trace.length > 0) {
    traceDiv.textContent = JSON.stringify(data.trace, null, 2);
  }
}

function statusColor(s) {
  if (s === 'completed') return 'highlight-green';
  if (s === 'running') return 'highlight-blue';
  if (s === 'stopped') return 'highlight-red';
  return '';
}

// ── Auto-resize textarea ──

const chatInput = document.getElementById('chat-input');
chatInput.addEventListener('input', () => {
  chatInput.style.height = 'auto';
  chatInput.style.height = Math.min(chatInput.scrollHeight, 120) + 'px';
});
window.addEventListener('keydown', (event) => {
  if (activeView !== 'observability' || (!event.ctrlKey && !event.metaKey)) return;
  if (event.key === '+' || event.key === '=') {
    event.preventDefault();
    adjustObservabilityZoom(0.1);
  } else if (event.key === '-') {
    event.preventDefault();
    adjustObservabilityZoom(-0.1);
  } else if (event.key === '0') {
    event.preventDefault();
    resetObservabilityZoom();
  }
});

// ── Init ──

refreshHealth();
setInterval(refreshHealth, 15000);
setInterval(() => {
  if (videoCallActive) updateCallStatus();
}, 1000);
loadFeatureStatus();
updateCallStatus();
initStorageDashboard();
applyAuthUi();
loadAuthStatus().then(() => {
  loadStorageFiles();
  loadConversationHistory();
});
