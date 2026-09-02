// BouleCam Studio Pro UI Renderer Logic

const API_BASE = 'http://127.0.0.1:8090';

// Persistent Individual Camera Profiles (Storage per device/ID)
const DEFAULT_CAM_SETTINGS = {
  isMirrored: false,
  manualRotation: 0,
  currentLens: 0, // 0 = back, 1 = front
  isTorchOn: false,
  isMicEnabled: true,
  isDimScreenActive: false,
  isoIndex: 0,
  evValue: 0,
  shutterIndex: 0,
  wbIndex: 0,
  isAutoAF: true,
  focusValue: 0
};

let isConnected = false;
let activeCamId = 1;
let currentLens = 0;
let isTorchOn = false;
let isMicEnabled = true;
let isAutoAF = true;
let isMirrored = false;
let manualRotation = 0;
let isDimScreenActive = false;
let lastDevicesList = [];

function getDeviceKey(camId, deviceName) {
  if (deviceName && deviceName.trim() && deviceName !== 'Móvil' && deviceName !== 'Desconocido' && deviceName !== 'Sin conexión') {
    return 'dev_' + deviceName.trim().replace(/[^a-zA-Z0-9_-]/g, '_');
  }
  return 'cam_' + camId;
}

function loadAllConfigs() {
  try {
    const raw = localStorage.getItem('boulecam_devices_v2');
    return raw ? JSON.parse(raw) : {};
  } catch (e) {
    return {};
  }
}

function saveAllConfigs(configs) {
  try {
    localStorage.setItem('boulecam_devices_v2', JSON.stringify(configs));
  } catch (e) {}
}

const allConfigs = loadAllConfigs();

function getActiveConfig() {
  const currentDev = lastDevicesList.find(d => d.id === activeCamId);
  const key = getDeviceKey(activeCamId, currentDev?.name);
  if (!allConfigs[key]) {
    allConfigs[key] = { ...DEFAULT_CAM_SETTINGS };
  }
  return allConfigs[key];
}

function saveCurrentConfig(partial) {
  const currentDev = lastDevicesList.find(d => d.id === activeCamId);
  const key = getDeviceKey(activeCamId, currentDev?.name);
  allConfigs[key] = { ...getActiveConfig(), ...partial };
  saveAllConfigs(allConfigs);
}

// DOM Elements
const statusDot = document.getElementById('status-dot');
const statusText = document.getElementById('status-text');
const placeholderBox = document.getElementById('placeholder-box');
const liveStreamImg = document.getElementById('live-stream-img');
const cameraSelectorBar = document.getElementById('camera-selector-bar');

// Top Badges
const badgeResolution = document.getElementById('badge-resolution');
const badgeFps = document.getElementById('badge-fps');
const badgeLatency = document.getElementById('badge-latency');
const badgeBitrate = document.getElementById('badge-bitrate');
const badgeDevice = document.getElementById('badge-device');
const badgeMode = document.getElementById('badge-mode');

// Floating Dock Buttons
const btnFlip = document.getElementById('btn-flip');
const btnTorch = document.getElementById('btn-torch');
const btnMic = document.getElementById('btn-mic');
const btnMirror = document.getElementById('btn-mirror');
const btnRotate180 = document.getElementById('btn-rotate-180');
const btnDimScreen = document.getElementById('btn-dim-screen');

// Connection Tabs
const tabUsb = document.getElementById('tab-usb');
const tabWifi = document.getElementById('tab-wifi');

// Sliders & Controls
const sliderIso = document.getElementById('slider-iso');
const valIso = document.getElementById('val-iso');
const sliderEv = document.getElementById('slider-ev');
const valEv = document.getElementById('val-ev');
const sliderShutter = document.getElementById('slider-shutter');
const valShutter = document.getElementById('val-shutter');
const sliderWb = document.getElementById('slider-wb');
const valWb = document.getElementById('val-wb');
const btnAfToggle = document.getElementById('btn-af-toggle');
const sliderFocus = document.getElementById('slider-focus');

