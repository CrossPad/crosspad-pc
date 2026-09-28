// The link between a real CrossPad on Web MIDI and its browser twin
// (simtwin.js): the board's firmware sends what its LVGL read, stamped with
// its clock (platform-idf twin_mirror.h, SysEx F0 7D 1F 09 ...), and the twin
// replays exactly that -- same firmware, same inputs, same times, same screen.
// Nothing of the screen travels.
//
//   const link = linkBoard(sim, { onApp });
//   input.onmidimessage = (e) => link.onMessage(e.data);   // every message from the board's port
//   link.setOutput(outputPort);                            // the board's own MIDI out port
//
// What it keeps going while an output is set:
//   * TWIN 1 every 2 s (companion notify): the board mirrors only while asked;
//   * TWIN_STATE every second (and at once when the board says its telemetry
//     moved, kind 8): the safety net compares the board's description of its
//     screen with the twin's own (twin_state.h, the same code on both sides);
//   * once: TWIN_INFO (Settings' board-only rows) and TWIN_FW (the Firmware
//     app's slots and library); TWIN_SETTINGS whenever the settings drift.
// request(verb) is the one companion channel -- a page that asks the board
// anything else (PAD_NOTES, UI_STATE) goes through it too, or the replies
// cross.

const CP = [0xf0, 0x7d, 0x1f];
const bytes = (text) => [...text].map((c) => c.charCodeAt(0) & 0x7f);

export function linkBoard(sim, { onApp = null, log = null } = {}) {
  const L = {
    out: null, busy: null, buf: '', queue: Promise.resolve(),
    lastFrame: -1e9, infoTaken: false, fwTaken: false, checking: false,
  };
  const locked = () => performance.now() - L.lastFrame < 1000;

  /** A companion verb; resolves to the reply text, or null (no board, busy, 1.5 s). */
  function request(verb) {
    const run = () => new Promise((res) => {
      if (!L.out) return res(null);
      const timer = setTimeout(() => { L.busy = null; res(null); }, 1500);
      L.busy = (text) => { clearTimeout(timer); L.busy = null; res(text); };
      L.buf = '';
      try { L.out.send([...CP, 0x01, ...bytes(verb), 0xf7]); }
      catch { clearTimeout(timer); L.busy = null; res(null); }
    });
    const p = L.queue.then(run);
    L.queue = p.catch(() => null);
    return p;
  }
  function keepalive() {
    try { L.out?.send([...CP, 0x04, ...bytes('TWIN 1'), 0xf7]); } catch { /* the port went away */ }
  }
  /** Rows of a paged verb: "<verb> <first>" says how many there are. */
  async function rows(head, verb, countRe) {
    const lines = [];
    for (const h of head) lines.push(await request(h));
    const first = await request(`${verb} 0`);
    const n = first ? +(countRe.exec(first) || [0, 0])[1] : 0;
    if (first) lines.push(first);
    for (let i = 1; i < n; i++) lines.push(await request(`${verb} ${i}`));
    return lines.every(Boolean) ? lines : null;
  }
  async function check() {
    if (!locked() || !L.out || L.checking) return;
    L.checking = true;
    try {
      if (!L.infoTaken) {
        const lines = await rows(['TWIN_INFO P', 'TWIN_INFO C'], 'TWIN_INFO', /^TWIN_INFO (\d+)/);
        if (lines) { sim.info(lines); L.infoTaken = true; }
      }
      if (!L.fwTaken) {
        const lines = await rows(['TWIN_FW M', 'TWIN_FW S 0', 'TWIN_FW S 1'], 'TWIN_FW L', /^TWIN_FW L (\d+)/);
        if (lines) { sim.fw(lines); L.fwTaken = true; }
      }
      const st = await request('TWIN_STATE');
      if (st?.startsWith('TWIN_STATE')) {
        sim.check(st);
        const app = /\bapp=(\S+)/.exec(st);
        if (app) onApp?.(app[1]);
      }
      if (sim.wantSettings()) {                        // the settings as the board's NVS holds them
        let text = '', total = 1;
        while (text.length < total) {
          const r = await request(`TWIN_SETTINGS ${text.length}`);
          const m = r && /^TWIN_SETTINGS (\d+) (\d+) ?(.*)$/s.exec(r);
          if (!m || !m[3]) break;
          total = +m[1]; text += m[3];
        }
        sim.settings(text.length === total ? text : '');
        log?.(`[link] settings from the board (${text.length} bytes)`);
      }
    } finally {
      L.checking = false;
    }
  }

  /** Feed every message from the board's port; true when it was the link's. */
  function onMessage(D) {
    if (D[0] !== 0xf0 || D[1] !== 0x7d || D[2] !== 0x1f) return false;
    if (D[3] === 0x09) {
      L.lastFrame = performance.now();
      sim.frame(D);
      if (D[4] === 8) setTimeout(check, 0);           // the board's telemetry moved: the rest too
      return true;
    }
    if (D[3] === 0x02 || D[3] === 0x03) {             // a companion reply, maybe in parts
      L.buf += String.fromCharCode(...D.slice(4, D.length - 1));
      if (D[3] === 0x03) L.busy?.(L.buf.trim());
      return true;
    }
    return false;
  }

  const timers = [setInterval(keepalive, 2000), setInterval(check, 1000)];
  return {
    onMessage, request, locked,
    /** The board's own output port (null: none). Asks for the mirror at once. */
    setOutput(port) { L.out = port; if (port) keepalive(); },
    stop() { timers.forEach(clearInterval); L.out = null; },
  };
}

/** The ports a CrossPad shows up with: the ESP's own ("Crosspad MIDI 1"), not the STM bridge's "MIDI+Serial". */
export const isBoardPort = (port) => /^crosspad midi(?!\+)/i.test(port.name.trim());
