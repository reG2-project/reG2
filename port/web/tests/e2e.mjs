// End-to-end tests of the web build with Playwright, against a served site (build.sh's output) and the player's
// own GTA2 (never part of the repository). Screenshots go to $OUT, outside the repository: they show the game.
//
//   npm i playwright && npx playwright install chromium firefox
//   python3 -m http.server -d <site> 8000 &
//   SITE=http://127.0.0.1:8000/ GTA2=~/GTA2INSTALLER.exe OUT=/tmp/shots node port/web/tests/e2e.mjs [chromium|firefox] [scenario...]
// The update scenario also needs SITE_DIR=<site> (it serves a copy and changes the build in it).
//
// Scenarios: import, play, save, exit, contextloss, offline, tabs, hide, keys, escape, resize, update, touch. Each runs in its own new profile (import first).
import { chromium, firefox } from 'playwright';
import { cpSync, createReadStream, mkdtempSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join } from 'node:path';

const SITE = process.env.SITE || 'http://127.0.0.1:8000/';
const GTA2 = process.env.GTA2;
const OUT = process.env.OUT || tmpdir();
const [which = 'chromium', ...wanted] = process.argv.slice(2);
const engine = which === 'firefox' ? firefox : chromium;

const failures = [];
function check(name, ok, detail = '') {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
  if (!ok) failures.push(name);
}

async function open(mobile) {
  const dir = mkdtempSync(join(tmpdir(), 'reg2-e2e-'));
  // Silent: the game's sound would reach the speakers otherwise.
  const options = { headless: true, locale: 'en-US', viewport: { width: 1280, height: 720 }, args: ['--mute-audio'],
    firefoxUserPrefs: { 'media.volume_scale': '0.0' } };
  if (which === 'firefox') delete options.args;
  // GPU=vulkan (or gl): Chromium on the machine's GPU instead of SwiftShader.
  if (which === 'chromium' && process.env.GPU) options.args.push('--use-angle=' + process.env.GPU, '--enable-gpu', '--ignore-gpu-blocklist');
  if (mobile) {
    Object.assign(options, { viewport: { width: 844, height: 390 }, deviceScaleFactor: 3, hasTouch: true, isMobile: which === 'chromium' });
  }
  const ctx = await engine.launchPersistentContext(dir, options);
  const page = ctx.pages()[0] || (await ctx.newPage());
  page.on('pageerror', (e) => console.log('  page error:', e.message));
  if (process.env.VERBOSE) page.on('console', (m) => console.log('  console:', m.text().slice(0, 300)));
  page.on('dialog', (d) => d.accept());
  return { ctx, page, close: async () => { await ctx.close(); rmSync(dir, { recursive: true, force: true }); } };
}

// A key held as long as a person holds it: the game reads the keyboard's state once per logic step.
async function press(page, key) {
  await page.keyboard.down(key);
  await page.waitForTimeout(120);
  await page.keyboard.up(key);
}

const hidden = (page, id) => page.evaluate((i) => document.getElementById(i).hidden, id);
const state = (page) => page.evaluate(() => window.reg2.state());
const shot = (page, name) => page.screenshot({ path: join(OUT, `${which}-${name}.png`) });

async function importGame(page) {
  await page.goto(SITE);
  const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click('#pick-file')]);
  await chooser.setFiles(GTA2);
  await page.waitForFunction(() => !document.getElementById('report').hidden || document.getElementById('import-error').textContent);
  const report = await page.textContent('#report-list');
  check('import: report', !(await hidden(page, 'report')), report);
  const start = Date.now();
  await page.click('#do-import');
  await page.waitForFunction(() => !document.getElementById('ready').hidden, null, { timeout: 900000 });
  check('import: stored', true, `${((Date.now() - start) / 1000).toFixed(1)} s`);
}

// From the page to a game in Downtown, with the keyboard: skip the intro, the style (first start), PLAY, START.
async function startGame(page, firstStart) {
  await page.click('#play');
  await page.waitForFunction(() => window.reg2 && window.reg2.module && window.reg2.state() & 1, null, { timeout: 60000 });
  await page.waitForTimeout(3000);
  const keys = firstStart ? 4 : 3;
  for (let i = 0; i < keys; i++) {
    await press(page, 'Enter');
    await page.waitForTimeout(2500);
    if (process.env.STEPS) await shot(page, 'start-' + i);
  }
  await page.waitForFunction(() => window.reg2.state() & 2, null, { timeout: 60000 });
  await page.waitForTimeout(3000);
}

