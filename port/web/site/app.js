// The page around the game: browser checks, the import of the player's GTA2, storage, and the game itself
// (reg2.js, the Emscripten build of the port) with what a browser adds: full screen, orientation, touch controls,
// pausing when hidden, saving the player's files as the game writes them.
import * as store from './storage.js';
import { t, applyTranslations } from './i18n.js';
import { TouchControls } from './touch.js';

const $ = (id) => document.getElementById(id);
const GAME_DIR = '/reG2';
const isTouchDevice = matchMedia('(pointer: coarse)').matches || navigator.maxTouchPoints > 0;
const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const standalone = matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;

// The page's own settings, per browser.
const prefs = Object.assign({ quality: 'auto', touchMode: 'auto', touchSize: 100, touchOpacity: 60 },
  readJson('reg2-prefs'));

function readJson(key) {
  try {
    return JSON.parse(localStorage.getItem(key)) || {};
  } catch {
    return {};
  }
}

function savePrefs() {
  try {
    localStorage.setItem('reg2-prefs', JSON.stringify(prefs));
  } catch {}
}

function show(id, on = true) {
  $(id).hidden = !on;
}

function formatMB(bytes) {
  return Math.round(bytes / 1048576) + ' MB';
}

// ---------------------------------------------------------------------------------------------------------------
// Start-up

// build.sh: the folder of this build's game and importer.
const BUILD = '@BUILD@/';

applyTranslations();
registerServiceWorker();
// Over plain HTTP (a LAN address) the browser leaves out the service worker, storage persistence, wake lock and
// the tab lock: the page says so once, and works without them.
show('insecure', !window.isSecureContext);
start();

// A tab opened before a new build was published asks for its old build folder, gone from the site: it reloads once
// for the new one, instead of failing.
async function reloadIfBuildGone() {
  try {
    if (!navigator.onLine || sessionStorage.getItem('reg2-reloaded')) return false;
    const response = await fetch(BUILD + 'reg2.js', { method: 'HEAD', cache: 'no-store' });
    if (response.status !== 404) return false;
    sessionStorage.setItem('reg2-reloaded', '1');
    location.reload();
    return true;
  } catch {
    return false;
  }
}

// The service worker (sw.js) keeps the site for offline starts and gets new builds by itself.
function registerServiceWorker() {
  if ('serviceWorker' in navigator && location.protocol !== 'file:') {
    navigator.serviceWorker.register('sw.js').catch((e) => console.warn('service worker:', e));
  }
}


function missingFeatures() {
  const missing = [];
  if (typeof WebAssembly !== 'object') missing.push(t('needWasm'));
  let gl = null;
  try {
    gl = document.createElement('canvas').getContext('webgl2');
  } catch {}
  if (!gl) missing.push(t('needWebgl'));
  if (!('indexedDB' in window)) missing.push(t('needStorage'));
  if (typeof Worker !== 'function') missing.push(t('needWorkers'));
  return missing;
}

async function start() {
  const missing = missingFeatures();
  if (!missing.length && !(await store.available())) missing.push(t('needStoragePrivate'));
  if (missing.length) {
    $('support-list').replaceChildren(...missing.map((m) => Object.assign(document.createElement('li'), { textContent: m })));
    show('support');
    return;
  }
  if (!(await takeStorage(false))) {
    show('other-tab');
    $('take-over').onclick = async () => {
      if (await takeStorage(true)) {
        show('other-tab', false);
        showHome();
      }
    };
    return;
  }
  showHome();
}

// Only one tab owns the storage (Web Locks): another tab can take it over, and this one then stops.
let storageLost = false;
function takeStorage(steal) {
  if (!navigator.locks) return Promise.resolve(true);
  return new Promise((resolve) => {
    navigator.locks.request('reg2-storage', steal ? { steal: true } : { ifAvailable: true }, (lock) => {
      resolve(!!lock);
      return lock ? new Promise(() => {}) : undefined;
    }).catch(() => {
      storageLost = true;
      game.stop(t('takenTitle'), t('takenText'));
      if (!game.module) {
        show('import', false);
        show('ready', false);
        show('other-tab');
      }
    });
  });
}

async function showHome() {
  const install = await store.getInstall();
  if (install && install.complete) {
    showReady(install);
  } else {
    showImport(install && !install.complete ? t('importInterrupted') : '');
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Import

let worker = null;
let pending = null; // {entries, root}

function showImport(message) {
  show('ready', false);
  show('import');
  show('report', false);
  show('progress', false);
  showImportError(message);
}

function showImportError(message) {
  $('import-error').textContent = message || '';
  show('import-error', !!message);
}

function newWorker() {
  worker?.terminate();
  worker = new Worker(BUILD + 'import-worker.js');
  return worker;
}

$('pick-folder').onclick = () => $('folder-input').click();
$('pick-file').onclick = () => $('file-input').click();
$('folder-input').onchange = (e) => {
  const files = [...e.target.files];
  e.target.value = '';
  if (!files.length) return;
  const entries = files.map((file) => ({ path: file.webkitRelativePath || file.name, file }));
  checkSource(entries, entries[0].path.split('/')[0]);
};
$('file-input').onchange = (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (file) checkSource([{ path: file.name, file }], file.name);
};

const drop = $('drop');
drop.addEventListener('dragover', (e) => {
  e.preventDefault();
  drop.classList.add('over');
});
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', async (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  const items = [...e.dataTransfer.items].map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  if (!items.length) {
    const file = e.dataTransfer.files[0];
    if (file) checkSource([{ path: file.name, file }], file.name);
    return;
  }
  const entries = [];
  for (const item of items) await readEntry(item, '', entries);
  if (!entries.length) return;
  checkSource(entries, items.length === 1 ? items[0].name : '');
});

