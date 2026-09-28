// The twin against the real board: drive the board over CDC (HIL verbs that go
// through the same read paths as a hand), compare its SCREENSHOT with the
// twin's LCD and its TWIN_STATE with the twin's own, after every step.
//
//   node tools/twin/test/lock.cjs [scenario.cjs]
//   env: TWIN_URL   the twin page (web/twin.html, served over http)
//        TWIN_PORT  the board's CDC port (/dev/ttyACM2)
//        OUT        where screenshots of a difference and the logs go (this dir)
//        PLAYWRIGHT the playwright module to load, CHROME the browser binary
const { chromium } = require(process.env.PLAYWRIGHT || 'playwright');
const { spawn } = require('child_process');
const fs = require('fs'), zlib = require('zlib');
const S = process.env.OUT || __dirname;   // screenshots, logs
const HERE = __dirname;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const LOG = fs.createWriteStream(`${S}/run.log`), PLOG = fs.createWriteStream(`${S}/page.log`);
const _log = console.log; console.log = (...a) => { _log(...a); LOG.write(a.join(' ') + '\n'); };
const SB = 24;                         // status bar rows (board-only icons, clock): scored apart
const helper = spawn(process.env.TWIN_PY || 'python3', ['-u', `${HERE}/cdcd.py`]);
let hbuf = '', hwait = [];
helper.stdout.on('data', (d) => { hbuf += d; let i; while ((i = hbuf.indexOf('\x00END\n')) >= 0) { const r = hbuf.slice(0, i).trim(); hbuf = hbuf.slice(i + 5); hwait.shift()(r); } });
helper.stderr.on('data', (d) => process.stderr.write(d));
const board = (c) => Promise.race([new Promise((res) => { hwait.push(res); helper.stdin.write(c + '\n'); }), sleep(40000).then(() => { throw new Error('board helper stuck on ' + c); })]);

