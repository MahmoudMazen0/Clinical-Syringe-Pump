/**
 * Syringe Pump Pro – Dashboard Application
 * Backend: ESP32 via WebSocket  ws://192.168.4.1:81
 */

'use strict';

// ── Constants ──────────────────────────────────────────────────────────────
const WS_PORT           = 81;
const RECONNECT_DELAY   = 4000;   // ms before each reconnect attempt
const VOL_MIN           = 0.1;
const VOL_MAX           = 10.0;
const RATE_MIN          = 0.1;
const RATE_MAX          = 150.0;
const OCCLUSION_THRESH  = 600;    // mirror of ESP32 constant
const LS_IP_KEY         = 'sp_esp32_ip';
const DEFAULT_IP        = '192.168.43.1'; // Android hotspot default gateway — update to actual ESP32 IP

function getSavedIP()  { return localStorage.getItem(LS_IP_KEY) || DEFAULT_IP; }
function saveIP(ip)    { localStorage.setItem(LS_IP_KEY, ip); }
function getWsUrl()    { return `ws://${getSavedIP()}:${WS_PORT}`; }

// ── State ──────────────────────────────────────────────────────────────────
const state = {
  ws:        null,
  connState: 'disconnected',   // 'connected' | 'connecting' | 'disconnected'
  reconnectTimer: null,
  reconnectCount: 0,
  telemetry: {
    deliveredVol:  0,
    setRate:       5.0,
    setVolume:     5.0,
    running:       false,
    occlusion:     false,
    empty:         false,
    doseCompleted: false,
    fsrRaw:        0,
    actualRate:    0,
    minsRemaining: -1,
  },
};

// ── DOM References ─────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);

