// The CrossPad firmware itself, running in the page: crosspad-core,
// crosspad-gui and every app exactly as platform-idf builds them for the
// ESP32-S3, over crosspad-pc's platform layer, compiled to WebAssembly
// (CMake preset web-twin -> web/build/; docs/web-twin.md). This file is the
// board around it:
//   · Web MIDI   → RtMidi         (the sim's own MIDI I/O, as on the desktop)
//   · WebAudio   → RtAudio        (OUT1/OUT2 to the speakers, microphone as IN)
//   · Web Serial → PcUart         (the emulator's USB jack)
//   · IndexedDB  → /sdcard + the settings profile (persistent, zip in/out)
//   · the LCD    → RGBA for the 3D twin; pads/encoder/touch/power in
const LCD_W = 320, LCD_H = 240;
const PROFILE = '/home/web_user/.crosspad';
const SD = '/sdcard';

export async function startSim({
  base = new URL('./build/', import.meta.url).href,
  canvas = null,           // a visible canvas shows the whole emulator (pads, knob) and takes the mouse
  lcd = false,             // true: the display is the LCD alone, as on the board (the live twin of a board)
  seed = new URL('./board/sdseed.json', import.meta.url).href,         // the board's card (tools/twin/make_seed.sh); null: none
  order = new URL('./board/sdseed-order.json', import.meta.url).href,  // its directories' order (tools/twin/pull_order.py); null: none
  midi = true,             // false: the sim sees no MIDI ports (the live twin feeds it directly)
  audio = true,            // false: silent (the live twin: the real CrossPad is the one making sound)
  log = false,
  onLog = null,
} = {}) {
  if (!canvas) {
    canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:fixed;left:-10000px;top:0;pointer-events:none';
    document.body.append(canvas);
  }
  canvas.id = 'cpsim-canvas';
  canvas.width = lcd ? LCD_W : 490; canvas.height = lcd ? LCD_H : 714;
  canvas.tabIndex = 0;
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());

  const webMidi = await makeWebMidi(midi);
  const webAudio = makeWebAudio();
  const webSerial = makeWebSerial();
  const out = (s) => { if (log) console.log('[sim]', s); onLog?.(s); };

  const { default: CrossPadSim } = await import(`${base}CrossPad.mjs`);
  const M = await CrossPadSim({
    canvas, locateFile: (p) => base + p, print: out, printErr: out,
    arguments: lcd ? ['--lcd'] : [],
    webMidi, webAudio, webSerial,
    preRun: [(mod) => {
      // The card and the settings live in IndexedDB; the firmware starts
      // only once both are back in the file system.
      for (const dir of [PROFILE, SD]) { mkdirp(mod.FS, dir); mod.FS.mount(mod.IDBFS, {}, dir); }
      mod.addRunDependency('idbfs');
      mod.FS.syncfs(true, async (err) => {
        if (err) console.warn('[sim] IndexedDB', err);
        // The board's card goes in before the firmware starts (its kit
        // manager scans the card once, at init) — and again whenever the
        // seed changes; files the twin made itself stay.
        try { if (seed) await seedCard(mod.FS, seed, out); } catch (e) { console.warn('[sim] seed', e); }
        try { if (order) await cardOrder(mod.FS, order, out); } catch (e) { console.warn('[sim] order', e); }
        mod.removeRunDependency('idbfs');
      });
    }],
  });
  webMidi.M = webAudio.M = webSerial.M = M;

  // Written files reach IndexedDB a moment later, and when the page goes away.
  let syncing = false;
  const persist = () => new Promise((res) => {
    if (syncing) return res();
    syncing = true;
    M.FS.syncfs(false, (err) => { syncing = false; if (err) console.warn('[sim] IndexedDB', err); res(); });
  });
  setInterval(persist, 3000);
  addEventListener('pagehide', persist);

  // Browsers start audio only after a gesture on the page.
  if (audio) {
    const kick = () => webAudio.start();
    addEventListener('pointerdown', kick, { once: true, capture: true });
    addEventListener('keydown', kick, { once: true, capture: true });
  }

  let last = -1, twinBuf = null;
  const sim = {
    M, canvas, webMidi, webAudio, webSerial,
    pad: (i, vel) => M._wasm_pad(i, vel),                 // vel 0 = release
    encoder: (diff) => M._wasm_encoder(diff),             // LVGL enc_diff
    encoderPress: (down) => M._wasm_encoder_press(down ? 1 : 0),
    touch: (st, x, y) => M._wasm_touch(st, x, y),         // st 1 press, 2 drag, 0 release
    power: (down) => M._wasm_power(down ? 1 : 0),
    /** Follow the board: its running app ("-" = launcher), its launcher style (0 grid, 1 list, -1 unknown). */
    sync(app, launcher = -1) { const p = M.stringToNewUTF8(app || '-'); M._wasm_ui_sync(p, launcher); M._free(p); },
    app: () => M.UTF8ToString(M._wasm_ui_app()),
    /** Lockstep (platform-idf twin_mirror.h): one board frame, F0 7D 1F 09 … F7, as it arrived. */
    frame(bytes, at = performance.now()) {
      twinBuf ??= M._malloc(512);
      if (bytes.length > 512) return;
      M.HEAPU8.set(bytes, twinBuf);
      M._wasm_twin_frame(twinBuf, bytes.length, at);
    },
    /** The board's TWIN_STATE reply: compared (and, if it stays apart at rest, corrected) on the LVGL task. */
    check(reply) { const p = M.stringToNewUTF8(reply); M._wasm_twin_check(p); M._free(p); },
    locked: () => !!M._wasm_twin_locked(),
    /** The twin's settings stayed apart from the board's: fetch them (TWIN_SETTINGS) and hand them to settings(). */
    wantSettings: () => !!M._wasm_twin_want_settings(),
    /** The board's TWIN_INFO replies (platform, coprocessor, Info rows), one per line. */
    info(lines) { const p = M.stringToNewUTF8(lines.join('\n')); M._wasm_twin_info(p); M._free(p); },
    /** The board's TWIN_FW replies (the Firmware app's slots, library, USB mode), one per line. */
    fw(lines) { const p = M.stringToNewUTF8(lines.join('\n')); M._wasm_twin_fw(p); M._free(p); },
    settings(text) { const p = M.stringToNewUTF8(text); M._wasm_twin_settings(p); M._free(p); },
    twinState: () => M.UTF8ToString(M._wasm_twin_state()),
    twinStats: () => JSON.parse(M.UTF8ToString(M._wasm_twin_stats())),
    W: LCD_W, H: LCD_H,
    /** RGBA view of the LCD when it changed since the last call, else null. */
    lcd(force = false) {
      const f = M._wasm_lcd_frame();
      if (f === last && !force) return null;
      const p = M._wasm_lcd_rgba();
      if (!p) return null;
      last = f;
      return M.HEAPU8.subarray(p, p + LCD_W * LCD_H * 4);
    },
    persist,
    /** The card as a .zip (Blob). */
    async exportSd() {
      const { zipSync } = await fflate();
      const files = {};
      walk(M.FS, SD, (path) => { files[path.slice(SD.length + 1)] = M.FS.readFile(path); });
      return new Blob([zipSync(files, { level: 0 })], { type: 'application/zip' });
    },
    /** Unpacks a .zip onto the card (merging; same names are replaced). Returns the file count. */
    async importSd(blob) {
      const { unzipSync } = await fflate();
      const entries = unzipSync(new Uint8Array(await blob.arrayBuffer()));
      let n = 0;
      for (const [name, data] of Object.entries(entries)) {
        if (name.endsWith('/') || name.includes('..') || name.startsWith('__MACOSX')) continue;
        const path = `${SD}/${name}`;
        mkdirp(M.FS, path.slice(0, path.lastIndexOf('/')));
        M.FS.writeFile(path, data);
        n++;
      }
      await persist();
      return n;
    },
    /** Empties the card. */
    async wipeSd() {
      for (const name of M.FS.readdir(SD)) if (name !== '.' && name !== '..') rmrf(M.FS, `${SD}/${name}`);
      await persist();
    },
    /** Forgets the settings (as a fresh board); takes effect on reload. */
    async resetSettings() {
      for (const name of M.FS.readdir(PROFILE)) if (name !== '.' && name !== '..') rmrf(M.FS, `${PROFILE}/${name}`);
      await persist();
    },
    sdUsage() {
      let files = 0, bytes = 0;
      walk(M.FS, SD, (path) => { files++; bytes += M.FS.stat(path).size; });
      return { files, bytes };
    },
  };
  return sim;
}

