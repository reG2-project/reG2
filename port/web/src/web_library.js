// The game's calls into the page (web_platform.cpp).
addToLibrary({
  // period: the display's frame period, the median of the last 31 intervals that look like one refresh.
  // busy: milliseconds the game ran between yields, summed until the page reads it (?stats in app.js).
  $webFrames: { last: 0, deltas: [], period: 1000 / 60, resumed: 0, busy: 0, frames: 0, presents: 0 },
  port_WebWaitFrame__async: true,
  port_WebWaitFrame__deps: ['$Asyncify', '$webFrames'],
  port_WebWaitFrame: () => Asyncify.handleSleep((wakeUp) => {
    const f = webFrames;
    Module['webFrames'] = f;
    if (f.resumed) {
      const busy = performance.now() - f.resumed;
      f.busy += busy;
      f.busyTotal = (f.busyTotal || 0) + busy;
      f.frames++;
      f.yields = (f.yields || 0) + 1;
    }
    requestAnimationFrame((t) => {
      const d = t - f.last;
      f.last = t;
      if (d > 2 && d < 50) {
        f.deltas.push(d);
        if (f.deltas.length > 31) f.deltas.shift();
        f.period = [...f.deltas].sort((a, b) => a - b)[f.deltas.length >> 1];
      }
      f.resumed = performance.now();
      wakeUp();
      // The whole callback: Asyncify's rewind, the game until its next yield, the unwind.
      f.callback = (f.callback || 0) + performance.now() - f.resumed;
    });
  }),
  // fsync: nothing to wait for in the memory file system (the page stores files as they are closed). Emscripten's
  // own may wait for a sync, which with Asyncify would need every caller's stack instrumented.
  fd_sync__async: false,
  fd_sync: (fd) => 0,
  // The game quit (QUIT, SAVE AND QUIT): the page's onGameQuit (app.js).
  port_WebQuit: (code) => {
    Module['onGameQuit']?.(code);
  },
  port_WebLoadFile__deps: ['$UTF8ToString'],
  port_WebLoadFile: (path) => (Module['loadFile']?.(UTF8ToString(path)) ? 1 : 0),
  port_WebCountPresent__deps: ['$webFrames'],
  port_WebCountPresent: () => {
    webFrames.presents++;
  },
  port_WebFramePeriodMs__deps: ['$webFrames'],
  port_WebFramePeriodMs: () => webFrames.period,
});
