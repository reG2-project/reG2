// Reads the player's GTA2 with the native setup's code (port/setup/install.cpp, compiled to reg2_import.wasm),
// off the page's thread. The source is mounted read-only from the chosen files (WORKERFS), the result is written
// to /out and sent back file by file.
//
// In:  {cmd: 'check' | 'install', entries: [{path, file}], root, installed: [paths]}
// Out: {type: 'progress', done, total} | {type: 'file', path, data} | {type: 'result', result, fullInstall, times}
//      | {type: 'error', message}
// times: milliseconds per phase (reading the source, writing and reading the worker's memory files, the rest of the
// unpacking, sending the files), which the page logs with ?debug.
'use strict';
importScripts('reg2_import.js');

let modulePromise = null;

function module() {
  modulePromise ??= createReg2Import({ print: () => {}, printErr: (s) => console.warn('import:', s) }).then(tune);
  return modulePromise;
}

// Emscripten's file systems as written for small files, tuned for this one's hundreds of MB:
// - a memory file past 1 MB grows by an eighth, copying all of it each time (2.7 GB copied for the installer's
//   300 MB of cabinets): it doubles instead;
// - a read of the chosen file is one FileReaderSync call of the size asked (a few KB from the unpackers): it reads
//   4 MB ahead and serves the next reads from that.
function tune(M) {
  const MEMFS = M.FS.lookupPath('/').node.mount.type;
  const expand = MEMFS.expandFileStorage;
  MEMFS.expandFileStorage = function (node, capacity) {
    const previous = node.contents ? node.contents.length : 0;
    return expand.call(this, node, previous < capacity ? Math.max(capacity, previous * 2) : capacity);
  };
  const AHEAD = 4 << 20;
  const ops = M.WORKERFS.stream_ops;
  ops.read = function (stream, buffer, offset, length, position) {
    const node = stream.node;
    if (position >= node.size) return 0;
    let cache = node.readAhead;
    if (!cache || position < cache.start || position + Math.min(length, node.size - position) > cache.start + cache.bytes.length) {
      const blob = node.contents.slice(position, position + Math.max(length, AHEAD));
      cache = node.readAhead = { start: position, bytes: new Uint8Array(M.WORKERFS.reader.readAsArrayBuffer(blob)) };
    }
    const n = Math.min(length, cache.start + cache.bytes.length - position);
    buffer.set(cache.bytes.subarray(position - cache.start, position - cache.start + n), offset);
    return n;
  };
  return M;
}

function removeTree(FS, path) {
  if (!FS.analyzePath(path).exists) return;
  for (const name of FS.readdir(path)) {
    if (name === '.' || name === '..') continue;
    const p = path + '/' + name;
    if (FS.isDir(FS.stat(p).mode)) removeTree(FS, p);
    else FS.unlink(p);
  }
  FS.rmdir(path);
}

function walk(FS, dir, base, out) {
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const p = dir + '/' + name;
    const rel = base ? base + '/' + name : name;
    if (FS.isDir(FS.stat(p).mode)) walk(FS, p, rel, out);
    else out.push(rel);
  }
  return out;
}

onmessage = async ({ data }) => {
  try {
    const M = await module();
    const FS = M.FS;
    try { FS.unmount('/src'); } catch {}
    removeTree(FS, '/src');
    removeTree(FS, '/out');
    FS.mkdir('/src');
    FS.mkdir('/out');
    FS.mount(M.WORKERFS, { blobs: data.entries.map((e) => ({ name: e.path, data: e.file })) }, '/src');
    // The installed game as empty files: the setup then sees it, to add the music or the movie to it.
    for (const path of data.installed || []) {
      const p = '/out/' + path;
      FS.mkdirTree(p.slice(0, p.lastIndexOf('/')));
      FS.writeFile(p, new Uint8Array(0));
    }
    FS.chdir('/out');
    const source = '/src/' + data.root;

    if (data.cmd === 'check') {
      const result = JSON.parse(M.ccall('import_check', 'string', ['string'], [source]));
      postMessage({ type: 'result', result });
      return;
    }

    const times = { sourceRead: 0, sourceReads: 0, memWrite: 0, memWrites: 0, memRead: 0, memReads: 0, unpack: 0, send: 0 };
    let last = 0;
    const write = FS.write;
    const read = FS.read;
    FS.write = function (...args) {
      const t0 = performance.now();
      const n = write.apply(this, args);
      const now = performance.now();
      times.memWrite += now - t0;
      times.memWrites++;
      if (now - last > 250) {
        last = now;
        postMessage({ type: 'progress', done: M._import_done(), total: M._import_total() });
      }
      return n;
    };
    FS.read = function (stream, ...args) {
      const t0 = performance.now();
      const n = read.call(this, stream, ...args);
      const dt = performance.now() - t0;
      if (stream.path.startsWith('/src/')) {
        times.sourceRead += dt;
        times.sourceReads++;
      } else {
        times.memRead += dt;
        times.memReads++;
      }
      return n;
    };
    const t0 = performance.now();
    const result = JSON.parse(M.ccall('import_install', 'string', ['string'], [source]));
    times.unpack = performance.now() - t0 - times.sourceRead - times.memWrite - times.memRead;
    FS.write = write;
    FS.read = read;
    let fullInstall = false;
    const t1 = performance.now();
    if (result.ok) {
      const files = walk(FS, '/out', '', []);
      const total = files.reduce((sum, f) => sum + FS.stat('/out/' + f).size, 0);
      let done = 0;
      for (const path of files) {
        const bytes = FS.readFile('/out/' + path);
        FS.unlink('/out/' + path);
        if (!bytes.length) continue;
        fullInstall ||= path.toLowerCase() === 'data/fstyle.sty';
        done += bytes.length;
        postMessage({ type: 'file', path, data: bytes.buffer, done, total }, [bytes.buffer]);
      }
    }
    times.send = performance.now() - t1;
    for (const k in times) times[k] = Math.round(times[k]);
    postMessage({ type: 'result', result, fullInstall, times });
  } catch (e) {
    postMessage({ type: 'error', message: String(e && e.message || e) });
  }
};