async function seedCard(FS, url, log) {
  const meta = await (await fetch(url, { cache: 'no-cache' })).json();
  const mark = `${SD}/.twin-seed`;
  let have = '';
  try { have = new TextDecoder().decode(FS.readFile(mark)); } catch {}
  if (have === meta.version) return;
  log?.(`[twin] loading the board's card (${meta.files} files, ${(meta.bytes / 1048576).toFixed(0)} MB)`);
  const zip = new Uint8Array(await (await fetch(new URL(url.replace(/\.json(\?.*)?$/, '.zip'), location.href))).arrayBuffer());
  const { unzipSync } = await fflate();
  for (const [name, data] of Object.entries(unzipSync(zip))) {
    if (name.endsWith('/') || name.includes('..')) continue;
    const path = `${SD}/${name.replace(/^\.\//, '')}`;
    mkdirp(FS, path.slice(0, path.lastIndexOf('/')));
    FS.writeFile(path, data);
  }
  FS.writeFile(mark, meta.version);
  await new Promise((res) => FS.syncfs(false, () => res()));
}

// FAT lists a directory in the order its entries were written, and the
// firmware shows unsorted listings as they come (the kit list above all);
// IndexedDB has no such order. Each directory the board listed reads back in
// the board's order, whatever it does not know after it.
async function cardOrder(FS, url, log) {
  const order = await (await fetch(url, { cache: 'no-cache' })).json();
  let n = 0;
  for (const [dir, names] of Object.entries(order)) {
    try { FS.lookupPath(SD + (dir === '/' ? '' : dir)).node.twinOrder = names; n++; } catch { /* not on this card */ }
  }
  const ops = FS.lookupPath(SD).node.node_ops;           // MEMFS: one table for every directory
  if (!ops.twinOrdered) {
    const readdir = ops.readdir;
    ops.readdir = (node) => {
      const all = readdir(node);
      if (!node.twinOrder) return all;
      const rest = new Set(all.slice(2)), out = ['.', '..'];
      for (const k of node.twinOrder) if (rest.delete(k)) out.push(k);
      return out.concat([...rest]);
    };
    ops.twinOrdered = true;
  }
  log?.(`[twin] ${n} directories in the board's order`);
}

