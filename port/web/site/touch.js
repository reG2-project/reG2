// On-screen controls for touch screens, held in landscape with two thumbs. They drive a virtual gamepad in the game
// (port_WebPad in web/src/web_input.cpp), so the port's pad support does the rest (port/compat/src/pad.cpp):
// - on foot, a floating stick on the left half walks; on the right, ATTACK, JUMP, ENTER and the weapons; with
//   TWIN STICK, the right half is a second floating stick that aims and fires while held (right stick and RT);
// - in a vehicle, the left stick steers; GAS and BRAKE (RT, LT), HAND BRAKE, FIRE, HORN and EXIT on the right;
// - in the menus, taps reach the game as the mouse; a d-pad (the pad's, repeating while held), OK (A) and BACK (B).
// In a game the controls cover the whole screen, so that no touch reaches the canvas as a mouse click.
// The action set follows the 3DS port's layout (attack or gas, jump or hand brake, get in or out, the weapons).

// SDL_GamepadButton bits and SDL_GamepadAxis indices.
const A = 1 << 0, B = 1 << 1, X = 1 << 2, Y = 1 << 3, START = 1 << 6, LB = 1 << 9, RB = 1 << 10;
const UP = 1 << 11, DOWN = 1 << 12, LEFT = 1 << 13, RIGHT = 1 << 14;
const RX = 2, LT = 4, RT = 5;
// SDL reads the virtual pad once a frame: a tap lasts at least this long.
const MIN_PRESS_MS = 50;
// The menus read the pad once a frame at 30 Hz or less: each d-pad press and release lasts this long, so that
// quick taps and repeats all count on a slow phone too.
const DPAD_STEP_MS = 80;
const DPAD_REPEAT_DELAY_MS = 400;
const DPAD_REPEAT_MS = 180;
// The aiming stick fires from this far out of its range; nearer, it only turns the player.
const FIRE_FORCE = 0.35;

// Places from the screen's edges, in CSS pixels at size 100% or as a share of the screen (inside the safe areas); size
// in pixels; mode: classic (on foot), twin (on foot, aiming with a stick), car, game (all three) or menu.
const BUTTONS = [
  { label: 'attack', bit: A, mode: 'classic', right: 24, bottom: 28, size: 92 },
  { label: 'jump', bit: B, mode: 'classic', right: 132, bottom: 22, size: 68 },
  { label: 'enter', bit: Y, mode: 'classic', right: 30, bottom: 136, size: 68 },
  { label: 'prev', bit: LB, mode: 'classic', right: 128, bottom: 112, size: 56, raw: '◀' },
  { label: 'next', bit: RB, mode: 'classic', right: 108, bottom: 184, size: 56, raw: '▶' },
  { label: 'jump', bit: B, mode: 'twin', right: 20, bottom: 20, size: 64 },
  { label: 'enter', bit: Y, mode: 'twin', right: 20, bottom: 96, size: 64 },
  { label: 'prev', bit: LB, mode: 'twin', right: 84, bottom: 172, size: 56, raw: '◀' },
  { label: 'next', bit: RB, mode: 'twin', right: 20, bottom: 172, size: 56, raw: '▶' },
  { label: 'gas', axis: RT, mode: 'car', right: 24, bottom: 28, size: 96 },
  { label: 'brake', axis: LT, mode: 'car', right: 138, bottom: 22, size: 80 },
  { label: 'handbrake', bit: B, mode: 'car', right: 150, bottom: 118, size: 64 },
  { label: 'fire', bit: A, mode: 'car', right: 34, bottom: 140, size: 64 },
  { label: 'horn', bit: X, mode: 'car', right: 236, bottom: 30, size: 56 },
  { label: 'exit', bit: Y, mode: 'car', right: 104, bottom: 210, size: 60 },
  // Under the HUD's money, hearts and weapon, which take the top fifth of the right edge.
  { label: 'pause', bit: START, mode: 'game', right: 16, top: '21%', size: 56, raw: '❚❚' },
  { label: 'ok', bit: A, mode: 'menu', right: 18, bottom: 22, size: 84 },
  { label: 'back', bit: B, mode: 'menu', right: 28, bottom: 128, size: 64 },
];