// A dropped file or folder, recursively, as {path, file}.
async function readEntry(entry, base, out) {
  const path = base ? base + '/' + entry.name : entry.name;
  if (entry.isFile) {
    out.push({ path, file: await new Promise((res, rej) => entry.file(res, rej)) });
  } else if (entry.isDirectory) {
    const reader = entry.createReader();
    for (;;) {
      const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
      if (!batch.length) break;
      for (const child of batch) await readEntry(child, path, out);
    }
  }
}

function runWorker(message, onFile) {
  return new Promise((resolve, reject) => {
    const w = newWorker();
    w.onmessage = ({ data }) => {
      if (data.type === 'progress') {
        setProgress(data.total ? data.done / data.total : 0, t('progressCopy', formatMB(data.done), formatMB(data.total)));
      } else if (data.type === 'file') {
        onFile?.(data);
      } else if (data.type === 'result') {
        resolve(data);
      } else if (data.type === 'error') {
        reject(new Error(data.message));
      }
    };
    w.onerror = async (e) => {
      if (!(await reloadIfBuildGone())) reject(new Error(e.message || 'worker'));
    };
    w.postMessage(message);
  });
}

function setProgress(fraction, label) {
  $('progress-bar').value = fraction;
  $('progress-label').textContent = label;
}

async function installedDataPaths() {
  const install = await store.getInstall();
  return install && install.complete ? (await store.listFiles()).filter(store.isData) : [];
}

async function checkSource(entries, root) {
  showImportError('');
  show('report', false);
  show('progress');
  setProgress(0, t('reading'));
  $('progress-bar').removeAttribute('value');
  try {
    const { result } = await runWorker({ cmd: 'check', entries, root, installed: await installedDataPaths() });
    show('progress', false);
    if (!result.ok) {
      showImportError(result.error || t('notGta2'));
      return;
    }
    pending = { entries, root };
    showReport(result);
  } catch (e) {
    show('progress', false);
    showImportError(t('importFailed', e.message));
  }
}

function checkItem(state, text) {
  return Object.assign(document.createElement('li'), { className: state, textContent: text });
}

function reportItems(check, addOnly) {
  const items = [];
  if (addOnly) items.push(checkItem('yes', t('addOnly')));
  else if (check.missing.length) items.push(checkItem('no', t('gameMissing', check.missing.slice(0, 6).join(', '))));
  else items.push(checkItem('yes', t('gameFiles')));
  const [music, total] = check.music;
  items.push(checkItem(music === total ? 'yes' : music ? 'part' : 'no', t('music', music, total)));
  items.push(checkItem(check.movie ? 'yes' : 'no', t(check.movie ? 'movie' : 'noMovie')));
  const languages = Object.entries(check.languages).filter(([, has]) => has).map(([name]) => name);
  items.push(checkItem(languages.length ? 'yes' : 'no', t('languages', languages.join(', ') || '-')));
  return items;
}

function showReport(result) {
  const check = result.check;
  $('report-list').replaceChildren(...reportItems(check, result.addOnly));
  const blocked = !result.addOnly && check.missing.length > 0;
  let note = '';
  if (blocked) note = t('incomplete');
  else if (check.music[0] < check.music[1] && !result.addOnly) note = t('noMusicNote');
  $('report-note').textContent = note;
  $('do-import').disabled = blocked;
  show('report');
  $('do-import').focus();
}

$('cancel-import').onclick = () => {
  pending = null;
  show('report', false);
  $('pick-file').focus();
};

$('abort-import').onclick = async () => {
  worker?.terminate();
  worker = null;
  show('progress', false);
  showImportError(t('importCancelled'));
  if (importing) await store.setInstall({ complete: false });
  importing = false;
};