// ── Web MIDI ─────────────────────────────────────────────────────────────
async function makeWebMidi(enabled) {
  const w = {
    M: null, access: null, ins: [], outs: [], opened: new Set(),
    inputs: () => w.ins, outputs: () => w.outs,
    open(out, i, open) {
      if (out) return;                                  // outputs need no opening
      const port = w.ins[i];
      if (!port) return;
      if (open) {
        w.opened.add(port.id);
        port.onmidimessage = (e) => {
          const M = w.M, d = e.data;
          if (!M || d.length > 4096) return;
          M.HEAPU8.set(d, M._wasm_midi_buf());
          M._wasm_midi_in(w.ins.indexOf(port), d.length, e.timeStamp);
        };
      } else {
        w.opened.delete(port.id);
        port.onmidimessage = null;
      }
    },
    send(i, bytes) { try { w.outs[i]?.send(bytes); } catch (e) { console.warn('[sim] MIDI send', e); } },
  };
  if (!enabled || !navigator.requestMIDIAccess) return w;
  try {
    w.access = await navigator.requestMIDIAccess({ sysex: true }).catch(() => navigator.requestMIDIAccess());
  } catch (e) {
    console.warn('[sim] no Web MIDI:', e.message);
    return w;
  }
  // Port lists grow at the end, so the indices the firmware holds stay valid
  // while devices come and go; a port that left keeps its slot, disconnected.
  const refresh = () => {
    for (const p of w.access.inputs.values()) if (!w.ins.includes(p)) w.ins.push(p);
    for (const p of w.access.outputs.values()) if (!w.outs.includes(p)) w.outs.push(p);
  };
  refresh();
  w.access.onstatechange = refresh;
  return w;
}