// ── Build UI ───────────────────────────────────────────────────────────────
function buildUI() {
  const root = $('root');
  root.innerHTML = `
    <!-- HEADER -->
    <header class="app-header">
      <div class="header-left">
        <span class="app-title">Syringe Pump Pro</span>
        <span class="app-subtitle">Medical Infusion Control</span>
      </div>
      <div class="conn-badge disconnected" id="conn-badge">
        <span class="conn-dot"></span>
        <span id="conn-text">Offline</span>
      </div>
    </header>

    <!-- RECONNECT BANNER -->
    <div class="reconnect-banner hidden" id="reconnect-banner">
      <div class="reconnect-spinner"></div>
      <span id="reconnect-text">Connecting to pump…</span>
    </div>

    <!-- IP CONFIG CARD -->
    <div class="card ip-config-card" id="ip-config-card">
      <div class="card-label">ESP32 Connection</div>
      <div class="ip-row">
        <div class="ip-prefix">ws://</div>
        <input
          class="ip-input" id="inp-ip"
          type="text" inputmode="decimal"
          placeholder="e.g. 192.168.43.105"
          value="${getSavedIP()}"
          aria-label="ESP32 IP Address"
          spellcheck="false" autocomplete="off"
        />
        <div class="ip-suffix">:${WS_PORT}</div>
        <button class="btn btn-connect" id="btn-connect" aria-label="Connect to ESP32">
          Connect
        </button>
      </div>
      <div class="ip-hint" id="ip-hint">
        💡 Find the ESP32 IP in Arduino Serial Monitor after it joins your WiFi.
      </div>
    </div>

    <!-- TELEMETRY CARD -->
    <div class="card" id="tele-card">
      <div class="card-label">Live Telemetry</div>

      <!-- Volume Display -->
      <div class="vol-display">
        <div class="vol-numbers">
          <span class="vol-delivered" id="vol-delivered">0.000</span>
          <span class="vol-sep">/</span>
          <span class="vol-total" id="vol-total">5.000</span>
          <span class="vol-unit">mL</span>
        </div>
        <div style="font-size:11px;color:var(--text-dim);margin-top:6px;letter-spacing:0.5px;">
          DELIVERED &nbsp;/&nbsp; TARGET
        </div>
      </div>

      <!-- Progress Bar -->
      <div class="progress-wrap" title="Infusion progress">
        <div class="progress-fill" id="progress-fill" style="width:0%"></div>
      </div>

      <!-- Status Banner -->
      <div class="status-banner standby" id="status-banner" role="status" aria-live="polite">
        <span id="status-icon">⏸</span>
        <span id="status-text">System Standby</span>
      </div>

      <!-- Metrics Row -->
      <div class="metrics-row">
        <div class="metric-tile" id="rate-tile">
          <div class="metric-label">Actual Rate</div>
          <div class="metric-value" id="actual-rate">0.0</div>
          <div class="metric-unit">mL / hr</div>
        </div>
        <div class="metric-tile" id="time-tile">
          <div class="metric-label">Time Remaining</div>
          <div class="metric-value" id="mins-remaining">--</div>
          <div class="metric-unit">minutes</div>
        </div>
      </div>

      <!-- FSR Pressure Bar -->
      <div class="fsr-row" title="Force Sensor Reading (occlusion detector)">
        <span class="fsr-label">Pressure</span>
        <div class="fsr-bar-wrap">
          <div class="fsr-bar-fill safe" id="fsr-bar" style="width:0%"></div>
        </div>
        <span class="fsr-value" id="fsr-value">0</span>
      </div>

      <!-- Disconnected lock -->
      <div class="disconnected-lock hidden" id="tele-lock">🔒</div>
    </div>

    <!-- SETTINGS CARD -->
    <div class="card" style="position:relative;" id="settings-card">
      <div class="card-label">Infusion Settings</div>

      <div class="input-group">
        <label class="input-label" for="inp-volume">
          Target Volume
          <span class="input-range-hint">0.1 – 10.0 mL</span>
        </label>
        <input
          class="input-field" id="inp-volume" type="number"
          min="0.1" max="10" step="0.1"
          value="5.0" inputmode="decimal"
          aria-label="Target Volume in millilitres"
        />
      </div>

      <div class="input-group">
        <label class="input-label" for="inp-rate">
          Flow Rate
          <span class="input-range-hint">0.1 – 150.0 mL/hr</span>
        </label>
        <input
          class="input-field" id="inp-rate" type="number"
          min="0.1" max="150" step="0.1"
          value="5.0" inputmode="decimal"
          aria-label="Flow rate in millilitres per hour"
        />
      </div>

      <button class="btn btn-apply" id="btn-apply" aria-label="Apply settings to pump">
        ⚙ APPLY SETTINGS
      </button>

      <!-- Disconnected lock -->
      <div class="disconnected-lock hidden" id="settings-lock">🔒</div>
    </div>

    <!-- CONTROL CARD -->
    <div class="card" style="position:relative;" id="control-card">
      <div class="card-label">Pump Control</div>

      <div class="btn-row btn-row-2">
        <button class="btn btn-start" id="btn-start" aria-label="Start infusion">
          ▶ START
        </button>
        <button class="btn btn-pause" id="btn-pause" aria-label="Pause infusion">
          ⏸ PAUSE
        </button>
      </div>

      <button class="btn btn-reset" id="btn-reset" aria-label="Reset dose">
        ↺ &nbsp;RESET DOSE
      </button>

      <!-- Disconnected lock -->
      <div class="disconnected-lock hidden" id="control-lock">🔒</div>
    </div>

    <!-- FOOTER -->
    <footer class="app-footer">
      Syringe Pump Pro &nbsp;·&nbsp; ESP32 WebSocket Control &nbsp;·&nbsp; Medical Equipment v1.0
    </footer>
  `;

  // Wire up buttons
  $('btn-apply').addEventListener('click', handleApply);
  $('btn-start').addEventListener('click', handleStart);
  $('btn-pause').addEventListener('click', handlePause);
  $('btn-reset').addEventListener('click', handleReset);
  $('btn-connect').addEventListener('click', handleConnectBtn);

  // Allow pressing Enter in IP field
  $('inp-ip').addEventListener('keydown', e => {
    if (e.key === 'Enter') handleConnectBtn();
  });
}