const LABELS = {
  en: { ok: 'OK', attack: 'Attack', jump: 'Jump', enter: 'Enter', gas: 'Gas', brake: 'Brake', handbrake: 'Hand brake', exit: 'Exit', fire: 'Fire', horn: 'Horn', back: 'Back' },
  fr: { ok: 'OK', attack: 'Frapper', jump: 'Sauter', enter: 'Monter', gas: 'Gaz', brake: 'Frein', handbrake: 'Frein à main', exit: 'Sortir', fire: 'Tirer', horn: 'Klaxon', back: 'Retour' },
  de: { ok: 'OK', attack: 'Angriff', jump: 'Springen', enter: 'Einsteigen', gas: 'Gas', brake: 'Bremse', handbrake: 'Handbremse', exit: 'Aussteigen', fire: 'Feuer', horn: 'Hupe', back: 'Zurück' },
  it: { ok: 'OK', attack: 'Attacca', jump: 'Salta', enter: 'Sali', gas: 'Gas', brake: 'Freno', handbrake: 'Freno a mano', exit: 'Scendi', fire: 'Spara', horn: 'Clacson', back: 'Indietro' },
  es: { ok: 'OK', attack: 'Atacar', jump: 'Saltar', enter: 'Subir', gas: 'Gas', brake: 'Freno', handbrake: 'Freno de mano', exit: 'Bajar', fire: 'Disparar', horn: 'Claxon', back: 'Atrás' },
};

// A floating stick: it moves to where the thumb lands in its zone and follows that one finger; at rest it shows,
// faint, at its zone's usual place (style.css). onMove(x, y, force)
// with x, y from -1 to 1 (y down) and force from 0 to 1; onEnd when the finger lifts or the touch is cancelled.
function stick(zone, onMove, onEnd) {
  const base = document.createElement('div');
  base.className = 'stick-base';
  const knob = document.createElement('div');
  knob.className = 'stick-knob';
  base.appendChild(knob);
  zone.appendChild(base);
  let id = null;
  let ox = 0;
  let oy = 0;
  // Under pointer lock every event has the same position: the stick starts at the first one after the lock ends.
  let placed = false;
  const place = (e) => {
    const r = zone.getBoundingClientRect();
    ox = e.clientX;
    oy = e.clientY;
    base.style.left = `${ox - r.left}px`;
    base.style.top = `${oy - r.top}px`;
    base.classList.add('on');
    placed = true;
  };
  const radius = () => base.offsetWidth / 2 || 60;
  const end = () => {
    if (id === null) return;
    id = null;
    base.classList.remove('on');
    base.style.left = base.style.top = '';
    knob.style.transform = '';
    onEnd();
  };
  zone.addEventListener('pointerdown', (e) => {
    if (id !== null) return;
    e.preventDefault();
    id = e.pointerId;
    try {
      zone.setPointerCapture(id);
    } catch {}
    placed = false;
    if (!document.pointerLockElement) place(e);
    onMove(0, 0, 0);
  });
  zone.addEventListener('pointermove', (e) => {
    if (e.pointerId !== id || document.pointerLockElement) return;
    if (!placed) return place(e);
    const r = radius();
    let dx = e.clientX - ox;
    let dy = e.clientY - oy;
    const d = Math.hypot(dx, dy);
    if (d > r) {
      dx *= r / d;
      dy *= r / d;
    }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    onMove(dx / r, dy / r, Math.min(1, d / r));
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    zone.addEventListener(type, (e) => {
      if (e.pointerId === id) end();
    });
  }
  return { end, forget: (pointerId) => (pointerId === null || pointerId === id) && end() };
}

// The menus' d-pad: one finger points a direction from its centre and may slide to another. setBits(bits) gets the
// d-pad's bits, each state held DPAD_STEP_MS; a direction held repeats.
function dpad(el, setBits) {
  const arms = {};
  for (const [bit, cls, glyph] of [[UP, 'u', '▲'], [DOWN, 'd', '▼'], [LEFT, 'l', '◀'], [RIGHT, 'r', '▶']]) {
    const arm = document.createElement('span');
    arm.className = cls;
    arm.textContent = glyph;
    el.appendChild(arm);
    arms[bit] = arm;
  }
  let id = null;
  let dir = 0;
  let repeat = 0;
  const queue = [];
  let applied = 0;
  let pump = 0;
  const next = () => {
    pump = 0;
    if (!queue.length) return;
    applied = queue.shift();
    setBits(applied);
    pump = setTimeout(next, DPAD_STEP_MS);
  };
  const push = (bits) => {
    if ((queue.length ? queue[queue.length - 1] : applied) === bits) return;
    queue.push(bits);
    if (!pump) next();
  };
  const set = (d) => {
    if (d === dir) return;
    clearTimeout(repeat);
    if (dir) arms[dir].classList.remove('down');
    dir = d;
    push(d);
    if (!d) return;
    arms[d].classList.add('down');
    if (navigator.vibrate) navigator.vibrate(10);
    const again = () => {
      push(0);
      push(d);
      repeat = setTimeout(again, DPAD_REPEAT_MS);
    };
    repeat = setTimeout(again, DPAD_REPEAT_DELAY_MS);
  };
  const aim = (e) => {
    const r = el.getBoundingClientRect();
    const dx = e.clientX - (r.left + r.width / 2);
    const dy = e.clientY - (r.top + r.height / 2);
    if (Math.hypot(dx, dy) < r.width * 0.12) return set(0);
    set(Math.abs(dx) > Math.abs(dy) ? (dx < 0 ? LEFT : RIGHT) : dy < 0 ? UP : DOWN);
  };
  el.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    e.stopPropagation();
    if (id !== null) return;
    id = e.pointerId;
    try {
      el.setPointerCapture(id);
    } catch {}
    aim(e);
  });
  el.addEventListener('pointermove', (e) => {
    if (e.pointerId === id) aim(e);
  });
  const end = () => {
    id = null;
    set(0);
  };
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    el.addEventListener(type, (e) => {
      if (e.pointerId === id) end();
    });
  }
  // The page hidden or the layout changed: released at once, nothing left queued.
  const release = () => {
    id = null;
    clearTimeout(repeat);
    clearTimeout(pump);
    pump = 0;
    queue.length = 0;
    if (dir) arms[dir].classList.remove('down');
    dir = applied = 0;
  };
  return { release, forget: (pointerId) => id !== null && (pointerId === null || pointerId === id) && end() };
}

