// The game's files in the browser: IndexedDB through idb-keyval, one entry per file, keyed by its path in the
// game folder ("data/fstyle.sty", "player/plyslot0.svg", "gta2_registry.ini"), as a Blob.
// The record 'install' in the meta store says whether the game data is complete.
import { createStore, get, set, del, keys, setMany, delMany, clear } from './vendor/idb-keyval.js';

const files = createStore('reg2-files', 'files');
const meta = createStore('reg2-meta', 'meta');

export const isData = (path) => /^data\//i.test(path);
export const isMusic = (path) => /^data\/gtaudio\//i.test(path);

// False when the browser gives the page no storage (some private modes, storage turned off).
export async function available() {
  try {
    await set('probe', Date.now(), meta);
    await del('probe', meta);
    return true;
  } catch {
    return false;
  }
}

export const getInstall = () => get('install', meta);
export const setInstall = (record) => set('install', record, meta);
export const listFiles = () => keys(files);
export const readFile = (path) => get(path, files);
export const writeFile = (path, bytes) => set(path, bytes instanceof Blob ? bytes : new Blob([bytes]), files);
export const deleteFile = (path) => del(path, files);
export const deleteFiles = (paths) => delMany(paths, files);
export const writeFiles = (entries) => setMany(entries.map(([p, b]) => [p, b instanceof Blob ? b : new Blob([b])]), files);

export async function deleteAll() {
  await clear(files);
  await clear(meta);
}

// The player's own files: everything outside data/.
export async function userFiles() {
  return (await listFiles()).filter((p) => !isData(p));
}

export async function estimate() {
  try {
    return await navigator.storage.estimate();
  } catch {
    return null;
  }
}

export async function persisted() {
  try {
    return await navigator.storage.persisted();
  } catch {
    return false;
  }
}

export async function persist() {
  try {
    return await navigator.storage.persist();
  } catch {
    return false;
  }
}