// ── WebSocket ──────────────────────────────────────────────────────────────
function handleConnectBtn() {
  const ipField = $('inp-ip');
  const hint    = $('ip-hint');
  const raw     = ipField.value.trim()
    .replace(/^ws:\/\//i, '')    // strip leading ws://
    .replace(/:\d+$/, '');        // strip trailing :port

  // Basic IP / hostname validation
  const ipPattern = /^(\d{1,3}\.){3}\d{1,3}$|^[a-zA-Z0-9.-]+$/;
  if (!raw || !ipPattern.test(raw)) {
    ipField.classList.add('error');
    if (hint) { hint.textContent = '❌ Enter a valid IP address (e.g. 192.168.43.105).'; hint.style.color = 'var(--red-light)'; }
    return;
  }

  ipField.classList.remove('error');
  if (hint) { hint.textContent = `🔄 Connecting to ws://${raw}:${WS_PORT} …`; hint.style.color = 'var(--text-dim)'; }

  saveIP(raw);

  // Close existing connection and reconnect with new IP
  if (state.ws) {
    state.ws.onclose = null; // prevent auto-reschedule
    state.ws.onerror = null;
    state.ws.close();
    state.ws = null;
  }
  clearTimeout(state.reconnectTimer);
  state.reconnectCount = 0;

  connect();
}

function connect() {
  if (state.ws && (state.ws.readyState === WebSocket.OPEN ||
                   state.ws.readyState === WebSocket.CONNECTING)) return;

  setConnState('connecting');

  const url = getWsUrl();
  // Keep the IP field in sync
  const ipField = $('inp-ip');
  if (ipField) ipField.value = getSavedIP();

  const ws = new WebSocket(url);
  state.ws = ws;

  ws.onopen = () => {
    state.reconnectCount = 0;
    setConnState('connected');
    const hint = $('ip-hint');
    if (hint) { hint.textContent = `✅ Connected to ws://${getSavedIP()}:${WS_PORT}`; hint.style.color = 'var(--green-light)'; }
    showToast('Connected to pump.', 'success');
  };

  ws.onmessage = evt => {
    try {
      const data = JSON.parse(evt.data);
      Object.assign(state.telemetry, data);
      render();
    } catch (e) {
      console.warn('WS parse error:', e);
    }
  };

  // Keep onerror and onclose separate to avoid double-reconnect
  ws.onerror = () => {
    console.warn('[WS] Socket error — connection will close.');
  };

  ws.onclose = () => {
    if (state.connState === 'connected') showToast('Connection lost. Reconnecting…', 'error');
    setConnState('disconnected');
    const hint = $('ip-hint');
    if (hint && state.reconnectCount === 0) {
      hint.textContent = `⚠️ Could not reach ws://${getSavedIP()}:${WS_PORT} — check IP and WiFi.`;
      hint.style.color = 'var(--amber-light)';
    }
    scheduleReconnect();
  };
}

function scheduleReconnect() {
  clearTimeout(state.reconnectTimer);
  state.reconnectTimer = setTimeout(() => {
    state.reconnectCount++;
    const txt = $('reconnect-text');
    if (txt) txt.textContent = `Reconnecting… (attempt ${state.reconnectCount})`;
    connect();
  }, RECONNECT_DELAY);
}

/**
 * Heartbeat — every 1 s, check that the actual WS readyState matches the
 * displayed badge. Fixes the "Connected" badge getting stuck after the
 * previous browser-session console manipulation or a silent drop.
 */
function startHeartbeat() {
  setInterval(() => {
    const wsOpen = state.ws && state.ws.readyState === WebSocket.OPEN;
    if (!wsOpen && state.connState === 'connected') {
      // Badge is lying — correct it
      setConnState('disconnected');
      scheduleReconnect();
    }
  }, 1000);
}

function send(payload) {
  const wsOpen = state.ws && state.ws.readyState === WebSocket.OPEN;
  if (!wsOpen) {
    // Auto-correct stale badge before showing the toast
    if (state.connState === 'connected') setConnState('disconnected');
    showToast('Not connected — cannot send command.', 'error');
    return false;
  }
  state.ws.send(JSON.stringify(payload));
  return true;
}

function setConnState(s) {
  state.connState = s;
  const badge = $('conn-badge');
  const text  = $('conn-text');
  const banner = $('reconnect-banner');
  if (!badge) return;

  badge.className = `conn-badge ${s}`;
  text.textContent = s === 'connected' ? 'Connected' : s === 'connecting' ? 'Connecting' : 'Offline';

  const locked = s !== 'connected';
  ['settings-lock','control-lock'].forEach(id => {
    const el = $(id);
    if (el) el.classList.toggle('hidden', !locked);
  });

  if (banner) banner.classList.toggle('hidden', s === 'connected');
  render();
}

// ── Render ─────────────────────────────────────────────────────────────────
function render() {
  const t = state.telemetry;
  const connected = state.connState === 'connected';

  // Volumes
  const volDelivered = $('vol-delivered');
  const volTotal     = $('vol-total');
  if (volDelivered) volDelivered.textContent = t.deliveredVol.toFixed(3);
  if (volTotal)     volTotal.textContent     = t.setVolume.toFixed(3);

  // Progress
  const pct = t.setVolume > 0 ? Math.min(100, (t.deliveredVol / t.setVolume) * 100) : 0;
  const fill = $('progress-fill');
  if (fill) fill.style.width = pct.toFixed(1) + '%';

  // Status Banner
  const banner     = $('status-banner');
  const statusIcon = $('status-icon');
  const statusText = $('status-text');
  if (banner) {
    let cls = 'standby', icon = '⏸', txt = 'System Standby';
    if (!connected) {
      cls = 'standby'; icon = '🔌'; txt = 'Disconnected';
    } else if (t.occlusion) {
      cls = 'occlusion'; icon = '🚨'; txt = 'ALARM: OCCLUSION DETECTED';
    } else if (t.empty) {
      cls = 'empty'; icon = '⚠️'; txt = 'ALARM: SYRINGE EMPTY';
    } else if (t.doseCompleted) {
      cls = 'completed'; icon = '✅'; txt = 'DOSE COMPLETED';
    } else if (t.running) {
      cls = 'infusing'; icon = '💧'; txt = 'Infusing…';
    }
    banner.className = `status-banner ${cls}`;
    if (statusIcon) statusIcon.textContent = icon;
    if (statusText) statusText.textContent = txt;
  }

  // Metrics
  const rateTile  = $('rate-tile');
  const timeTile  = $('time-tile');
  const actualRateEl   = $('actual-rate');
  const minsRemainingEl = $('mins-remaining');
  if (actualRateEl)    actualRateEl.textContent    = t.actualRate.toFixed(1);
  if (minsRemainingEl) minsRemainingEl.textContent = (t.minsRemaining < 0) ? '--' : t.minsRemaining.toFixed(1);
  if (rateTile) rateTile.classList.toggle('active', t.running && connected);
  if (timeTile) timeTile.classList.toggle('active', t.running && connected);

  // FSR bar (0-4095 ADC)
  const fsrBar = $('fsr-bar');
  const fsrVal = $('fsr-value');
  const fsrPct = Math.min(100, (t.fsrRaw / 4095) * 100);
  if (fsrBar) {
    fsrBar.style.width = fsrPct.toFixed(1) + '%';
    const fsrClass = t.fsrRaw >= OCCLUSION_THRESH ? 'danger' : t.fsrRaw >= OCCLUSION_THRESH * 0.7 ? 'warn' : 'safe';
    fsrBar.className = `fsr-bar-fill ${fsrClass}`;
  }
  if (fsrVal) fsrVal.textContent = t.fsrRaw;

  // Button states
  const btnStart = $('btn-start');
  const btnPause = $('btn-pause');
  if (btnStart) btnStart.disabled = !connected || t.running || t.doseCompleted;
  if (btnPause) btnPause.disabled = !connected || !t.running;
}

// ── Validation ─────────────────────────────────────────────────────────────
function validate() {
  const volRaw  = parseFloat($('inp-volume').value);
  const rateRaw = parseFloat($('inp-rate').value);

  const errors = [];
  if (isNaN(volRaw)  || volRaw  < VOL_MIN  || volRaw  > VOL_MAX)
    errors.push(`Volume must be between ${VOL_MIN} and ${VOL_MAX} mL.`);
  if (isNaN(rateRaw) || rateRaw < RATE_MIN || rateRaw > RATE_MAX)
    errors.push(`Rate must be between ${RATE_MIN} and ${RATE_MAX} mL/hr.`);

  if (errors.length) {
    const volField  = $('inp-volume');
    const rateField = $('inp-rate');
    if (isNaN(volRaw)  || volRaw  < VOL_MIN  || volRaw  > VOL_MAX)  volField.classList.add('error');
    else volField.classList.remove('error');
    if (isNaN(rateRaw) || rateRaw < RATE_MIN || rateRaw > RATE_MAX) rateField.classList.add('error');
    else rateField.classList.remove('error');

    showModal({
      icon: '⚠️',
      title: 'Invalid Input',
      body: errors.join('\n\n') + '\n\nNo command has been sent to the pump.',
      type: 'alert',
    });
    return null;
  }

  $('inp-volume').classList.remove('error');
  $('inp-rate').classList.remove('error');
  return { volume: volRaw, rate: rateRaw };
}

// ── Command Handlers ────────────────────────────────────────────────────────
function handleApply() {
  const vals = validate();
  if (!vals) return;
  if (send({ cmd: 'settings', rate: vals.rate, volume: vals.volume })) {
    showToast(`Settings applied: ${vals.volume} mL @ ${vals.rate} mL/hr`, 'success');
  }
}

function handleStart() {
  const vals = validate();
  if (!vals) return;
  // Apply settings first, then start
  send({ cmd: 'settings', rate: vals.rate, volume: vals.volume });
  setTimeout(() => {
    if (send({ cmd: 'start' })) {
      showToast('Infusion started.', 'success');
    }
  }, 80);
}

function handlePause() {
  if (send({ cmd: 'pause' })) {
    showToast('Infusion paused.', 'info');
  }
}

function handleReset() {
  showModal({
    icon: '↺',
    title: 'Confirm Reset',
    body: 'Are you sure you want to reset the current dose?\n\nDelivered volume will be cleared and the pump will stop.',
    type: 'confirm',
    onConfirm: () => {
      if (send({ cmd: 'reset' })) {
        showToast('Dose reset.', 'info');
      }
    },
  });
}

// ── Modal ──────────────────────────────────────────────────────────────────
function showModal({ icon, title, body, type, onConfirm }) {
  const overlay  = $('modal-overlay');
  const iconEl   = $('modal-icon');
  const titleEl  = $('modal-title');
  const bodyEl   = $('modal-body');
  const actionsEl = $('modal-actions');

  iconEl.textContent  = icon;
  titleEl.textContent = title;
  bodyEl.textContent  = body;
  actionsEl.innerHTML = '';

  if (type === 'confirm') {
    const cancel = document.createElement('button');
    cancel.className = 'btn btn-modal-cancel';
    cancel.textContent = 'Cancel';
    cancel.onclick = hideModal;

    const confirm = document.createElement('button');
    confirm.className = 'btn btn-modal-confirm';
    confirm.textContent = 'Reset';
    confirm.onclick = () => { hideModal(); onConfirm && onConfirm(); };

    actionsEl.appendChild(cancel);
    actionsEl.appendChild(confirm);
  } else {
    const ok = document.createElement('button');
    ok.className = 'btn btn-modal-ok';
    ok.textContent = 'Understood';
    ok.onclick = hideModal;
    actionsEl.appendChild(ok);
  }

  overlay.classList.remove('hidden');
}

function hideModal() {
  $('modal-overlay').classList.add('hidden');
}

// Close modal on overlay click (outside card)
$('modal-overlay').addEventListener('click', e => {
  if (e.target === $('modal-overlay')) hideModal();
});

// ── Toast ──────────────────────────────────────────────────────────────────
function showToast(msg, type = 'info') {
  const container = $('toast-container');

  // Deduplicate: don't stack the same message
  for (const existing of container.children) {
    if (existing.textContent === msg) return;
  }

  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  toast.textContent = msg;
  container.appendChild(toast);

  setTimeout(() => {
    toast.classList.add('fade-out');
    setTimeout(() => toast.remove(), 320);
  }, 3000);
}

// Clear error highlighting on input
function clearInputError(id) {
  const el = $(id);
  if (el) el.classList.remove('error');
}

// ── Keyboard accessibility ─────────────────────────────────────────────────
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') hideModal();
});

// ── Boot ───────────────────────────────────────────────────────────────────
buildUI();

// Attach input error clearing after UI is built
['inp-volume','inp-rate'].forEach(id => {
  const el = $(id);
  if (el) el.addEventListener('input', () => clearInputError(id));
});

connect();
startHeartbeat();