export class TouchControls {
  constructor(root, module, getState, prefs) {
    this.root = root;
    this.module = module;
    this.getState = getState;
    this.buttons = 0;
    this.axes = [0, 0, 0, 0, 0, 0];
    this.usedTouch = false;
    this.visible = false;
    this.releases = [];
    this.forgets = [];
    const lang = (navigator.language || 'en').slice(0, 2);
    const labels = LABELS[lang] || LABELS.en;

    const add = (className) => {
      const el = document.createElement('div');
      el.className = className;
      root.appendChild(el);
      return el;
    };
    // Touches that land on no control in a game stop here instead of reaching the canvas as the mouse.
    add('zone blocker game-only');
    this.moveZone = add('zone stick-zone game-only');
    this.aimZone = add('zone aim-zone twin-only');
    this.sticks = [
      stick(this.moveZone, (x, y) => this.setAxes(0, x, y), () => this.setAxes(0, 0, 0)),
      stick(this.aimZone, (x, y, force) => {
        this.axes[RT] = force >= FIRE_FORCE ? 32767 : 0;
        this.setAxes(RX, x, y);
      }, () => {
        this.axes[RT] = 0;
        this.setAxes(RX, 0, 0);
      }),
    ];

    const pad = dpad(add('dpad menu-only'), (bits) => {
      this.buttons = (this.buttons & ~(UP | DOWN | LEFT | RIGHT)) | bits;
      this.send();
    });
    this.releases.push(pad.release);
    this.forgets.push(pad.forget);

    for (const def of BUTTONS) {
      const el = add(`tbtn ${def.mode}-only${def.mode === 'game' || def.mode === 'menu' ? '' : ' game-only'}`);
      el.textContent = def.raw || labels[def.label];
      el.setAttribute('aria-hidden', 'true');
      for (const side of ['right', 'bottom', 'left', 'top']) {
        const v = def[side];
        if (v !== undefined) el.style[side] = `calc(${typeof v === 'string' ? v : `${v}px * var(--touch-scale, 1)`} + env(safe-area-inset-${side}))`;
      }
      el.style.setProperty('--size', `${def.size}px`);
      const pointers = new Set();
      let pressedAt = 0;
      let releaseTimer = 0;
      const set = (down) => {
        el.classList.toggle('down', down);
        if (def.axis !== undefined) this.axes[def.axis] = down ? 32767 : 0;
        else this.buttons = down ? this.buttons | def.bit : this.buttons & ~def.bit;
        this.send();
      };
      const update = () => {
        clearTimeout(releaseTimer);
        if (pointers.size) {
          pressedAt = performance.now();
          set(true);
          if (navigator.vibrate) navigator.vibrate(10);
        } else {
          const left = MIN_PRESS_MS - (performance.now() - pressedAt);
          if (left > 0) releaseTimer = setTimeout(() => set(pointers.size > 0), left);
          else set(false);
        }
      };
      el.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        e.stopPropagation();
        try {
          el.setPointerCapture(e.pointerId);
        } catch {}
        pointers.add(e.pointerId);
        update();
      });
      for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
        el.addEventListener(type, (e) => {
          if (pointers.delete(e.pointerId)) update();
        });
      }
      this.releases.push(() => {
        pointers.clear();
        clearTimeout(releaseTimer);
        el.classList.remove('down');
      });
      this.forgets.push((id) => {
        if (id === null ? pointers.size : pointers.delete(id)) {
          if (id === null) pointers.clear();
          update();
        }
      });
    }
    // No long-press menu on the controls.
    root.addEventListener('contextmenu', (e) => e.preventDefault());
    // A release the button itself did not get (the finger left it, the browser cancelled the touch, a lost
    // capture) still releases it, as does the last finger lifted (null: any pointer). A quick tap still lasts
    // MIN_PRESS_MS. The page in the background releases all at once.
    for (const type of ['pointerup', 'pointercancel']) {
      addEventListener(type, (e) => {
        this.forget(e.pointerId);
      }, true);
    }
    for (const type of ['touchend', 'touchcancel']) {
      addEventListener(type, (e) => {
        if (!e.touches.length) this.forget(null);
      }, { capture: true, passive: true });
    }
    addEventListener('blur', () => this.release());
    document.addEventListener('visibilitychange', () => this.release());

    // Shown while touch is the input in use (or always, or never, as set on the page).
    // Browsers follow a touch with emulated mouse events: those are not a mouse in use.
    let lastTouch = 0;
    // A pointer lock the game took for the mouse (TWIN STICK) freezes every touch's position: a touch ends it.
    addEventListener('pointerdown', (e) => {
      if (e.pointerType === 'touch' && document.pointerLockElement) document.exitPointerLock();
    }, true);
    addEventListener('touchstart', () => {
      lastTouch = performance.now();
      this.setUsed(true);
    }, { capture: true, passive: true });
    addEventListener('touchend', () => (lastTouch = performance.now()), { capture: true, passive: true });
    addEventListener('keydown', (e) => {
      if (e.isTrusted) this.setUsed(false);
    }, true);
    addEventListener('pointermove', (e) => {
      if (e.pointerType === 'mouse' && (e.movementX || e.movementY) && performance.now() - lastTouch > 1000) this.setUsed(false);
    }, true);
    addEventListener('gamepadconnected', () => this.setUsed(false));
    this.usedTouch = matchMedia('(pointer: coarse)').matches || navigator.maxTouchPoints > 0;
    this.setPrefs(prefs);
    this.poll();
  }

  setAxes(first, x, y) {
    this.axes[first] = Math.round(x * 32767);
    this.axes[first + 1] = Math.round(y * 32767);
    this.send();
  }

  setUsed(touch) {
    if (this.usedTouch === touch) return;
    this.usedTouch = touch;
    this.refresh();
  }

  setPrefs(prefs) {
    this.prefs = prefs;
    this.root.style.setProperty('--touch-scale', prefs.touchSize / 100);
    this.root.style.setProperty('--touch-opacity', prefs.touchOpacity / 100);
    this.refresh();
  }

  refresh() {
    const mode = this.prefs.touchMode;
    const visible = mode === 'on' || (mode === 'auto' && this.usedTouch);
    if (visible !== this.visible) {
      this.visible = visible;
      this.root.hidden = !visible;
      // The controls have their own pause button, and the page asks for no pointer lock while they are in use.
      document.body.classList.toggle('touch-on', visible);
      if (!visible) this.release();
      else {
        this.send();
        if (document.pointerLockElement) document.exitPointerLock();
      }
    }
  }

  // The game's state picks the layout; a physical gamepad in use hides the controls, as a key or the mouse does.
  poll() {
    const state = this.getState();
    const mode = !(state & 2) || state & 4 ? 'menu' : state & 8 ? 'car' : state & 16 ? 'twin' : 'classic';
    if (this.root.dataset.mode !== mode) {
      this.root.dataset.mode = mode;
      this.release();
    }
    if (this.visible && navigator.getGamepads) {
      for (const pad of navigator.getGamepads()) {
        if (pad && pad.buttons.some((b) => b.pressed)) {
          this.setUsed(false);
          break;
        }
      }
    }
    requestAnimationFrame(() => this.poll());
  }

  forget(pointerId) {
    for (const forget of this.forgets) forget(pointerId);
    for (const s of this.sticks) s.forget(pointerId);
  }

  release() {
    for (const s of this.sticks) s.end();
    this.buttons = 0;
    this.axes.fill(0);
    for (const r of this.releases) r();
    this.send();
  }

  send() {
    // The game gets its virtual pad only once the controls are in use.
    if (!this.visible && !this.attached) return;
    this.attached = true;
    try {
      this.module._port_WebPad(this.buttons, ...this.axes);
    } catch {}
  }
}
