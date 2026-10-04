/* battery-ui-jxa.js — JavaScriptCore (osascript) variant of battery-ui.cjs
 * for hosts without Node/Playwright. It loads the REAL app.js, stubs only
 * $()/Date.now and the page globals onTelemetry touches, then replays the
 * same scenarios: debounce regression PLUS charge recovery and the
 * vehicle-restart (uptime regression) reset.
 * Run from the repository root:  osascript -l JavaScript esp32c6_car/test/web/battery-ui-jxa.js
 */
ObjC.import('Foundation');
const bridge = $; // JXA ObjC bridge — the page's $() stub shadows it below

(function () {
function fail(msg) { throw new Error('FAIL: ' + msg); }
function readFile(path) {
  const err = Ref();
  const s = bridge.NSString.stringWithContentsOfFileEncodingError(path, bridge.NSUTF8StringEncoding, err);
  if (s.isNil()) fail('cannot read ' + path);
  return s.js;
}

// locate app.js relative to the repo root (cwd) or this script's own position
function appJsPath() {
  const candidates = [
    'esp32c6_car/assets_src/app.js',
    'assets_src/app.js',
  ];
  for (const c of candidates) {
    if (!bridge.NSFileManager.defaultManager.fileExistsAtPath(c)) continue;
    return c;
  }
  // fall back to argv: osascript puts the script path somewhere in there
  const raw = ObjC.deepUnwrap(bridge.NSProcessInfo.processInfo.arguments) || [];
  const self = raw.find(a => typeof a === 'string' && a.includes('battery-ui-jxa'));
  if (self) return self.replace(/test\/web\/battery-ui-jxa\.js$/, 'assets_src/app.js');
  fail('cannot locate assets_src/app.js — run from the repository root');
}
const src = readFile(appJsPath());

// whole-file syntax gate: parsing the full page catches typos anywhere
try { new Function(src); } catch (e) { fail('app.js syntax: ' + e.message); }

const start = src.indexOf('/* Battery display only');
const end = src.indexOf('/* ---- speedometer');
if (start < 0 || end < 0 || end <= start) fail('battery section markers not found');
const section = src.slice(start, end);

// ---- browser stubs -----------------------------------------------------------
const elements = {};
function $(id) {
  if (!elements[id]) elements[id] = { textContent: '', style: {} };
  return elements[id];
}
// fresh display state per scenario
function freshSection() {
  return new Function('$', section + '\n; return {rv: renderBatteryVoltage, rp: renderBatteryPercent, guard: batteryUptimeGuard};')($);
}

// ---- scenario driver (mirrors battery-ui.cjs) ---------------------------------
const realNow = Date.now;
let now = 100000;
Date.now = function () { return now; };

function frame(mv, pct, fault, uptime) {
  const t = new Uint8Array(38);
  t[19] = mv & 255; t[20] = (mv >> 8) & 255; t[21] = pct;
  t[9] = fault & 255; t[10] = (fault >> 8) & 255;
  if (uptime !== undefined) {
    t[4] = uptime & 255; t[5] = (uptime >> 8) & 255;
    t[6] = (uptime >> 16) & 255; t[7] = (uptime >> 24) & 255;
  }
  return t;
}
// route frames through the REAL onTelemetry together with the battery section
// in one scope (onTelemetry calls batteryUptimeGuard), page globals stubbed
const teleIdx = src.indexOf('function onTelemetry(p)');
const teleEnd = src.indexOf('function setBar(');
if (teleIdx < 0 || teleEnd <= teleIdx) fail('onTelemetry markers not found');
function freshCombined() {
  return new Function('$',
    'const TELEMETRY_LEN=38; const state={tc:true};' +
    'function renderSpeed(){} function setBar(){} function setState(){};' +
    section + '\n' + src.slice(teleIdx, teleEnd) +
    '\nreturn {rv: renderBatteryVoltage, rp: renderBatteryPercent, tele: onTelemetry};')($);
}

// ---- voltage scenarios ----------------------------------------------------------
{
  const m = freshSection();
  const V = () => $('batt_v').textContent;
  now = 100000;
  m.rv(0);
  if (V() !== '') fail('voltage: zero measurement initialized');
  m.rv(8400);
  if (V() !== '8.40V') fail('voltage: first sample not initialized, got ' + V());
  for (let i = 0; i < 500; i++) { now += 20; m.rv(i % 2 ? 8410 : 8390); }
  if (V() !== '8.40V') fail('voltage: jitter changed display');
  m.rv(7000); now += 20;
  for (let i = 0; i < 100; i++) { now += 20; m.rv(8400); }
  if (V() !== '8.40V') fail('voltage: single low spike latched');
  for (let i = 0; i < 150; i++) { now += 20; m.rv(8000); }
  const lowered = parseFloat(V());
  if (!(lowered <= 8.03 && lowered >= 8)) fail('voltage: sustained decline not followed, got ' + V());
  m.rv(8500); now += 20; m.rv(8500); now += 20;
  for (let i = 0; i < 50; i++) { now += 20; m.rv(8000); }
  if (parseFloat(V()) !== lowered) fail('voltage: short charge blip raised display');
  for (let i = 0; i < 150; i++) { now += 20; m.rv(8500); }
  const charged = parseFloat(V());
  if (charged < lowered + 0.05) fail('voltage: sustained charge not followed, got ' + V());
  for (let i = 0; i < 100; i++) { now += 20; m.rv(0); }
  if (parseFloat(V()) !== charged) fail('voltage: missing sample changed display');
  now += 10000;
  for (let i = 0; i < 5; i++) { now += 20; m.rv(8500); }
  if (parseFloat(V()) !== charged) fail('voltage: gap alone changed display');
  console.log('PASS voltage: jitter/spike/blip rejection, decline+charge followed, gap stability');
}

// ---- percent scenarios ----------------------------------------------------------
{
  const m = freshSection();
  const P = () => $('batt_pct').textContent;
  now = 100000;
  m.rp(0, 0);
  if (P() !== '') fail('percent: initialized from missing voltage');
  m.rp(50, 8000);
  if (P() !== '50%') fail('percent: first sample not seeded, got ' + P());
  for (let i = 0; i < 500; i++) { now += 20; m.rp(i % 2 ? 50 : 49, 8000); }
  if (P() !== '50%') fail('percent: 1% boundary noise changed display');
  m.rp(0, 8000); now += 20;
  for (let i = 0; i < 100; i++) { now += 20; m.rp(50, 8000); }
  if (P() !== '50%') fail('percent: isolated low latched');
  for (let i = 0; i < 50; i++) { now += 20; m.rp(49, 8000); }
  if (P() !== '50%') fail('percent: drop without confirmation');
  for (let i = 0; i < 50; i++) { now += 20; m.rp(49, 8000); }
  if (P() !== '49%') fail('percent: sustained 1% decline ignored');
  for (let i = 0; i < 150; i++) { now += 20; m.rp(60, 8000); }
  if (P() !== '49%') fail('percent: short rise blip displayed before confirmation');
  for (let i = 0; i < 100; i++) { now += 20; m.rp(49, 8000); }
  m.rp(255, 8000); now += 20;
  for (let i = 0; i < 100; i++) { now += 20; m.rp(0, 0); }
  if (P() !== '49%') fail('percent: invalid samples changed display');
  for (let i = 0; i < 300; i++) { now += 20; m.rp(50, 8000); }
  if (P() !== '49%') fail('percent: sub-hysteresis rise displayed');
  for (let i = 0; i < 600; i++) { now += 20; m.rp(60, 8000); }
  if (P() !== '60%') fail('percent: sustained charge rise not confirmed, got ' + P());
  for (let i = 0; i < 520; i++) { now += 20; m.rp(100, 8400); }
  if (P() !== '100%') fail('percent: charge recovery did not reach full, got ' + P());
  for (let i = 0; i < 25; i++) { now += 20; m.rp(48, 8000); }
  now += 10000; m.rp(48, 8000);
  if (P() !== '100%') fail('percent: missing telemetry counted as confirmation');
  m.rp(5, 8000);
  if (P() !== '100%') fail('percent: single low bypassed confirmation');
  for (let i = 0; i < 200; i++) { now += 20; m.rp(0, 8000); }
  if (P() !== '0%') fail('percent: valid empty battery rejected, got ' + P());
  console.log('PASS percent: dip/spike/blip/sub-hysteresis rejection, 10s rise to 100%, gap, valid 0%');
}

// ---- reboot via the real onTelemetry wiring --------------------------------------
{
  const m = freshCombined();
  now = 100000;
  m.tele(frame(7500, 50, 0, 100000));
  if ($('batt_pct').textContent !== '50%') fail('reboot: drained session not seeded, got ' + $('batt_pct').textContent);
  m.tele(frame(8400, 100, 0, 3000)); // uptime regression = vehicle restart
  if ($('batt_pct').textContent !== '100%') fail('reboot: percent latch not reset, got ' + $('batt_pct').textContent);
  if ($('batt_v').textContent !== '8.40V') fail('reboot: voltage latch not reset, got ' + $('batt_v').textContent);
  m.tele(frame(8400, 100, 0, 3200));
  if ($('batt_pct').textContent !== '100%' || $('batt_v').textContent !== '8.40V') fail('reboot: post-reboot frames unstable');
  console.log('PASS reboot: uptime regression via onTelemetry(d32(4)) resets both planes and re-seeds');
}

Date.now = realNow;
console.log('ALL PASS');
})();