// Maps
const ISO_VALUES = [-1, 100, 200, 400, 800, 1600, 3200];
const SHUTTER_VALUES = [
  { label: 'Auto', ns: -1 },
  { label: '1/4000s', ns: 250000 },
  { label: '1/2000s', ns: 500000 },
  { label: '1/1000s', ns: 1000000 },
  { label: '1/500s', ns: 2000000 },
  { label: '1/250s', ns: 4000000 },
  { label: '1/125s', ns: 8000000 },
  { label: '1/60s', ns: 16666666 }
];
const WB_LABELS = ['Auto', 'Incand. (3200K)', 'Fluor. (4000K)', 'Luz de día (5500K)', 'Nublado (6500K)', 'Sombra (7500K)'];
const WB_MODES = [0, 1, 2, 3, 4, 5];

let isStreamingActive = false;
let isFetchingFrame = false;

// Send command to C++ Backend Daemon
async function sendCommand(action, intParam = 0, longParam = 0, floatParam = 0.0) {
  try {
    const resp = await fetch(`${API_BASE}/api/command`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        action,
        intParam,
        longParam,
        floatParam,
        deviceId: activeCamId
      })
    });
    return await resp.json();
  } catch (err) {
    return null;
  }
}

// Apply active camera's individual config to UI & hardware
function applyActiveConfigToUI(syncHardware = true) {
  const cfg = getActiveConfig();

  isMirrored = !!cfg.isMirrored;
  manualRotation = cfg.manualRotation || 0;
  currentLens = cfg.currentLens || 0;
  isTorchOn = !!cfg.isTorchOn;
  isMicEnabled = cfg.isMicEnabled !== false;
  isDimScreenActive = !!cfg.isDimScreenActive;
  isAutoAF = cfg.isAutoAF !== false;

  // Video transform & OBS URL
  applyVideoTransform();

  // Floating dock button states
  if (btnMirror) btnMirror.classList.toggle('active-cyan', isMirrored);
  if (btnRotate180) btnRotate180.classList.toggle('active-cyan', manualRotation !== 0);
  if (btnFlip) btnFlip.classList.toggle('active', currentLens === 1);
  if (btnTorch) btnTorch.classList.toggle('active-torch', isTorchOn);
  if (btnMic) btnMic.classList.toggle('muted', !isMicEnabled);

  if (btnDimScreen) {
    btnDimScreen.classList.toggle('active-dim', isDimScreenActive);
    btnDimScreen.title = isDimScreenActive
      ? 'Atenuar pantalla ACTIVA en móvil (Clic para restaurar brillo)'
      : 'Atenuar pantalla (Brillo mínimo del móvil)';
  }

  // Sliders & Values
  if (sliderIso) {
    sliderIso.value = cfg.isoIndex || 0;
    const isoVal = ISO_VALUES[cfg.isoIndex || 0];
    if (valIso) valIso.textContent = isoVal === -1 ? 'Auto' : `ISO ${isoVal}`;
  }

  if (sliderEv) {
    sliderEv.value = cfg.evValue || 0;
    const evVal = cfg.evValue || 0;
    if (valEv) valEv.textContent = evVal === 0 ? '0.0 EV' : (evVal > 0 ? `+${evVal * 0.5} EV` : `${evVal * 0.5} EV`);
  }

  if (sliderShutter) {
    sliderShutter.value = cfg.shutterIndex || 0;
    const shVal = SHUTTER_VALUES[cfg.shutterIndex || 0];
    if (valShutter && shVal) valShutter.textContent = shVal.label;
  }

  if (sliderWb) {
    sliderWb.value = cfg.wbIndex || 0;
    if (valWb) valWb.textContent = WB_LABELS[cfg.wbIndex || 0] || 'Auto';
  }

  if (btnAfToggle) {
    btnAfToggle.classList.toggle('active', isAutoAF);
    btnAfToggle.textContent = isAutoAF ? 'AF Auto' : 'Manual';
  }
  if (sliderFocus) {
    sliderFocus.disabled = isAutoAF;
    sliderFocus.value = cfg.focusValue || 0;
  }

  // Synchronize state with camera hardware if online
  if (syncHardware && isConnected) {
    sendCommand(1, currentLens);
    sendCommand(2, isTorchOn ? 1 : 0);
    sendCommand(8, isMicEnabled ? 1 : 0);
    sendCommand(10, isDimScreenActive ? 1 : 0);
    if ((cfg.isoIndex || 0) > 0) sendCommand(3, ISO_VALUES[cfg.isoIndex]);
    if ((cfg.evValue || 0) !== 0) sendCommand(5, cfg.evValue);
    if ((cfg.shutterIndex || 0) > 0) sendCommand(4, 0, SHUTTER_VALUES[cfg.shutterIndex].ns);
    if ((cfg.wbIndex || 0) > 0) sendCommand(6, WB_MODES[cfg.wbIndex]);
    if (!isAutoAF) sendCommand(7, 1, 0, (cfg.focusValue || 0) / 100.0);
  }
}