let importing = false;
$('do-import').onclick = async () => {
  if (!pending) return;
  show('report', false);
  show('progress');
  setProgress(0, t('reading'));
  const before = await installedDataPaths();
  const written = new Set();
  let writes = Promise.resolve();
  let writeError = null;
  importing = true;
  try {
    const started = performance.now();
    const { result, fullInstall, times } = await runWorker({ cmd: 'install', ...pending, installed: before }, (file) => {
      setProgress(file.done / file.total, t('progressStore', formatMB(file.done), formatMB(file.total)));
      writes = writes.then(async () => {
        if (writeError) return;
        try {
          // From the first file on, the install is incomplete until the last one is stored.
          if (!written.size) await store.setInstall({ complete: false });
          await store.writeFile(file.path, file.data);
          written.add(file.path);
        } catch (e) {
          writeError = e;
        }
      });
    });
    await writes;
    if (writeError) throw writeError;
    if (new URLSearchParams(location.search).has('debug')) {
      console.log('import: ' + JSON.stringify({ ...times, total: Math.round(performance.now() - started) }));
    }
    if (!result.ok) {
      importing = false;
      show('progress', false);
      if (written.size) await store.setInstall({ complete: false });
      showImportError(result.error);
      return;
    }
    // A whole new copy replaces the data: what the previous one had and this one has not goes.
    if (fullInstall) {
      const stale = before.filter((p) => !written.has(p));
      if (stale.length) await store.deleteFiles(stale);
    }
    await store.setInstall({ complete: true, date: Date.now(), check: result.check });
    importing = false;
    pending = null;
    // Not awaited: Firefox asks the player, and the answer may never come.
    store.persist().then(updateStorageInfo);
    show('import', false);
    showHome();
  } catch (e) {
    importing = false;
    show('progress', false);
    const quota = e && (e.name === 'QuotaExceededError' || /quota/i.test(e.message));
    showImportError(quota ? t('quota') : t('importFailed', e.message));
    await store.setInstall({ complete: false }).catch(() => {});
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Home: ready to play

async function showReady(install) {
  show('import', false);
  show('ready');
  // A returning player gets Play and little else: the introduction is the landing page's and the first import's.
  show('intro', false);
  const check = install.check;
  const parts = [];
  if (check) {
    parts.push(t('readyMusic', check.music[0], check.music[1]));
    const languages = Object.entries(check.languages).filter(([, has]) => has).map(([name]) => name);
    parts.push(languages.join(', '));
  }
  $('ready-summary').textContent = parts.join(' · ');
  show('ios-tip', isIOS && !standalone);
  $('quality').value = prefs.quality;
  show('touch-settings', isTouchDevice || prefs.touchMode === 'on');
  $('touch-mode').value = prefs.touchMode;
  $('touch-size').value = prefs.touchSize;
  $('touch-opacity').value = prefs.touchOpacity;
  updateStorageInfo();
  $('play').focus({ preventScroll: true });
}

for (const [id, key, parse] of [['quality', 'quality', String], ['touch-mode', 'touchMode', String],
  ['touch-size', 'touchSize', Number], ['touch-opacity', 'touchOpacity', Number]]) {
  $(id).addEventListener('input', () => {
    prefs[key] = parse($(id).value);
    savePrefs();
    game.applyPrefs();
  });
}

async function updateStorageInfo() {
  const estimate = await store.estimate();
  const kept = await store.persisted();
  const parts = [];
  if (estimate && estimate.usage) parts.push(t('storageUsed', formatMB(estimate.usage)));
  parts.push(t(kept ? 'storageKept' : 'storageMayClear'));
  $('storage-info').textContent = parts.join(' ');
}

$('reimport').onclick = () => showImport('');

$('delete-all').onclick = async () => {
  if (!confirm(t('deleteConfirm'))) return;
  await store.deleteAll();
  try {
    localStorage.removeItem('reg2-prefs');
  } catch {}
  $('data-message').textContent = t('deleted');
  show('intro');
  showImport('');
};

$('export-saves').onclick = async () => {
  const { zipSync } = await import('./vendor/fflate.js');
  const paths = await store.userFiles();
  if (!paths.length) {
    $('data-message').textContent = t('noSaves');
    return;
  }
  const files = {};
  for (const p of paths) files[p] = new Uint8Array(await (await store.readFile(p)).arrayBuffer());
  const zip = zipSync(files, { level: 6 });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([zip], { type: 'application/zip' }));
  a.download = 'reG2-saves-' + new Date().toISOString().slice(0, 10) + '.zip';
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  $('data-message').textContent = t('exported', paths.length);
};

$('import-saves').onclick = () => $('saves-input').click();
$('saves-input').onchange = async (e) => {
  const chosen = [...e.target.files];
  e.target.value = '';
  const entries = [];
  for (const file of chosen) {
    if (/\.zip$/i.test(file.name)) {
      const { unzipSync } = await import('./vendor/fflate.js');
      const files = unzipSync(new Uint8Array(await file.arrayBuffer()));
      for (const [path, bytes] of Object.entries(files)) {
        if (!path.endsWith('/') && !store.isData(path) && !path.includes('..')) entries.push([path, bytes]);
      }
    } else {
      entries.push([/\.ini$/i.test(file.name) ? file.name : 'player/' + file.name, new Uint8Array(await file.arrayBuffer())]);
    }
  }
  if (!entries.length) {
    $('data-message').textContent = t('noSavesInFile');
    return;
  }
  await store.writeFiles(entries);
  $('data-message').textContent = t('imported', entries.length);
};

$('play').onclick = () => game.start();

// ---------------------------------------------------------------------------------------------------------------
// The game

const realPixelRatio = window.devicePixelRatio || 1;
// SDL sizes the canvas by devicePixelRatio: the page sets it from the picture setting, so that a phone's 3x
// screen is not drawn at its full resolution unless asked.
// ?scale=0.5: the canvas at that part of its resolution, to compare frame rates.
const urlScale = Math.min(2, Math.max(0.25, +new URLSearchParams(location.search).get('scale') || 1));
function gamePixelRatio() {
  return urlScale * pagePixelRatio();
}
function pagePixelRatio() {
  const dpr = realPixelRatio;
  if (prefs.quality === 'sharp') return dpr;
  if (prefs.quality === 'fast') return Math.min(dpr, 1);
  const cssPixels = innerWidth * innerHeight;
  const budget = isTouchDevice ? 1.3e6 : 2.4e6;
  return Math.max(1, Math.min(dpr, Math.sqrt(budget / cssPixels)));
}
try {
  Object.defineProperty(window, 'devicePixelRatio', { configurable: true, get: () => (game.module ? gamePixelRatio() : realPixelRatio) });
} catch {}

const game = {
  module: null,
  touch: null,
  log: [],
  persistTimer: 0,
  dirty: new Set(),
  wakeLock: null,
  exiting: false,

  logLine(line) {
    this.log.push(line);
    if (this.log.length > 200) this.log.shift();
    console.log(line);
  },

  async start() {
    if (this.module || storageLost) return;
    const root = $('game');
    // Full screen and the orientation lock need the click's user activation.
    if (isTouchDevice) enterFullscreen();
    document.body.classList.add('playing');
    show('page', false);
    show('game');
    show('loading');
    $('loading-bar').removeAttribute('value');
    $('loading-label').textContent = t('loading');
    this.requestWakeLock();
    try {
      const [wasm, files] = await Promise.all([this.fetchWasm(), this.readFiles()]);
      await loadScript(BUILD + 'reg2.js');
      await this.run(wasm, files);
    } catch (e) {
      if (!(await reloadIfBuildGone())) this.crash(t('startFailedTitle'), t('startFailed', e.message || e), e);
    }
  },

  async fetchWasm() {
    const response = await fetch(BUILD + 'reg2.wasm');
    if (!response.ok) throw new Error('reg2.wasm: HTTP ' + response.status);
    const total = +response.headers.get('content-length') || 0;
    const reader = response.body.getReader();
    const chunks = [];
    let done = 0;
    for (;;) {
      const { value, done: end } = await reader.read();
      if (end) break;
      chunks.push(value);
      done += value.length;
      if (total) {
        $('loading-bar').value = done / total;
        $('loading-label').textContent = t('loadingGame', formatMB(done), formatMB(total));
      }
    }
    const bytes = new Uint8Array(done);
    let at = 0;
    for (const c of chunks) {
      bytes.set(c, at);
      at += c.length;
    }
    return bytes.buffer;
  },

  // Everything but the music, the biggest part, which is only streamed: each track is read when it plays.
  async readFiles() {
    const paths = await store.listFiles();
    const now = paths.filter((p) => !store.isMusic(p));
    this.music = paths.filter(store.isMusic);
    const files = [];
    await Promise.all(now.map(async (p) => {
      const blob = await store.readFile(p);
      if (blob) files.push([p, new Uint8Array(await blob.arrayBuffer())]);
    }));
    return files;
  },

  writeToFS(FS, path, bytes) {
    const full = GAME_DIR + '/' + path;
    FS.mkdirTree(full.slice(0, full.lastIndexOf('/')));
    FS.writeFile(full, bytes, { canOwn: true });
  },

  async run(wasmBinary, files) {
    const self = this;
    const Module = {
      canvas: $('canvas'),
      wasmBinary,
      print: (s) => self.logLine(s),
      printErr: (s) => self.logLine(s),
      preRun: [(M) => {
        M.FS.mkdirTree(GAME_DIR + '/player');
        M.FS.mkdirTree(GAME_DIR + '/test');
        for (const [path, bytes] of files) self.writeToFS(M.FS, path, bytes);
        files.length = 0;
        // The tracks as empty files: the game finds its music, and compat/src/miles.cpp asks for a track
        // (port_WebLoadFile) when it opens one that is empty.
        for (const path of self.music) self.writeToFS(M.FS, path, new Uint8Array(0));
        // Phones: at most 2x anti-aliasing (compat/src/video_port.cpp).
        if (isTouchDevice) M.ENV.GTA2_MSAA_CAP = '2';
        // Toggles for this session, to compare frame rates: ?msaa=N caps the anti-aliasing, ?bloom=0 turns bloom off,
        // ?nofx=1 every ENHANCED rendering pass (lighting, shadows, occlusion, fire, bloom, anti-aliasing).
        const params = new URLSearchParams(location.search);
        if (params.get('msaa') !== null) M.ENV.GTA2_MSAA_CAP = params.get('msaa');
        if (params.get('bloom') === '0') M.ENV.GTA2_CALM = '1';
        if (params.get('nofx') === '1') M.ENV.GTA2_NOFX = '1';
        // ?soundlog: the sound starts, those lost and their delay, once a second; =2 also each one (compat/src/miles.cpp).
        if (params.has('soundlog')) M.ENV.GTA2_SOUND_LOG = params.get('soundlog') || '1';
        // ?audioframes=N: SDL's audio buffer hint for this session (web/src/web_platform.cpp).
        if (params.get('audioframes')) M.ENV.GTA2_AUDIO_FRAMES = params.get('audioframes');
        // ?fps=N: the frame limit (0 a frame each refresh).
        if (params.get('fps') !== null) M.ENV.GTA2_MAX_FPS = params.get('fps');
        // ?stats: the frame loop's rates in the console, every 5 seconds.
        if (new URLSearchParams(location.search).has('stats')) M.ENV.GTA2_LOOP_STATS = '1';
      }],
      onAbort: (what) => self.exiting || self.crash(t('crashTitle'), t('crashText'), what),
      onGameQuit: (code) => self.exited(code),
      loadFile: (path) => self.loadTrack(path),
    };
    const M = await window.createReg2(Module);
    this.module = M;
    try {
      sessionStorage.removeItem('reg2-reloaded');
    } catch {}
    this.watchWrites(M.FS);
    this.setupPage(M);
    show('loading', false);
    $('canvas').focus();
    M.callMain([]);
    this.logLine('WebGL: ' + this.glRenderer());
    const params = new URLSearchParams(location.search);
    if (params.has('stats') || params.has('debug')) this.showStats(M);
    if (params.has('debug')) this.debugLog(M);
  },

  // A music track for the game (true if the page has it), read from storage while the stream waits silently.
  // Only the last two tracks stay in memory: one plays at a time, the other may still be fading out.
  loadTrack(path) {
    const rel = path.replace(/^\/?reG2\//, '').replace(/\\/g, '/').toLowerCase();
    const key = (this.music || []).find((p) => p.toLowerCase() === rel);
    if (!key) return false;
    this.tracks ??= [];
    if (this.tracks.includes(key)) return true;
    this.tracks.push(key);
    const FS = this.module.FS;
    while (this.tracks.length > 2) this.writeToFS(FS, this.tracks.shift(), new Uint8Array(0));
    store.readFile(key).then(async (blob) => {
      if (blob && this.module && this.tracks.includes(key)) this.writeToFS(FS, key, new Uint8Array(await blob.arrayBuffer()));
    }).catch((e) => this.logLine('music ' + key + ': ' + e.message));
    return true;
  },

  // The game's writes outside data/ (settings, saves, replays) are stored as soon as the file is closed.
  watchWrites(FS) {
    const self = this;
    const rel = (path) => (path && path.startsWith(GAME_DIR + '/') ? path.slice(GAME_DIR.length + 1) : null);
    const mark = (path) => {
      const r = rel(path);
      if (r && !store.isData(r)) {
        self.dirty.add(r);
        clearTimeout(self.persistTimer);
        self.persistTimer = setTimeout(() => self.flush(), 250);
      }
    };
    const close = FS.close;
    FS.close = function (stream) {
      const writing = (stream.flags & 3) !== 0;
      const path = stream.path;
      const result = close.apply(this, arguments);
      if (writing) mark(path);
      return result;
    };
    const unlink = FS.unlink;
    FS.unlink = function (path) {
      const result = unlink.apply(this, arguments);
      mark(path);
      return result;
    };
    const rename = FS.rename;
    FS.rename = function (from, to) {
      const result = rename.apply(this, arguments);
      mark(from);
      mark(to);
      return result;
    };
  },

  async flush() {
    const started = performance.now();
    clearTimeout(this.persistTimer);
    const M = this.module;
    if (!M || storageLost) return;
    const paths = [...this.dirty];
    this.dirty.clear();
    for (const p of paths) {
      const full = GAME_DIR + '/' + p;
      try {
        if (M.FS.analyzePath(full).exists) await store.writeFile(p, M.FS.readFile(full));
        else await store.deleteFile(p);
      } catch (e) {
        this.logLine('save ' + p + ': ' + e.message);
        const quota = e && (e.name === 'QuotaExceededError' || /quota/i.test(e.message));
        if (quota) this.notice(t('saveQuota'));
      }
    }
    if (this.debug) this.debug.io += performance.now() - started;
  },

  notice(text) {
    this.logLine(text);
  },

  setupPage(M) {
    const canvas = $('canvas');
    canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();
      this.flush();
      this.crash(t('glLostTitle'), t('glLostText'), 'webglcontextlost');
    });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    document.addEventListener('visibilitychange', () => this.onVisibility());
    addEventListener('pagehide', () => this.flush());
    addEventListener('beforeunload', (e) => {
      this.flush();
      // Leaving a game loses what was done since the last save; the menus have nothing to lose.
      if (!this.exiting && this.state() & 2) e.preventDefault();
    });
    addEventListener('keydown', (e) => this.onKey(e), true);
    addEventListener('resize', () => this.checkOrientation());
    screen.orientation?.addEventListener?.('change', () => this.checkOrientation());
    document.addEventListener('fullscreenchange', () => this.onFullscreen());
    $('fs-button').onclick = () => toggleFullscreen();
    $('pause-button').onclick = () => this.pause();
    // The browser takes the first Escape to leave pointer lock (or full screen) and the game never sees it: losing
    // either during play counts as the player's Escape.
    document.addEventListener('pointerlockchange', () => {
      if (!document.pointerLockElement && lockWanted) this.pauseForEscape();
    });
    // The full screen button shows while the mouse moves, then fades: it sits over the HUD's corner.
    let idleTimer = 0;
    addEventListener('pointermove', () => {
      $('game-bar').classList.remove('idle');
      clearTimeout(idleTimer);
      idleTimer = setTimeout(() => $('game-bar').classList.add('idle'), 2500);
    });
    idleTimer = setTimeout(() => $('game-bar').classList.add('idle'), 4000);
    this.lockedPointing(M);
    if (!document.fullscreenEnabled && !document.webkitFullscreenEnabled) document.body.classList.add('no-fullscreen');
    $('crash-reload').onclick = () => location.reload();
    $('crash-copy').onclick = () => this.copyDiagnostic();
    this.touch = new TouchControls($('touch'), M, () => this.state(), prefs);
    this.applyPrefs();
    this.checkOrientation();
  },

  // With the pointer locked (TWIN STICK's mouse aim) the page's pointer is gone: the game's cursor points at the
  // pause and full screen buttons instead, and a click on one goes to the button, not to the game.
  lockedPointing(M) {
    const canvas = $('canvas');
    const buttons = [$('pause-button'), $('fs-button')];
    let aimed = null;
    let swallowUp = false;
    const under = () => {
      if (document.pointerLockElement !== canvas || $('game-bar').classList.contains('idle')) return null;
      const c = canvas.getBoundingClientRect();
      const x = c.left + M._port_WebCursorX();
      const y = c.top + M._port_WebCursorY();
      return buttons.find((b) => {
        const r = b.getBoundingClientRect();
        return r.width && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
      }) || null;
    };
    const aim = (b) => {
      if (b === aimed) return;
      aimed?.classList.remove('aimed');
      b?.classList.add('aimed');
      aimed = b;
    };
    addEventListener('pointermove', () => aim(under()));
    document.addEventListener('pointerlockchange', () => aim(null));
    addEventListener('pointerdown', (e) => {
      const b = e.button === 0 ? under() : null;
      if (!b) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      swallowUp = true;
      b.click();
    }, true);
    addEventListener('pointerup', (e) => {
      if (!swallowUp) return;
      swallowUp = false;
      e.stopImmediatePropagation();
    }, true);
  },

  // The graphics driver as the browser names it (unmasked where the browser allows), for the diagnostics. Read
  // once: the query waits for the GPU to finish everything queued, tens of milliseconds in a game.
  glRenderer() {
    if (this.renderer) return this.renderer;
    try {
      const gl = $('canvas').getContext('webgl2');
      const info = gl.getExtension('WEBGL_debug_renderer_info');
      this.renderer = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    } catch {
      this.renderer = '?';
    }
    return this.renderer;
  },

  // ?stats: frames per second, the game code's time per frame, the display's period, the canvas and memory, on
  // the screen every second (and in the console every 5 s), for players to report.
  showStats(M) {
    const box = $('stats');
    show('stats');
    // The GPU's time for each browser frame (EXT_disjoint_timer_query_webgl2, where the browser has it), and the
    // frames that came late (more than 1.5 display periods after the previous one).
    const gpu = [];
    let late = 0;
    const gl = $('canvas').getContext('webgl2');
    // Only with ?gpu=1: a query every frame may change how the browser paces them.
    const timer = gl && new URLSearchParams(location.search).has('gpu') && gl.getExtension('EXT_disjoint_timer_query_webgl2');
    let query = null;
    const pending = [];
    let lastFrame = 0;
    let browserFrames = 0;
    this.browserFrames = () => browserFrames;
    const frame = (t) => {
      browserFrames++;
      if (lastFrame && t - lastFrame > 1.5 * (M.webFrames?.period || 16.7)) late++;
      lastFrame = t;
      if (timer && !gl.isContextLost()) {
        if (query) {
          gl.endQuery(timer.TIME_ELAPSED_EXT);
          pending.push(query);
        }
        while (pending.length && gl.getQueryParameter(pending[0], gl.QUERY_RESULT_AVAILABLE)) {
          const q = pending.shift();
          gpu.push(gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6);
          gl.deleteQuery(q);
        }
        query = gl.createQuery();
        gl.beginQuery(timer.TIME_ELAPSED_EXT, query);
      }
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
    let lastPresents = 0;
    let lastBrowser = 0;
    let lastTime = performance.now();
    let tick = 0;
    setInterval(() => {
      const f = M.webFrames;
      if (!f) return;
      const now = performance.now();
      const fps = ((f.presents - lastPresents) * 1000) / (now - lastTime);
      const browserFps = ((this.browserFrames() - lastBrowser) * 1000) / (now - lastTime);
      lastBrowser = this.browserFrames();
      const busy = f.frames ? f.busy / f.frames : 0;
      lastPresents = f.presents;
      lastTime = now;
      f.busy = f.frames = 0;
      const canvas = $('canvas');
      const gpuMs = gpu.length ? gpu.reduce((a, b) => a + b, 0) / gpu.length : NaN;
      gpu.length = 0;
      box.textContent = `${fps.toFixed(0)} fps drawn of ${browserFps.toFixed(0)} browser frames  game ${busy.toFixed(1)} ms  ` +
        `gpu ${isNaN(gpuMs) ? '?' : gpuMs.toFixed(1)} ms  late ${late}  screen ${(1000 / f.period).toFixed(0)} Hz\n` +
        `canvas ${canvas.width}x${canvas.height} (x${devicePixelRatio.toFixed(2)})  wasm ${M.HEAPU8.length >> 20} MB\n${this.glRenderer()}`;
      late = 0;
      if (++tick % 5 === 0) console.log('web: ' + box.textContent.replace(/\n/g, ' | '));
    }, 1000);
  },

  // ?debug: one compact console line a second about the page's thread, for players to send (DevTools console,
  // Save as): the browser's frame intervals, the time in the game, in Asyncify, in sound, in storage, long tasks,
  // and what happened (pointer lock, full screen, visibility, resizes, the audio context's state).
  debugLog(M) {
    const d = { raf: [], audio: 0, audioCalls: 0, audioLate: 0, audioSlack: Infinity, io: 0, long: [], events: [], gl: 0, draws: 0, fbos: 0, uploads: 0 };
    this.debug = d;
    // WebGL calls a second: all of them, draws, framebuffer binds, bytes of buffer and texture uploads.
    const gl = $('canvas').getContext('webgl2');
    const draws = new Set(['drawArrays', 'drawElements', 'drawArraysInstanced', 'drawElementsInstanced', 'drawRangeElements']);
    const bytes = (a) => (a && a.byteLength) || 0;
    for (const name of Object.getOwnPropertyNames(WebGL2RenderingContext.prototype)) {
      const fn = gl && gl[name];
      if (typeof fn !== 'function' || name === 'constructor') continue;
      gl[name] = function (...args) {
        d.gl++;
        if (draws.has(name)) d.draws++;
        else if (name === 'bindFramebuffer') d.fbos++;
        else if (name === 'bufferData' || name === 'bufferSubData') {
          // (target, data or offset, ...): Emscripten passes the heap with an offset and a length.
          const src = name === 'bufferData' ? args[1] : args[2];
          const length = name === 'bufferData' ? args[4] : args[4];
          if (typeof src === 'object' && src) d.uploads += length !== undefined ? length * (src.BYTES_PER_ELEMENT || 1) : bytes(src);
        } else if (name === 'texImage2D' && args.length >= 9 && args[8]) d.uploads += args[3] * args[4] * 4;
        else if (name === 'texSubImage2D' && args.length >= 9 && args[8]) d.uploads += args[4] * args[5] * 4;
        return fn.apply(gl, args);
      };
    }
    let last = 0;
    const frame = (t) => {
      if (last) d.raf.push(t - last);
      last = t;
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
    const event = (name) => d.events.push(name);
    document.addEventListener('pointerlockchange', () => event(document.pointerLockElement ? 'lock' : 'unlock'));
    document.addEventListener('fullscreenchange', () => event(document.fullscreenElement ? 'fullscreen' : 'window'));
    document.addEventListener('visibilitychange', () => event(document.hidden ? 'hidden' : 'visible'));
    addEventListener('resize', () => event(`resize ${innerWidth}x${innerHeight}`));
    try {
      new PerformanceObserver((list) => {
        for (const e of list.getEntries()) {
          const a = e.attribution && e.attribution[0];
          d.long.push(`${Math.round(e.duration)}ms@${Math.round(e.startTime / 1000)}s${a ? ' ' + (a.containerType || a.name) : ''}`);
        }
      }).observe({ type: 'longtask', buffered: false });
    } catch {}
    // The sound's callback, timed once SDL has made it.
    const wrapAudio = () => {
      const sdl = M.SDL3;
      const node = sdl?.audio_playback?.scriptProcessorNode;
      if (!node || !node.onaudioprocess) return setTimeout(wrapAudio, 500);
      const inner = node.onaudioprocess;
      event(`audio buffer ${node.bufferSize} frames at ${sdl.audioContext.sampleRate} Hz`);
      node.onaudioprocess = (e) => {
        // How far ahead of the speakers this buffer is filled; behind them, it plays as a gap.
        const slack = e.playbackTime - sdl.audioContext.currentTime;
        d.audioSlack = Math.min(d.audioSlack, slack * 1000);
        if (slack < 0) d.audioLate++;
        const t = performance.now();
        inner(e);
        d.audio += performance.now() - t;
        d.audioCalls++;
      };
      sdl.audioContext.addEventListener('statechange', () => event('audio ' + sdl.audioContext.state));
      event('audio ' + sdl.audioContext.state);
    };
    wrapAudio();
    let tick = 0;
    let lastCallback = 0;
    let lastBusy = 0;
    let lastFrames = 0;
    setInterval(() => {
      const f = M.webFrames || {};
      const r = d.raf.splice(0);
      const sum = r.reduce((a, b) => a + b, 0);
      const fmt = (v) => v.toFixed(1);
      const frames = (f.yields || 0) - lastFrames;
      const callback = (f.callback || 0) - lastCallback;
      const busy = (f.busyTotal || 0) - lastBusy;
      lastFrames = f.yields || 0;
      lastCallback = f.callback || 0;
      lastBusy = f.busyTotal || 0;
      const known = callback + d.audio + d.io;
      console.log(`[reg2 debug] t=${++tick}s raf n=${r.length} min/avg/max ${r.length ? fmt(Math.min(...r)) : '-'}/` +
        `${r.length ? fmt(sum / r.length) : '-'}/${r.length ? fmt(Math.max(...r)) : '-'} >20ms:${r.filter((x) => x > 20).length} ` +
        `>33ms:${r.filter((x) => x > 33).length} | game ${fmt(busy)}ms in ${frames} yields, asyncify+callback ` +
        `${fmt(callback - busy)}ms | audio ${fmt(d.audio)}ms in ${d.audioCalls} calls, late ${d.audioLate}, slack min ` +
        `${d.audioCalls ? fmt(d.audioSlack) : '-'}ms | storage (wall) ${fmt(d.io)}ms | ` +
        `page thread busy (known) ${fmt(known)}ms of 1000 | state ${this.state()} | gl calls ${d.gl} draws ${d.draws} ` +
        `fbo binds ${d.fbos} uploads ${(d.uploads / 1048576).toFixed(1)} MB | long tasks: ${d.long.join(', ') || '-'} | ` +
        `events: ${d.events.join(', ') || '-'}`);
      d.audio = d.audioCalls = d.audioLate = d.io = d.gl = d.draws = d.fbos = d.uploads = 0;
      d.audioSlack = Infinity;
      d.long.length = 0;
      d.events.length = 0;
    }, 1000);
  },

  // The game's pause menu, in a game and not already open.
  pause() {
    const state = this.state();
    if (!(state & 2) || state & 4) return;
    this.pausedAt = performance.now();
    try {
      this.module._port_WebPause();
    } catch {}
  },

  // The browser took an Escape to leave pointer lock or full screen: the pause menu, once. A browser may also deliver
  // that Escape, before (the game opens the menu itself) or after (onKey drops it).
  pauseForEscape() {
    if (performance.now() - (this.escapeAt || 0) < 400) return;
    this.pause();
  },

  // A short message over the game, shown once per page load for each text.
  toast(text) {
    this.toasts ??= new Set();
    if (this.toasts.has(text)) return;
    this.toasts.add(text);
    const el = $('toast');
    el.textContent = text;
    show('toast');
    setTimeout(() => show('toast', false), 4000);
  },

  state() {
    try {
      return this.module ? this.module._port_WebState() : 0;
    } catch {
      return 0;
    }
  },

  applyPrefs() {
    this.touch?.setPrefs(prefs);
    dispatchEvent(new Event('resize'));
  },

  audioContext() {
    return this.module?.SDL3?.audioContext;
  },

  onVisibility() {
    if (document.hidden) {
      this.pause();
      this.audioContext()?.suspend().catch(() => {});
      this.flush();
      this.releaseWakeLock();
    } else {
      this.audioContext()?.resume().catch(() => {});
      this.requestWakeLock();
    }
  },

  onKey(e) {
    if (!this.module) return;
    // A browser that also delivers the Escape that left pointer lock or full screen: the pause menu just opened
    // for it, and the game would close it again.
    if (e.key === 'Escape') {
      if (performance.now() - (this.pausedAt || 0) < 400) {
        e.preventDefault();
        e.stopImmediatePropagation();
        return;
      }
      this.escapeAt = performance.now();
    }
    // Alt+Enter: full screen, as the desktop builds.
    if (e.key === 'Enter' && e.altKey) {
      e.preventDefault();
      e.stopPropagation();
      if (e.type === 'keydown') toggleFullscreen();
      return;
    }
    // The game's function keys (F1 to F10), not the browser's help, search, reload, address bar or caret mode.
    if (/^F([1-9]|10)$/.test(e.key) || e.key === 'Backspace' || e.key === 'Tab') e.preventDefault();
  },

  onFullscreen() {
    const on = !!document.fullscreenElement;
    document.body.classList.toggle('fullscreen', on);
    // In full screen, Escape goes to the game (the pause menu); holding it leaves full screen (Chromium's Keyboard
    // Lock). Without it, the Escape that left full screen opens the pause menu.
    if (on) {
      navigator.keyboard?.lock?.(['Escape']).then(() => this.toast(t('holdEscape'))).catch(() => {});
    } else {
      this.pauseForEscape();
    }
    if (on && isTouchDevice) screen.orientation?.lock?.('landscape').catch(() => {});
    this.checkOrientation();
  },

  checkOrientation() {
    const portrait = isTouchDevice && innerHeight > innerWidth;
    if (portrait && $('rotate').hidden) this.pause();
    show('rotate', portrait);
  },

  async requestWakeLock() {
    try {
      if (!this.wakeLock && navigator.wakeLock && !document.hidden) {
        this.wakeLock = await navigator.wakeLock.request('screen');
        this.wakeLock.addEventListener('release', () => (this.wakeLock = null));
      }
    } catch {}
  },

  releaseWakeLock() {
    this.wakeLock?.release().catch(() => {});
    this.wakeLock = null;
  },

  async exited(code) {
    this.exiting = true;
    await this.flush();
    location.reload();
  },

  stop(title, text) {
    if (!this.module) return;
    this.crash(title, text, 'stopped');
    try {
      this.module.pauseMainLoop?.();
    } catch {}
  },

  diagnostic: '',
  crash(title, text, detail) {
    if (this.crashed) return;
    this.crashed = true;
    this.flush();
    this.audioContext()?.suspend().catch(() => {});
    show('loading', false);
    show('game');
    show('page', false);
    $('crash-title').textContent = title;
    $('crash-text').textContent = text;
    show('crash');
    this.diagnostic = [
      'reG2 web ' + (window.REG2_VERSION || ''),
      navigator.userAgent,
      'error: ' + String(detail && detail.stack || detail || ''),
      'wasm memory: ' + (this.module?.HEAPU8 ? (this.module.HEAPU8.length >> 20) + ' MB' : '?'),
      'WebGL: ' + this.glRenderer(),
      '--- log',
      ...this.log.slice(-60),
    ].join('\n');
    $('crash-reload').focus();
  },

  async copyDiagnostic() {
    try {
      await navigator.clipboard.writeText(this.diagnostic);
      $('crash-copy').textContent = t('copied');
    } catch {
      prompt(t('copyDiag'), this.diagnostic);
    }
  },
};

// For tests and the console.
window.reg2 = game;

// Only the game's own failures stop it: a trap in its code (aborts come through onAbort) or an error in reg2.js.
// Anything else, such as a browser API refusing a request, is logged. After the game quit, the runtime's unwinding
// may trap: the page is already on its way back.
function isGameFailure(error, file) {
  return error instanceof WebAssembly.RuntimeError || /\/reg2\.js$/.test(file || '');
}
addEventListener('error', (e) => {
  if (!game.module || game.exiting) return;
  if (isGameFailure(e.error, e.filename)) game.crash(t('crashTitle'), t('crashText'), e.error || e.message);
  else game.logLine('page error: ' + (e.message || e.error));
});
addEventListener('unhandledrejection', (e) => {
  if (!game.module || game.exiting) return;
  if (isGameFailure(e.reason)) game.crash(t('crashTitle'), t('crashText'), e.reason);
  else game.logLine('unhandled: ' + (e.reason && e.reason.message || e.reason));
});

// SDL asks for pointer lock when TWIN STICK aims with the mouse. The browser may refuse (just after the player left
// it with Escape, or outside a click): the refusal is not an error, and the next click on the game asks again.
let lockWanted = null;
const requestLock = Element.prototype.requestPointerLock;
if (requestLock) {
  Element.prototype.requestPointerLock = function (...args) {
    // Touch controls in use: a phone's browser may lock the pointer, and then every touch goes to the canvas.
    if (document.body.classList.contains('touch-on')) return;
    lockWanted = this;
    try {
      const result = requestLock.apply(this, args);
      result?.catch?.((e) => game.logLine('pointer lock: ' + e.message));
      return result;
    } catch (e) {
      game.logLine('pointer lock: ' + e.message);
    }
  };
  const exitLock = document.exitPointerLock;
  document.exitPointerLock = function (...args) {
    lockWanted = null;
    return exitLock.apply(this, args);
  };
  addEventListener('pointerdown', (e) => {
    if (lockWanted && !document.pointerLockElement && e.target === lockWanted) lockWanted.requestPointerLock();
  }, true);
}

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = resolve;
    s.onerror = () => reject(new Error(src));
    document.head.appendChild(s);
  });
}

function enterFullscreen() {
  const el = $('game');
  const request = el.requestFullscreen || el.webkitRequestFullscreen;
  if (request && !document.fullscreenElement) {
    try {
      const p = request.call(el, { navigationUI: 'hide' });
      p?.catch?.(() => {});
    } catch {}
  }
}

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  else enterFullscreen();
}
