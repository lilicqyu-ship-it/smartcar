const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

(async () => {
  const project = path.resolve(__dirname, '../..');
  const packed = fs.readFileSync(path.join(project, 'build/assets.bin'));
  const assets = new Map();
  for (let i = 0; i < packed.readUInt16LE(8); i++) {
    const entry = 16 + i * 32;
    const name = packed.subarray(entry, entry + 16).toString().replace(/\0.*$/, '');
    const offset = packed.readUInt32LE(entry + 16);
    const length = packed.readUInt32LE(entry + 20);
    assets.set('/' + name, zlib.gunzipSync(packed.subarray(offset, offset + length)));
  }
  const server = fs.readFileSync(path.join(project, 'components/c6_http/http_server.c'), 'utf8');
  const routes = new Set([...server.matchAll(/\.uri\s*=\s*"([^"]+)"[^\n]*\.handler\s*=\s*assets_handler/g)].map(match => match[1]));
  const browser = await chromium.launch({ headless: true, ...(process.env.CHROME_PATH ? { executablePath: process.env.CHROME_PATH } : {}) });
  try {
    for (const javaScriptEnabled of [true, false]) {
      const context = await browser.newContext({ viewport: { width: 375, height: 667 }, hasTouch: true, isMobile: true, javaScriptEnabled });
      const page = await context.newPage();
      const errors = [], missing = [], frames = [];
      page.on('pageerror', error => errors.push(error.message));
      await page.route('http://c6.test/**', route => {
        const uri = new URL(route.request().url()).pathname;
        if (uri === '/api/health') return route.fulfill({ json: { ver: 'preview' } });
        if (!routes.has(uri)) { missing.push(uri); return route.fulfill({ status: 404, body: 'Not found' }); }
        const name = uri === '/' ? '/index.html' : uri;
        const body = assets.get(name);
        if (!body) { missing.push(name); return route.fulfill({ status: 404, body: 'Not found' }); }
        const contentType = name.endsWith('.css') ? 'text/css' : name.endsWith('.js') ? 'text/javascript' : name.endsWith('.svg') ? 'image/svg+xml' : 'text/html';
        return route.fulfill({ body, contentType });
      });
      await page.routeWebSocket('ws://c6.test/ws', ws => {
        ws.onMessage(data => { if (typeof data !== 'string') frames.push(Array.from(data)); });
        ws.send(JSON.stringify({ t: 'hello', role: 'ctrl', ver: 'preview' }));
        ws.send(JSON.stringify({ t: 'tc', on: true }));
      });
      await page.goto('http://c6.test/calib.html');
      const link = page.locator('#back_drive');
      const bounds = await link.boundingBox();
      if (bounds.width < 44 || bounds.height < 44) throw Error('Driving link touch target too small');
      if (javaScriptEnabled) await page.evaluate(() => { document.getElementById('ck_airborne').checked = true; jogStart(0, 1); });
      await link.tap();
      await page.waitForURL('http://c6.test/index.html');
      await page.locator('#joy').waitFor({ state: 'visible' });
      if (javaScriptEnabled && !frames.some(frame => frame[3] === 0x71 && frame[6] === 0 && frame[7] === 0 && frame[8] === 0)) throw Error('Leaving did not stop the active jog');
      if (missing.length) throw Error('Assets not served by firmware: ' + missing.join(', '));
      if (errors.length) throw Error(errors.join('\n'));
      await context.close();
    }
    console.log('PASS: packed assets match C6 routes, SE2 touch navigation, active jog stop, native navigation without JavaScript');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });
