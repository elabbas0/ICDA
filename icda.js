// Boots kernel.iso in QEMU compiled to WebAssembly and wires the guest
// display, keyboard and pointer to a canvas.  The QEMU side of this is the
// "canvas" display in the patched qemu-wasm build (see web/README.md).

import initQemu from './qemu/qemu-system-x86_64.js';

const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');
const logEl = document.getElementById('log');
const startBtn = document.getElementById('start');

const EV_KEY = 1, EV_ABS = 2, EV_BTN = 3, EV_WHEEL = 4;

// KeyboardEvent.code -> Linux input keycode (QEMU maps these to its own codes)
const LINUX_KEYS = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7,
  Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11, Minus: 12, Equal: 13,
  Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20,
  KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26,
  BracketRight: 27, Enter: 28, ControlLeft: 29, KeyA: 30, KeyS: 31, KeyD: 32,
  KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39,
  Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45,
  KeyC: 46, KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50, Comma: 51, Period: 52,
  Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56, Space: 57,
  CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65,
  F8: 66, F9: 67, F10: 68, NumLock: 69, ScrollLock: 70, Numpad7: 71,
  Numpad8: 72, Numpad9: 73, NumpadSubtract: 74, Numpad4: 75, Numpad5: 76,
  Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81,
  Numpad0: 82, NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88,
  NumpadEnter: 96, ControlRight: 97, NumpadDivide: 98, PrintScreen: 99,
  AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105,
  ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110,
  Delete: 111, Pause: 119, MetaLeft: 125, MetaRight: 126, ContextMenu: 127,
};

let Module = null;
let ring = null;       // Int32Array view of the event ring
let shared = null;     // Int32Array view of [pending, head, tail]
let ringSize = 0;
let image = null;
let booted = false;

function setStatus(text) {
  statusEl.textContent = text;
}

function log(line) {
  // Boot progress from the serial console, until the desktop is up
  if (!booted) {
    const stage = /^\[stage\] \[S\d+ ([^\]]+)\]/.exec(line);
    if (stage) setStatus(`Booting ICDA: ${stage[1]}…`);
    if (line.startsWith('wm:')) {
      booted = true;
      setStatus(matchMedia('(pointer: coarse)').matches
        ? 'Running. Tap Keyboard to type.'
        : 'Running. Click the screen to type.');
    }
  }
  logEl.textContent += line + '\n';
  if (logEl.textContent.length > 200000) {
    logEl.textContent = logEl.textContent.slice(-100000);
  }
  logEl.scrollTop = logEl.scrollHeight;
}

function push(type, a, b) {
  if (!ring) return;
  const head = Atomics.load(shared, 1);
  const tail = Atomics.load(shared, 2);
  if (head - tail >= ringSize) return; // full: drop the event
  const i = (head % ringSize) * 4;
  ring[i] = type;
  ring[i + 1] = a;
  ring[i + 2] = b;
  Atomics.store(shared, 1, head + 1);
}

function pointerPos(e) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round((e.clientX - r.left) * canvas.width / r.width);
  const y = Math.round((e.clientY - r.top) * canvas.height / r.height);
  return [Math.max(0, Math.min(canvas.width - 1, x)),
          Math.max(0, Math.min(canvas.height - 1, y))];
}

// Characters typed on a phone's on-screen keyboard -> [Linux keycode, shift]
const CHAR_KEYS = (() => {
  const map = { ' ': [57, 0], '\n': [28, 0], '\t': [15, 0] };
  const rows = [
    ['1234567890-=', '!@#$%^&*()_+', 2],
    ['qwertyuiop[]', 'QWERTYUIOP{}', 16],
    ["asdfghjkl;'`", 'ASDFGHJKL:"~', 30],
    ['\\zxcvbnm,./', '|ZXCVBNM<>?', 43],
  ];
  for (const [plain, shifted, first] of rows) {
    [...plain].forEach((c, i) => { map[c] = [first + i, 0]; });
    [...shifted].forEach((c, i) => { map[c] = [first + i, 1]; });
  }
  return map;
})();

// Key presses replayed from the text field arrive in bursts (a whole
// autocorrected word at once), and ICDA drops keys when too many land in one
// display tick.  Feed them to QEMU a couple at a time instead.
const keyQueue = [];
let keyTimer = 0;

function queueKey(code, down) {
  keyQueue.push([code, down]);
  if (!keyTimer) {
    keyTimer = setInterval(() => {
      for (let i = 0; i < 2 && keyQueue.length; i++) push(EV_KEY, ...keyQueue.shift());
      if (!keyQueue.length) { clearInterval(keyTimer); keyTimer = 0; }
    }, 20);
  }
}

function tapKey(code, shift) {
  if (shift) queueKey(42, 1);
  queueKey(code, 1);
  queueKey(code, 0);
  if (shift) queueKey(42, 0);
}

function typeText(text) {
  for (const c of text.normalize('NFKD')) {
    const k = CHAR_KEYS[c];
    if (k) tapKey(k[0], k[1]);
  }
}

function releaseModifiers() {
  for (const k of ['ShiftLeft', 'ShiftRight', 'ControlLeft', 'ControlRight',
                   'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight']) {
    push(EV_KEY, LINUX_KEYS[k], 0);
  }
}

// Phones only show their keyboard for a focused text field, and many send no
// usable key codes (Android reports keyCode 229 for everything).  So on touch
// devices a hidden textarea takes the focus, and whatever the keyboard does to
// its text is replayed as key presses: added characters are typed, removed
// ones become Backspace.  The field always holds a few placeholder characters
// so that Backspace has something to delete.
function wireSoftKeyboard() {
  const field = document.getElementById('softkbd');
  const button = document.getElementById('kbd');
  const FILL = '    ';
  let last = FILL;
  let wanted = false;

  const reset = () => {
    field.value = FILL;
    last = FILL;
    field.setSelectionRange(FILL.length, FILL.length);
  };
  const open = () => {
    wanted = true;
    reset();
    field.focus({ preventScroll: true });
    button.setAttribute('aria-pressed', 'true');
  };
  const close = () => {
    wanted = false;
    field.blur();
    button.setAttribute('aria-pressed', 'false');
  };

  if (matchMedia('(pointer: coarse)').matches) button.hidden = false;
  // pointerdown would move the focus to the button and close the keyboard
  // before the click toggles it.
  button.addEventListener('pointerdown', (e) => e.preventDefault());
  button.addEventListener('click', () => (wanted ? close() : open()));

  field.addEventListener('keydown', (e) => {
    // A hardware keyboard reports real codes: send those directly and keep
    // them out of the text field.
    const code = LINUX_KEYS[e.code];
    if (code && !e.isComposing && e.keyCode !== 229) {
      push(EV_KEY, code, 1);
      e.preventDefault();
    }
  });
  field.addEventListener('keyup', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code && !e.isComposing && e.keyCode !== 229) push(EV_KEY, code, 0);
  });
  field.addEventListener('input', (e) => {
    const now = field.value;
    let same = 0;
    while (same < last.length && same < now.length && last[same] === now[same]) same++;
    for (let i = same; i < last.length; i++) tapKey(14, 0); // Backspace
    typeText(now.slice(same));
    last = now;
    if (now.length < 2 || /\n/.test(now) || (now.length > 64 && !e.isComposing)) reset();
  });
  field.addEventListener('compositionend', () => {
    if (last.length > 64) reset();
  });
  // Fires when the phone's own "done" button hides the keyboard, too.
  field.addEventListener('blur', () => {
    releaseModifiers();
    wanted = false;
    button.setAttribute('aria-pressed', 'false');
  });

  return { active: () => wanted };
}

function wireInput() {
  const softKeyboard = wireSoftKeyboard();
  canvas.addEventListener('keydown', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code) { push(EV_KEY, code, 1); e.preventDefault(); }
  });
  canvas.addEventListener('keyup', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code) { push(EV_KEY, code, 0); e.preventDefault(); }
  });
  canvas.addEventListener('pointermove', (e) => push(EV_ABS, ...pointerPos(e)));
  canvas.addEventListener('pointerdown', (e) => {
    // Focusing the canvas would close the on-screen keyboard.
    if (!softKeyboard.active()) canvas.focus();
    push(EV_ABS, ...pointerPos(e));
    if (e.button <= 2) push(EV_BTN, e.button, 1);
    e.preventDefault();
  });
  canvas.addEventListener('pointerup', (e) => {
    if (e.button <= 2) push(EV_BTN, e.button, 0);
    e.preventDefault();
  });
  canvas.addEventListener('wheel', (e) => {
    push(EV_WHEEL, e.deltaY < 0 ? 1 : -1, 0);
    e.preventDefault();
  }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  // Release every modifier when focus leaves, so none stay stuck down.
  canvas.addEventListener('blur', releaseModifiers);
}

async function fetchWithProgress(url, label) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const total = Number(res.headers.get('content-length')) || 0;
  const reader = res.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    const mb = (got / 1048576).toFixed(1);
    setStatus(total ? `Downloading ${label}: ${mb} / ${(total / 1048576).toFixed(1)} MB`
                    : `Downloading ${label}: ${mb} MB`);
  }
  const out = new Uint8Array(got);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

async function boot() {
  startBtn.disabled = true;
  if (!self.crossOriginIsolated) {
    setStatus('This page needs cross-origin isolation (SharedArrayBuffer). Reloading…');
    return;
  }
  // The kernel is served gzip-compressed and unpacked here at native speed.
  // QEMU loads it through the Multiboot 1 shim (shim/shim.c) instead of
  // GRUB, which takes over a minute to read it from an emulated CD.
  const gz = await fetchWithProgress('kernel.bin.gz', 'ICDA kernel');
  setStatus('Unpacking the kernel…');
  const kernel = new Uint8Array(await new Response(
    new Blob([gz]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
  const shim = new Uint8Array(await (await fetch('shim.elf')).arrayBuffer());
  const bios = {};
  for (const f of ['bios-256k.bin', 'vgabios-stdvga.bin', 'kvmvapic.bin',
                   'efi-e1000.rom', 'multiboot_dma.bin']) {
    bios[f] = new Uint8Array(await (await fetch('qemu/pc-bios/' + f)).arrayBuffer());
  }
  setStatus('Starting the emulator…');

  Module = {
    arguments: [
      '-L', '/pc-bios',
      '-m', '256M',
      '-accel', 'tcg,tb-size=500',
      '-display', 'canvas',
      '-vga', 'std',
      '-serial', 'file:/dev/stdout',
      '-monitor', 'none',
      '-device', 'qemu-xhci',
      '-device', 'usb-tablet',
      '-nic', 'none',
      '-kernel', '/icda/shim.elf',
      '-initrd', '/icda/kernel.bin',
      '-no-reboot',
    ],
    mainScriptUrlOrBlob: new URL('./qemu/qemu-system-x86_64.js', import.meta.url).href,
    locateFile: (p) => new URL('./qemu/' + p, import.meta.url).href,
    print: log,
    printErr: log,
    preRun: [(mod) => {
      mod.FS.mkdir('/pc-bios');
      for (const [name, data] of Object.entries(bios)) {
        mod.FS.writeFile('/pc-bios/' + name, data);
      }
      mod.FS.mkdir('/icda');
      mod.FS.writeFile('/icda/shim.elf', shim);
      mod.FS.writeFile('/icda/kernel.bin', kernel);
    }],
    canvasInit(ringPtr, sharedPtr, size) {
      ring = new Int32Array(Module.HEAP32.buffer, ringPtr, size * 4);
      shared = new Int32Array(Module.HEAP32.buffer, sharedPtr, 4);
      ringSize = size;
      setStatus('Booting ICDA…');
    },
    canvasResize(w, h) {
      canvas.width = w;
      canvas.height = h;
      image = ctx.createImageData(w, h);
    },
    canvasFrame(ptr, w, h, pendingPtr) {
      if (!image || image.width !== w || image.height !== h) {
        canvas.width = w;
        canvas.height = h;
        image = ctx.createImageData(w, h);
      }
      image.data.set(Module.HEAPU8.subarray(ptr, ptr + w * h * 4));
      Atomics.store(Module.HEAP32, pendingPtr >> 2, 0);
      ctx.putImageData(image, 0, 0);
    },
  };
  wireInput();
  await initQemu(Module);
  canvas.focus();
}

startBtn.addEventListener('click', () => {
  boot().catch((err) => {
    setStatus('Failed: ' + err.message);
    console.error(err);
  });
});

if (new URLSearchParams(location.search).has('autostart')) {
  startBtn.click();
}