async function selectCamera(camId) {
  if (activeCamId === camId) return;
  activeCamId = camId;
  try {
    await fetch(`${API_BASE}/api/select_cam?cam=${camId}`, { method: 'POST' });
  } catch (err) {}
  applyActiveConfigToUI(true);
}

function applyVideoTransform() {
  const cfg = getActiveConfig();
  const rot = (cfg.manualRotation || 0) % 360;
  const scale = cfg.isMirrored ? -1 : 1;
  liveStreamImg.style.transform = `rotate(${rot}deg) scaleX(${scale})`;
  updateObsBox(activeCamId);

  // Sync live transform directly with C++ backend and OBS Browser Source in real-time
  fetch(`${API_BASE}/api/cam_transform?cam=${activeCamId}&mirror=${cfg.isMirrored ? 1 : 0}&rot=${rot}`, { method: 'POST' }).catch(() => {});
}

function updateObsBox(camId) {
  const inputObsUrl = document.getElementById('input-obs-url');
  const obsCamBadge = document.getElementById('obs-cam-badge');
  if (inputObsUrl) {
    // Clean, permanent URL: automatically syncs mirror, rotation, mic and lens live in real-time
    inputObsUrl.value = `http://127.0.0.1:8090/obs/${camId}`;
  }
  if (obsCamBadge) {
    obsCamBadge.textContent = `Cam ${camId}`;
  }
}

// Render dynamic camera selector tabs
function renderDeviceTabs(devices, activeId) {
  if (!cameraSelectorBar) return;
  lastDevicesList = devices || [];

  if (devices.length === 0) {
    cameraSelectorBar.innerHTML = `
      <div class="cam-tab active" data-cam-id="1">
        <span class="cam-tab-dot" style="background:var(--accent-orange); box-shadow:none;"></span>
        <span class="cam-tab-name">📹 Cam 1 (Sin señal)</span>
      </div>
    `;
    updateObsBox(1);
    return;
  }

  const html = devices.map(d => {
    const isActive = d.id === activeId;
    const isVert = d.isVertical ? ' (Vertical)' : '';
    return `
      <div class="cam-tab ${isActive ? 'active' : ''}" data-cam-id="${d.id}" title="Seleccionar Cam ${d.id}">
        <span class="cam-tab-dot"></span>
        <span class="cam-tab-name">📹 Cam ${d.id}: ${d.name}${isVert}</span>
      </div>
    `;
  }).join('');

  cameraSelectorBar.innerHTML = html;

  const tabs = cameraSelectorBar.querySelectorAll('.cam-tab');
  tabs.forEach(tab => {
    tab.addEventListener('click', () => {
      const id = parseInt(tab.getAttribute('data-cam-id'));
      selectCamera(id);
    });
  });

  updateObsBox(activeId);
}