// ── WebAudio ─────────────────────────────────────────────────────────────
function makeWebAudio() {
  const FRAMES = 1024;
  const a = {
    M: null, ctx: null, node: null, micEnabled: false, mic: null, gain: null,
    async start() {
      if (!a.M) return;
      if (a.ctx) { if (a.ctx.state !== 'running') await a.ctx.resume(); return; }
      a.ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
      // ScriptProcessor runs on the page's thread, which is where the firmware
      // lives: each block asks it for exactly this many frames.
      a.node = a.ctx.createScriptProcessor(FRAMES, 2, 2);
      a.gain = a.ctx.createGain();
      a.node.onaudioprocess = (e) => {
        const M = a.M;
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
        let inPtr = 0;
        if (a.micEnabled && a.mic) {
          const il = e.inputBuffer.getChannelData(0), ir = e.inputBuffer.numberOfChannels > 1 ? e.inputBuffer.getChannelData(1) : il;
          inPtr = M._wasm_audio_in_buf(FRAMES);
          const inb = M.HEAPF32.subarray(inPtr >> 2, (inPtr >> 2) + FRAMES * 2);
          for (let i = 0; i < FRAMES; i++) { inb[2 * i] = il[i]; inb[2 * i + 1] = ir[i]; }
        }
        const p = M._wasm_audio_render(FRAMES, inPtr);
        if (!p) { L.fill(0); R.fill(0); return; }
        const buf = M.HEAPF32.subarray(p >> 2, (p >> 2) + FRAMES * 2);
        for (let i = 0; i < FRAMES; i++) { L[i] = buf[2 * i]; R[i] = buf[2 * i + 1]; }
      };
      a.node.connect(a.gain).connect(a.ctx.destination);
      // a ScriptProcessor only runs with something on its input
      const silent = a.ctx.createConstantSource(); silent.offset.value = 0; silent.connect(a.node); silent.start();
      await a.ctx.resume();
    },
    /** The microphone as the firmware's audio input (asks for permission). */
    async enableMic() {
      await a.start();
      const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
      a.mic = a.ctx.createMediaStreamSource(stream);
      a.mic.connect(a.node);
      a.micEnabled = true;
    },
    setVolume(v) { if (a.gain) a.gain.gain.value = v; },
    get running() { return a.ctx?.state === 'running'; },
  };
  return a;
}

// ── Web Serial ───────────────────────────────────────────────────────────
function makeWebSerial() {
  const s = {
    M: null, list: [], cur: null, reader: null, writer: null,
    ports: () => s.list,
    /** Asks the user for a port (needs a click); it then shows up in the firmware's list. */
    async request(filters = []) {
      const port = await navigator.serial.requestPort({ filters });
      return s.add(port);
    },
    add(port) {
      let e = s.list.find((x) => x.port === port);
      if (!e) {
        const info = port.getInfo();
        e = { port, vid: info.usbVendorId || 0, pid: info.usbProductId || 0 };
        e.name = `Web Serial ${s.list.length + 1}` + (e.vid ? ` (${hex4(e.vid)}:${hex4(e.pid)})` : '');
        s.list.push(e);
      }
      return e;
    },
    open(i, baud) {
      const e = s.list[i];
      if (!e) return false;
      s.close();
      s.cur = e;
      (async () => {
        try {
          await e.port.open({ baudRate: baud || 115200 });
          s.writer = e.port.writable.getWriter();
          s.reader = e.port.readable.getReader();
          for (;;) {
            const { value, done } = await s.reader.read();
            if (done) break;
            const M = s.M;
            for (let o = 0; o < value.length; o += 4096) {
              const chunk = value.subarray(o, o + 4096);
              M.HEAPU8.set(chunk, M._wasm_uart_buf());
              M._wasm_uart_rx(chunk.length);
            }
          }
        } catch (err) {
          console.warn('[sim] serial', err.message);
        }
        if (s.cur === e) { s.M?._wasm_uart_lost(); s.cur = null; }
      })();
      return true;
    },
    write(bytes) {
      if (!s.writer) return -1;
      s.writer.write(bytes).catch(() => {});
      return bytes.length;
    },
    close() {
      const e = s.cur;
      s.cur = null;
      const r = s.reader, w = s.writer;
      s.reader = s.writer = null;
      (async () => {
        try { await r?.cancel(); } catch {}
        try { r?.releaseLock(); } catch {}
        try { w?.releaseLock(); } catch {}
        try { await e?.port.close(); } catch {}
      })();
    },
  };
  if (navigator.serial) {
    navigator.serial.getPorts().then((ps) => ps.forEach((p) => s.add(p)));
    navigator.serial.addEventListener('connect', (ev) => s.add(ev.target));
  }
  return s;
}

// ── helpers ──────────────────────────────────────────────────────────────
const hex4 = (n) => n.toString(16).padStart(4, '0');
let fflateP = null;
const fflate = () => (fflateP ??= import('https://cdn.jsdelivr.net/npm/fflate@0.8.2/esm/browser.js'));

function mkdirp(FS, dir) {
  let cur = '';
  for (const part of dir.split('/').filter(Boolean)) {
    cur += `/${part}`;
    try { FS.mkdir(cur); } catch {}
  }
}
function walk(FS, dir, fn) {
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const path = `${dir}/${name}`;
    if (FS.isDir(FS.stat(path).mode)) walk(FS, path, fn); else fn(path);
  }
}
function rmrf(FS, path) {
  if (FS.isDir(FS.stat(path).mode)) {
    for (const name of FS.readdir(path)) if (name !== '.' && name !== '..') rmrf(FS, `${path}/${name}`);
    FS.rmdir(path);
  } else FS.unlink(path);
}