function bmp(file) {
  const b = fs.readFileSync(file), off = b.readUInt32LE(10), w = b.readInt32LE(18), h = b.readInt32LE(22);
  const row = (w * 3 + 3) & ~3, out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const s = off + (h - 1 - y) * row + x * 3, d = (y * w + x) * 4;
    out[d] = b[s + 2]; out[d + 1] = b[s + 1]; out[d + 2] = b[s]; out[d + 3] = 255;
  }
  return { w, h, px: out };
}
function png(file, w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 4 + 1)] = 0; Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1); }
  const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
  const crc = (buf) => { let c = 0xffffffff; for (const x of buf) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

let page, n = 0;
const results = [];
async function compare(label, settle = 1200) {
  await sleep(settle);
  const [bs, shot] = [await board('TWIN_STATE'), await board('SCREENSHOT')];
  if (/top=missing/.test(shot)) console.log('   (board screenshot: top layer left out, no PSRAM for it)');
  const tw = await Promise.race([
    page.evaluate(() => ({ st: window.__sim.twinState(), px: Array.from(window.__sim.lcd(true)), stats: window.__sim.twinStats() })),
    sleep(20000).then(() => { throw new Error('twin page unresponsive (20 s)'); })]);
  await board(`!pull /sdcard/crosspad/screen.bmp ${S}/b.bmp`);
  const b = bmp(`${S}/b.bmp`), t = Uint8Array.from(tw.px);
  let dTop = 0, dBody = 0;
  const diff = new Uint8Array(320 * 240 * 4);
  for (let i = 0; i < 320 * 240; i++) {
    const d = Math.max(Math.abs(b.px[4 * i] - t[4 * i]), Math.abs(b.px[4 * i + 1] - t[4 * i + 1]), Math.abs(b.px[4 * i + 2] - t[4 * i + 2]));
    if (d > 40) { if (i / 320 < SB) dTop++; else dBody++; diff[4 * i] = 255; }
    diff[4 * i + 3] = 255;
  }
  const strip = (s) => s.replace(/^TWIN_STATE tick=\d+ /, '').replace(/ geo=\w+/, '').replace(/m\d+p\d+(?= |$)/, '').replace(/ host=\S+/, '').replace(/st=b\d+/, 'st=').trim();
  const same = strip(bs) === strip(tw.st);
  const ok = same && dBody === 0;
  n++;
  const tag = String(n).padStart(2, '0');
  if (!ok || process.env.SAVE) {
    const side = new Uint8Array(960 * 240 * 4);
    for (let y = 0; y < 240; y++) for (let x = 0; x < 320; x++) for (let c = 0; c < 4; c++) {
      side[(y * 960 + x) * 4 + c] = b.px[(y * 320 + x) * 4 + c];
      side[(y * 960 + 320 + x) * 4 + c] = t[(y * 320 + x) * 4 + c];
      side[(y * 960 + 640 + x) * 4 + c] = diff[(y * 320 + x) * 4 + c];
    }
    png(`${S}/cmp_${tag}.png`, 960, 240, side);
  }
  const geo = (/geo=(\w+)/.exec(bs) || [])[1] === (/geo=(\w+)/.exec(tw.st) || [])[1];
  results.push({ tag, label, ok, same, geo, dBody, dTop });
  console.log(`${ok ? 'OK  ' : 'DIFF'} ${tag} ${label.padEnd(34)} body=${dBody} bar=${dTop} state=${same ? '=' : '≠'} geo=${geo ? '=' : '≠'}`);
  if (!same) { console.log('   board:', strip(bs)); console.log('   twin :', strip(tw.st)); }
  return ok;
}
const touch = (...pts) => board(`TOUCH_INJECT ${pts.join(' ')}`);

(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || '/usr/bin/google-chrome', args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 720 } });
  const url = process.env.TWIN_URL || 'http://127.0.0.1:8931/crosspad-pc/web/twin.html?log';
  await ctx.grantPermissions(['midi', 'midi-sysex'], { origin: new URL(url).origin });
  page = await ctx.newPage();
  page.on('pageerror', (e) => console.error('[pageerror]', e.message));
  page.on('console', (m) => { const t = m.text(); PLOG.write(t + '\n'); if (/\[twin\]|error/i.test(t)) console.log('   [page]', t.slice(0, 300)); });
  await page.goto(url);
  await page.waitForFunction(() => window.__sim?.locked(), null, { timeout: 180000 });
  console.log('lockstep:', JSON.stringify(await page.evaluate(() => window.__sim.twinStats())));
  // The page opened on a board already somewhere: the safety net brings the twin there.
  const strip0 = (x) => x.replace(/^TWIN_STATE tick=\d+ /, '').replace(/ geo=\w+/, '').replace(/m\d+p\d+(?= |$)/, '').replace(/ host=\S+/, '').replace(/st=b\d+/, 'st=').trim();
  const t0 = Date.now();
  let conv = false;
  while (Date.now() - t0 < 20000) {
    const b = strip0(await board('TWIN_STATE')), t = strip0(await page.evaluate(() => window.__sim.twinState()));
    if (b === t) { conv = true; break; }
    await sleep(500);
  }
  console.log(conv ? `converged in ${((Date.now() - t0) / 1000).toFixed(1)} s` : 'did NOT converge in 20 s');
  const scen = require(process.argv[2] ? require('path').resolve(process.argv[2]) : `${HERE}/scen_smoke.cjs`);
  await scen({ board, touch, compare, sleep, page });
  const stats = await page.evaluate(() => window.__sim.twinStats());
  console.log('stats:', JSON.stringify(stats));
  const bad = results.filter((r) => !r.ok);
  console.log(`\n${results.length - bad.length}/${results.length} identical (state + pixels below the status bar)`);
  fs.writeFileSync(`${S}/results.json`, JSON.stringify({ results, stats }, null, 1));
  await browser.close(); helper.stdin.end();
  process.exit(bad.length ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