// Polling daemon status every 500ms
async function pollStatus() {
  try {
    const res = await fetch(`${API_BASE}/api/status`);
    if (res.ok) {
      const data = await res.json();
      updateUIStatus(data);
    }
  } catch (err) {
    updateUIStatus({
      connected: false,
      fps: 0,
      latencyMs: 0,
      bitrateKbps: 0,
      deviceName: 'BouleCam Service desconectado',
      localIps: [],
      devices: []
    });
  }
}

let lastActiveDeviceId = null;

function updateUIStatus(data) {
  isConnected = data.connected;

  if (data.connected) {
    statusDot.classList.add('connected');
    statusText.textContent = `Conectado: ${data.deviceName || 'Móvil'}`;
    statusText.style.color = 'var(--accent-green)';

    if (!isStreamingActive) {
      isStreamingActive = true;
      requestAnimationFrame(fetchNextFrame);
    }

    if (data.isVertical || (data.width && data.height && data.width < data.height)) {
      liveStreamImg.style.aspectRatio = '9/16';
      liveStreamImg.style.objectFit = 'contain';
      liveStreamImg.style.maxHeight = '92vh';
      if (badgeResolution) badgeResolution.textContent = '1080x1920 (Vertical)';
    } else {
      liveStreamImg.style.aspectRatio = '16/9';
      liveStreamImg.style.objectFit = 'contain';
      liveStreamImg.style.maxHeight = '88vh';
      if (badgeResolution) badgeResolution.textContent = '1920x1080 (HD 60)';
    }

    const latency = Math.round(data.latencyMs || 12);
    if (badgeLatency) badgeLatency.textContent = `${latency} ms`;
    if (badgeFps) badgeFps.textContent = `${Math.round(data.fps || 30)} FPS`;
    if (badgeBitrate) {
      const mbps = (data.bitrateKbps || 4000) / 1000;
      badgeBitrate.textContent = `${mbps.toFixed(1)} Mbps`;
    }
    if (badgeDevice) badgeDevice.textContent = data.deviceName || 'Móvil';
  } else {
    isStreamingActive = false;
    statusDot.classList.remove('connected');
    statusText.textContent = 'Esperando conexión...';
    statusText.style.color = 'var(--accent-orange)';
    placeholderBox.style.display = 'flex';
    liveStreamImg.style.display = 'none';

    if (badgeResolution) badgeResolution.textContent = '1080p';
    if (badgeLatency) badgeLatency.textContent = '-- ms';
    if (badgeFps) badgeFps.textContent = '-- FPS';
    if (badgeBitrate) badgeBitrate.textContent = '-- Mbps';
    if (badgeDevice) badgeDevice.textContent = 'Sin conexión';
  }

  // Detect active device change from server
  const serverActiveId = data.activeDeviceId || 1;
  if (serverActiveId !== lastActiveDeviceId) {
    lastActiveDeviceId = serverActiveId;
    activeCamId = serverActiveId;
    applyActiveConfigToUI(false);
  }

  // Update Multi-Camera Tabs
  renderDeviceTabs(data.devices || [], serverActiveId);

  // Handle USB Cable Connection detection
  const usbAvailable = (data.usbConnected === true);
  if (tabUsb) {
    if (!usbAvailable) {
      tabUsb.disabled = true;
      tabUsb.classList.add('disabled');
      tabUsb.title = 'Cable USB no detectado. Conecta el teléfono por USB para usar este modo.';
      if (tabUsb.classList.contains('active')) {
        tabWifi.classList.add('active');
        tabUsb.classList.remove('active');
        if (badgeMode) badgeMode.textContent = 'Wi-Fi';
      }
    } else {
      tabUsb.disabled = false;
      tabUsb.classList.remove('disabled');
      tabUsb.title = '🔌 Cable USB conectado y listo';
    }
  }
}