// The pause menu's QUIT GAME, then option n of its question (0 SAVE AND QUIT, 1 QUIT): back in the menus.
async function quitGame(page, option) {
  await press(page, 'Escape');
  await page.waitForTimeout(1000);
  await press(page, 'ArrowUp');
  await press(page, 'Enter');
  await page.waitForTimeout(800);
  for (let i = 0; i < option; i++) await press(page, 'ArrowDown');
  await press(page, 'Enter');
  await page.waitForFunction(() => window.reg2.state() === 1, null, { timeout: 30000 }).catch(() => {});
  await page.waitForTimeout(1500);
}

async function hold(page, key, ms) {
  await page.keyboard.down(key);
  await page.waitForTimeout(ms);
  await page.keyboard.up(key);
}

const scenarios = {
  async import() {
    const { page, close } = await open();
    await importGame(page);
    await page.reload();
    await page.waitForTimeout(2000);
    check('import: ready after reload', !(await hidden(page, 'ready')));
    await close();
  },

  async play() {
    const { page, close } = await open();
    await importGame(page);
    await startGame(page, true);
    check('play: in a game', (await state(page)) & 2);
    await hold(page, 'ArrowUp', 2000);
    await shot(page, 'play');
    await close();
  },

  // SAVE AND QUIT in the pause menu, a reload, then LOAD GAME.
  // The pause menu's QUIT GAME, then SAVE AND QUIT (first) or QUIT (second, without saving).
  async save() {
    const { page, close } = await open();
    await importGame(page);
    await startGame(page, true);
    await hold(page, 'ArrowLeft', 1500);
    await quitGame(page, 0);
    check('save: back in the menus', (await state(page)) === 1);
    check('save: no failure screen', await hidden(page, 'crash'));
    await page.waitForTimeout(1000);
    const files = await page.evaluate(() => import('./storage.js').then((s) => s.userFiles()));
    check('save: autosave stored', files.some((f) => /_auto_/.test(f)), files.join(' '));
    // LOAD GAME, where the PLAY page opens with a save: the autosave, without leaving the page.
    for (const key of ['Enter', 'Enter', 'Enter']) {
      await press(page, key);
      await page.waitForTimeout(key === 'Enter' ? 2000 : 400);
    }
    await page.waitForFunction(() => window.reg2.state() & 2, null, { timeout: 60000 });
    check('save: loaded from the menus', true);
    // QUIT without saving, then a new game.
    await page.waitForTimeout(2000);
    await quitGame(page, 1);
    check('quit: back in the menus', (await state(page)) === 1);
    for (const key of ['Enter', 'ArrowDown', 'ArrowDown', 'Enter', 'ArrowUp', 'Enter']) {
      await press(page, key);
      await page.waitForTimeout(key === 'Enter' || key === 'ArrowUp' ? 2000 : 400);
    }
    await page.waitForFunction(() => window.reg2.state() & 2, null, { timeout: 60000 });
    check('quit: another game started', true);
    // After a reload, LOAD GAME again.
    await page.goto(SITE);
    await page.click('#play');
    await page.waitForFunction(() => window.reg2 && window.reg2.module && window.reg2.state() & 1);
    await page.waitForTimeout(3000);
    for (const key of ['Enter', 'Enter', 'Enter', 'Enter']) {
      await press(page, key);
      await page.waitForTimeout(key === 'Enter' ? 2000 : 400);
    }
    await page.waitForFunction(() => window.reg2.state() & 2, null, { timeout: 60000 });
    check('save: loaded after reload', true);
    // The pause menu's LOAD GAME: warned, then the save.
    await press(page, 'Escape');
    await page.waitForTimeout(1000);
    for (const key of ['ArrowDown', 'ArrowDown', 'ArrowDown', 'Enter']) await press(page, key);
    await page.waitForTimeout(1000);
    // The warning (CANCEL first), CONTINUE, then the save in the list.
    await press(page, 'ArrowUp');
    await press(page, 'Enter');
    await page.waitForTimeout(1500);
    await shot(page, 'pause-load-list');
    await press(page, 'Enter');
    await page.waitForFunction(() => (window.reg2.state() & 7) === 2, null, { timeout: 60000 }).catch(() => {});
    await page.waitForTimeout(3000);
    await shot(page, 'pause-load-done');
    check('pause load: in a game, not paused', ((await state(page)) & 7) === 2);
    check('pause load: no failure screen', await hidden(page, 'crash'));
    await close();
  },

  // The start menu's QUIT: the game ends and the page comes back.
  async exit() {
    const { page, close } = await open();
    await importGame(page);
    await page.click('#play');
    await page.waitForFunction(() => window.reg2 && window.reg2.module && window.reg2.state() & 1, null, { timeout: 60000 });
    await page.waitForTimeout(3000);
    for (const key of ['Enter', 'Enter', 'ArrowUp', 'Enter']) {
      await press(page, key);
      await page.waitForTimeout(1500);
    }
    await page.waitForFunction(() => !document.getElementById('ready').hidden, null, { timeout: 30000 }).catch(() => {});
    check('exit: back on the page', !(await hidden(page, 'ready')));
    check('exit: no failure screen', await hidden(page, 'crash'));
    await close();
  },

  async contextloss() {
    const { page, close } = await open();
    await importGame(page);
    await page.click('#play');
    await page.waitForFunction(() => window.reg2 && window.reg2.module);
    await page.waitForTimeout(6000);
    await page.evaluate(() => document.getElementById('canvas').getContext('webgl2').getExtension('WEBGL_lose_context').loseContext());
    await page.waitForTimeout(1000);
    check('contextloss: message', !(await hidden(page, 'crash')), await page.textContent('#crash-title'));
    await close();
  },

  async offline() {
    const { ctx, page, close } = await open();
    await importGame(page);
    await page.waitForFunction(() => navigator.serviceWorker.controller, null, { timeout: 30000 }).catch(() => {});
    await ctx.setOffline(true);
    await page.goto(SITE);
    await page.waitForTimeout(2000);
    check('offline: page', !(await hidden(page, 'ready')));
    await page.click('#play');
    await page.waitForFunction(() => window.reg2 && window.reg2.module && window.reg2.state() & 1, null, { timeout: 60000 });
    check('offline: game started', true);
    await close();
  },

  // A second tab offers to take over; taking over stops the first one.
  async tabs() {
    const { ctx, page, close } = await open();
    await importGame(page);
    const second = await ctx.newPage();
    await second.goto(SITE);
    await second.waitForTimeout(1500);
    check('tabs: second tab told', !(await hidden(second, 'other-tab')));
    await second.click('#take-over');
    await second.waitForTimeout(1500);
    check('tabs: second tab ready', !(await hidden(second, 'ready')));
    check('tabs: first tab stopped', !(await hidden(page, 'other-tab')));
    await close();
  },

  // The tab hidden in a game: the pause menu opens and the sound stops.
  async hide() {
    const { page, close } = await open();
    await importGame(page);
    await startGame(page, true);
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => true });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForTimeout(500);
    const audio = await page.evaluate(() => window.reg2.audioContext()?.state);
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => false });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForTimeout(1000);
    check('hide: pause menu', (await state(page)) & 4);
    check('hide: sound suspended', audio !== 'running', audio);
    await close();
  },

  // Escape and Alt+Enter pressed again and again, with clicks between (pointer lock asked, left, refused): the game
  // goes on, no failure screen.
  async keys() {
    const { page, close } = await open();
    await importGame(page);
    await startGame(page, true);
    for (let i = 0; i < 12; i++) {
      await page.mouse.click(640, 360);
      await page.keyboard.press('Escape');
      await page.waitForTimeout(150);
      if (i % 3 === 0) await page.keyboard.press('Alt+Enter');
    }
    await page.waitForTimeout(1500);
    check('keys: no failure screen', await hidden(page, 'crash'));
    check('keys: still in a game', (await state(page)) & 2);
    await close();
  },

  // Escape opens the pause menu whether the game or the browser gets it (the browser takes it to leave pointer
  // lock); Escape again resumes, a click takes the pointer again; the page's pause button opens the menu too.
  async escape() {
    const { page, close } = await open();
    await importGame(page);
    await startGame(page, true);
    // In a game with TWIN STICK the browser may hold the pointer (lock), so the button is clicked by script.
    await page.evaluate(() => document.getElementById('pause-button').click());
    await page.waitForTimeout(800);
    check('escape: pause button', (await state(page)) & 4);
    await press(page, 'Escape');
    await page.waitForTimeout(800);
    for (let round = 0; round < 3; round++) {
      await page.mouse.click(640, 360);
      await page.waitForTimeout(500);
      await press(page, 'Escape');
      await page.waitForTimeout(800);
      check(`escape: pause menu (${round + 1})`, (await state(page)) & 4);
      await press(page, 'Escape');
      await page.waitForTimeout(800);
      check(`escape: resumed (${round + 1})`, !((await state(page)) & 4));
    }
    // P, a pause key the browser never takes, opens and closes it the same way.
    await press(page, 'p');
    await page.waitForTimeout(800);
    check('escape: P pauses', (await state(page)) & 4);
    await press(page, 'p');
    await page.waitForTimeout(800);
    check('escape: P resumes', !((await state(page)) & 4));
    check('escape: no failure screen', await hidden(page, 'crash'));
    await close();
  },

  // The page resized in a game (as full screen does): the game takes the canvas' new size, not a scaled frame.
  async resize() {
    const { page, close } = await open();
    const modes = [];
    page.on('console', (m) => { if (/Vid_SetMode \d+: \d+x\d+/.test(m.text())) modes.push(m.text().match(/(\d+x\d+)/)[1]); });
    await importGame(page);
    await startGame(page, true);
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.waitForTimeout(2000);
    check('resize: game at the new size', modes.includes('1280x900'), modes.join(' '));
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.waitForTimeout(2000);
    check('resize: and back', modes[modes.length - 1] === '1280x720', modes.join(' '));
    await shot(page, 'resize');
    await close();
  },

  // A new build reaches a new tab at once; offline, the cached build starts.
  async update() {
    if (!process.env.SITE_DIR) {
      console.log('  (update needs SITE_DIR)');
      return;
    }
    const dir = mkdtempSync(join(tmpdir(), 'reg2-site-'));
    cpSync(process.env.SITE_DIR, dir, { recursive: true });
    const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css', '.json': 'application/json', '.svg': 'image/svg+xml', '.webmanifest': 'application/manifest+json' };
    const server = createServer((req, res) => {
      let file = join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
      try {
        if (statSync(file).isDirectory()) file = join(file, 'index.html');
        statSync(file);
      } catch {
        res.writeHead(404);
        return res.end();
      }
      res.writeHead(200, { 'content-type': types[extname(file)] || 'application/octet-stream', 'cache-control': 'no-cache' });
      createReadStream(file).pipe(res);
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const url = `http://127.0.0.1:${server.address().port}/`;
    const { ctx, page, close } = await open();
    await page.goto(url);
    await page.waitForFunction(() => navigator.serviceWorker.controller, null, { timeout: 30000 });
    // The next build: another version in the service worker, another title in the page.
    const sw = readFileSync(join(dir, 'sw.js'), 'utf8').replace(/const VERSION = '[^']*'/, "const VERSION = 'next'");
    writeFileSync(join(dir, 'sw.js'), sw);
    writeFileSync(join(dir, 'index.html'), readFileSync(join(dir, 'index.html'), 'utf8').replace('<title>reG2</title>', '<title>reG2 next</title>'));
    const tab = await ctx.newPage();
    await tab.goto(url);
    const got = await tab.waitForFunction(() => document.title === 'reG2 next', null, { timeout: 30000 }).then(() => true, () => false);
    check('update: a new tab has the new build', got, await tab.title());
    // The first tab, opened before the new build and idle since, starts the importer from its old build folder:
    // gone from the site and from the new worker's cache, so the page reloads once into the new build.
    const build = readdirSync(dir).find((f) => f.startsWith('build-'));
    renameSync(join(dir, build), join(dir, 'build-0000000000aa'));
    writeFileSync(join(dir, 'app.js'), readFileSync(join(dir, 'app.js'), 'utf8').replace(build + '/', 'build-0000000000aa/'));
    writeFileSync(join(dir, 'sw.js'), readFileSync(join(dir, 'sw.js'), 'utf8').replaceAll(build + '/', 'build-0000000000aa/')
      .replace(/const VERSION = '[^']*'/, "const VERSION = 'third'"));
    writeFileSync(join(dir, 'index.html'), readFileSync(join(dir, 'index.html'), 'utf8').replace('<title>reG2 next</title>', '<title>reG2 third</title>'));
    // Another tab brings the new worker, which drops the old build's cache.
    const third = await ctx.newPage();
    await third.goto(url);
    await third.waitForTimeout(5000);
    await third.close();
    const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click('#pick-file')]);
    await chooser.setFiles(GTA2);
    const moved = await page.waitForFunction(() => document.title === 'reG2 third', null, { timeout: 30000 }).then(() => true, () => false);
    check('update: an old tab reloads into the new build', moved, await page.title());
    // The other tabs hold the storage: left first, as a player would.
    await page.goto('about:blank');
    await tab.close();
    await ctx.setOffline(true);
    const offline = await ctx.newPage();
    await offline.goto(url).catch(() => {});
    const started = await offline.waitForFunction(() => !document.getElementById('import').hidden || !document.getElementById('ready').hidden, null, { timeout: 30000 }).then(() => true, () => false);
    check('update: offline, the page starts', started, await offline.title().catch(() => '?'));
    await close();
    server.close();
    rmSync(dir, { recursive: true, force: true });
  },

  // A phone in landscape: the menus with the d-pad and OK, the stick.
  async touch() {
    const { ctx, page, close } = await open(true);
    await importGame(page);
    await page.click('#play');
    await page.waitForFunction(() => window.reg2 && window.reg2.module);
    await page.waitForTimeout(6000);
    check('touch: controls shown', !(await hidden(page, 'touch')));
    const tapAt = async (locator) => {
      const b = await locator.boundingBox();
      await page.touchscreen.tap(b.x + b.width / 2, b.y + b.height / 2);
      await page.waitForTimeout(1500);
    };
    const ok = page.locator('#touch .tbtn.menu-only', { hasText: 'OK' });
    const option = () => page.evaluate(() => window.reg2.module._port_WebMenu(1));
    // The d-pad and OK only: the intro, the style, then down and up on the start menu.
    await tapAt(ok);
    await tapAt(ok);
    await tapAt(page.locator('#touch .dpad .d'));
    check('touch: the d-pad moves down the menu', (await option()) === 1);
    await tapAt(page.locator('#touch .dpad .u'));
    check('touch: and up', (await option()) === 0);
    // PLAY, START PLAY IN AREA.
    for (let i = 0; i < 4 && !((await state(page)) & 2); i++) await tapAt(ok);
    await page.waitForFunction(() => window.reg2.state() & 2, null, { timeout: 60000 });
    await page.waitForTimeout(3000);
    const cdp = await ctx.newCDPSession(page);
    const player = (i) => page.evaluate((i) => window.reg2.module._port_WebPlayer(i), i);
    const before = [await player(0), await player(1)];
    // The stick pushed down (the player starts below a wall): he walks.
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: 150, y: 250 }] });
    for (let i = 1; i <= 10; i++) {
      await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: 150, y: 250 + 7 * i }] });
      await page.waitForTimeout(30);
    }
    await page.waitForTimeout(3000);
    check('touch: in-game controls', ['twin', 'classic'].includes(await page.evaluate(() => document.getElementById('touch').dataset.mode)));
    const after = [await player(0), await player(1)];
    const walked = Math.hypot(after[0] - before[0], after[1] - before[1]);
    check('touch: the stick walks the player', walked > 0.3, `${walked.toFixed(2)} blocks`);
    await shot(page, 'touch');
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await page.waitForTimeout(500);
    // At rest, the virtual pad's triggers read 0: no attack (RT with TWIN STICK), no special.
    check('touch: nothing held at rest', (await player(3)) === 0);
    await close();
  },
};

if (!GTA2) {
  console.log('GTA2=<a GTA2 installer, disc image or zip> is needed');
  process.exit(2);
}
for (const name of wanted.length ? wanted : Object.keys(scenarios)) {
  if (name === 'touch' && which !== 'chromium') continue;
  console.log(`-- ${which} ${name}`);
  try {
    await scenarios[name]();
  } catch (e) {
    check(name, false, e.message.split('\n')[0]);
  }
}
console.log(failures.length ? `${failures.length} failed: ${failures.join(', ')}` : 'all passed');
process.exit(failures.length ? 1 : 0);