let currentBlobUrl = null;

// Anti-freeze frame fetcher
async function fetchNextFrame() {
  if (!isConnected || isFetchingFrame) return;
  isFetchingFrame = true;

  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 350);

  try {
    const res = await fetch(`${API_BASE}/api/snapshot?cam=${activeCamId}`, { signal: controller.signal });
    clearTimeout(timeoutId);

    if (res.status === 200) {
      const blob = await res.blob();
      if (blob.size > 1000) {
        const newUrl = URL.createObjectURL(blob);
        liveStreamImg.src = newUrl;
        liveStreamImg.style.display = 'block';
        placeholderBox.style.display = 'none';

        if (currentBlobUrl) {
          URL.revokeObjectURL(currentBlobUrl);
        }
        currentBlobUrl = newUrl;
      }
    }
  } catch (err) {
    // Timeout recovery
  } finally {
    clearTimeout(timeoutId);
    isFetchingFrame = false;
    if (isConnected) {
      requestAnimationFrame(fetchNextFrame);
    } else {
      isStreamingActive = false;
    }
  }
}

// Setup Interactive Events
function setupEvents() {
  // Tabs (Header Toggle)
  if (tabUsb && tabWifi) {
    tabUsb.addEventListener('click', () => {
      if (tabUsb.disabled || tabUsb.classList.contains('disabled')) {
        alert('Cable USB no detectado.\n\nPor favor, conecta tu teléfono a la computadora con un cable USB para usar este modo.');
        return;
      }
      tabUsb.classList.add('active');
      tabWifi.classList.remove('active');
      if (badgeMode) badgeMode.textContent = 'USB';
    });

    tabWifi.addEventListener('click', () => {
      tabWifi.classList.add('active');
      tabUsb.classList.remove('active');
      if (badgeMode) badgeMode.textContent = 'Wi-Fi';
    });
  }

  // Flip Camera Button (Front / Back Lens)
  if (btnFlip) {
    btnFlip.addEventListener('click', async () => {
      currentLens = currentLens === 0 ? 1 : 0;
      btnFlip.classList.toggle('active', currentLens === 1);
      saveCurrentConfig({ currentLens });
      await sendCommand(1, currentLens);
    });
  }

  // Torch / Flash Button
  if (btnTorch) {
    btnTorch.addEventListener('click', async () => {
      isTorchOn = !isTorchOn;
      btnTorch.classList.toggle('active-torch', isTorchOn);
      saveCurrentConfig({ isTorchOn });
      await sendCommand(2, isTorchOn ? 1 : 0);
    });
  }

  // Mic Mute Button
  if (btnMic) {
    btnMic.addEventListener('click', async () => {
      isMicEnabled = !isMicEnabled;
      btnMic.classList.toggle('muted', !isMicEnabled);
      saveCurrentConfig({ isMicEnabled });
      await sendCommand(8, isMicEnabled ? 1 : 0);
    });
  }

  // Mirror Button (Espejar Video Horizontalmente)
  if (btnMirror) {
    btnMirror.addEventListener('click', () => {
      isMirrored = !isMirrored;
      btnMirror.classList.toggle('active-cyan', isMirrored);
      saveCurrentConfig({ isMirrored });
      applyVideoTransform();
    });
  }

  // Rotate 180 Button (Invertir Video)
  if (btnRotate180) {
    btnRotate180.addEventListener('click', () => {
      manualRotation = (manualRotation + 180) % 360;
      btnRotate180.classList.toggle('active-cyan', manualRotation !== 0);
      saveCurrentConfig({ manualRotation });
      applyVideoTransform();
    });
  }

  // Dim Screen Button (Atenuación de pantalla en móvil)
  if (btnDimScreen) {
    btnDimScreen.addEventListener('click', async () => {
      isDimScreenActive = !isDimScreenActive;
      btnDimScreen.classList.toggle('active-dim', isDimScreenActive);
      btnDimScreen.title = isDimScreenActive
        ? 'Atenuar pantalla ACTIVA en móvil (Clic para restaurar brillo)'
        : 'Atenuar pantalla (Brillo mínimo del móvil)';
      saveCurrentConfig({ isDimScreenActive });
      await sendCommand(10, isDimScreenActive ? 1 : 0);
    });
  }

  // ISO Slider
  if (sliderIso) {
    sliderIso.addEventListener('input', async (e) => {
      const idx = parseInt(e.target.value);
      const val = ISO_VALUES[idx];
      if (valIso) valIso.textContent = val === -1 ? 'Auto' : `ISO ${val}`;
      saveCurrentConfig({ isoIndex: idx });
      await sendCommand(3, val);
    });
  }

  // EV Slider
  if (sliderEv) {
    sliderEv.addEventListener('input', async (e) => {
      const val = parseInt(e.target.value);
      if (valEv) valEv.textContent = val === 0 ? '0.0 EV' : (val > 0 ? `+${val * 0.5} EV` : `${val * 0.5} EV`);
      saveCurrentConfig({ evValue: val });
      await sendCommand(5, val);
    });
  }

  // Shutter Slider
  if (sliderShutter) {
    sliderShutter.addEventListener('input', async (e) => {
      const idx = parseInt(e.target.value);
      const item = SHUTTER_VALUES[idx];
      if (valShutter && item) valShutter.textContent = item.label;
      saveCurrentConfig({ shutterIndex: idx });
      await sendCommand(4, 0, item.ns);
    });
  }

  // White Balance Slider
  if (sliderWb) {
    sliderWb.addEventListener('input', async (e) => {
      const idx = parseInt(e.target.value);
      if (valWb) valWb.textContent = WB_LABELS[idx];
      saveCurrentConfig({ wbIndex: idx });
      await sendCommand(6, WB_MODES[idx]);
    });
  }

  // OBS Studio URL Copy Button
  const btnCopyObs = document.getElementById('btn-copy-obs');
  const inputObsUrl = document.getElementById('input-obs-url');
  if (btnCopyObs) {
    btnCopyObs.addEventListener('click', () => {
      if (inputObsUrl) {
        navigator.clipboard.writeText(inputObsUrl.value).then(() => {
          const orig = btnCopyObs.textContent;
          btnCopyObs.textContent = '✓ ¡Copiada!';
          btnCopyObs.style.background = 'var(--accent-green)';
          setTimeout(() => {
            btnCopyObs.textContent = orig;
            btnCopyObs.style.background = '';
          }, 1500);
        });
      }
    });
  }

  // Focus Toggle & Slider
  if (btnAfToggle) {
    btnAfToggle.addEventListener('click', async () => {
      isAutoAF = !isAutoAF;
      btnAfToggle.classList.toggle('active', isAutoAF);
      btnAfToggle.textContent = isAutoAF ? 'AF Auto' : 'Manual';
      if (sliderFocus) sliderFocus.disabled = isAutoAF;
      saveCurrentConfig({ isAutoAF });
      if (isAutoAF) {
        await sendCommand(7, 0, 0, 0.0);
      } else {
        const dist = (sliderFocus ? sliderFocus.value : 0) / 100.0;
        await sendCommand(7, 1, 0, dist);
      }
    });
  }

  if (sliderFocus) {
    sliderFocus.addEventListener('input', async (e) => {
      if (!isAutoAF) {
        const val = parseInt(e.target.value);
        saveCurrentConfig({ focusValue: val });
        await sendCommand(7, 1, 0, val / 100.0);
      }
    });
  }

  // Initial load
  applyActiveConfigToUI(false);
}

// Start polling
setInterval(pollStatus, 500);
setupEvents();
pollStatus();
