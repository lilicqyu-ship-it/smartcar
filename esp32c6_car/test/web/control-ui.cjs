const {chromium}=require('playwright');
const fs=require('fs');
const pathModule=require('path');
(async()=>{
 const browser=await chromium.launch({headless:true, ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
 const page=await browser.newPage(); const errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 await page.route('http://c6.test/**',route=>{
 const path=new URL(route.request().url()).pathname;
 if(path==='/api/health')return route.fulfill({json:{ver:'preview'}});
 if(path.startsWith('/api/')) return route.fulfill({json:{ok:false}});
 const f=pathModule.resolve(__dirname, '../../assets_src', path==='/'?'index.html':path.slice(1));
 route.fulfill({body:fs.readFileSync(f),contentType:path.endsWith('.css')?'text/css':path.endsWith('.js')?'text/javascript':path.endsWith('.svg')?'image/svg+xml':'text/html'});
 });
 await page.routeWebSocket('ws://c6.test/ws',ws=>{ws.onMessage(()=>{});ws.send(JSON.stringify({t:'hello',role:'ctrl',ver:'preview'}));ws.send(JSON.stringify({t:'tc',on:true}));});
 await page.goto('http://c6.test/');
 for(const width of [1440,390,375,320]){
 await page.setViewportSize({width,height:width===375?667:900});
 if (process.env.SCREENSHOT_DIR) {
 await page.screenshot({path:pathModule.join(process.env.SCREENSHOT_DIR, `c6-ui-${width}.png`),fullPage:true});
 }
 if(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth))throw Error('overflow '+width);
 }
 await page.setViewportSize({width:1440,height:900});
 const joy=await page.locator('#joy').boundingBox();
 await page.mouse.move(joy.x+joy.width/2,joy.y+40);
 if(await page.evaluate(()=>state.joyV!==0))throw Error('hover drives');
 await page.mouse.down();
 if(await page.evaluate(()=>state.joyV<=0))throw Error('drag does not drive');
 await page.evaluate(()=>document.getElementById('btn_stop').click());
 await page.mouse.move(joy.x+joy.width/2,joy.y+30);
 if(await page.evaluate(()=>state.joyV!==0))throw Error('stop resumes');
 await page.mouse.up();
 await page.mouse.move(joy.x+joy.width/2,joy.y+40);await page.mouse.down();await page.mouse.up();
 if(await page.evaluate(()=>state.joyV!==0))throw Error('release does not reset');
 await page.mouse.move(joy.x+joy.width/2,joy.y+40);await page.mouse.down();
 await page.evaluate(()=>document.getElementById('joy').dispatchEvent(new PointerEvent('pointercancel',{pointerId:activePointer})));
 if(await page.evaluate(()=>state.joyV!==0))throw Error('cancel does not reset');
 await page.mouse.up();
 await page.evaluate(()=>{ state.uploading=true; updateControlHint(); });
 await page.mouse.move(joy.x+joy.width/2,joy.y+40);await page.mouse.down();
 if(await page.evaluate(()=>state.joyV!==0))throw Error('uploading allows drive');
 await page.mouse.up();
 await page.evaluate(()=>{ state.uploading=false; onCtl({t:'hello',role:'spec',ver:'preview'}); });
 await page.mouse.move(joy.x+joy.width/2,joy.y+40);await page.mouse.down();
 if(await page.evaluate(()=>state.joyV!==0))throw Error('spectator drives');
 await page.mouse.up();
 await page.locator('#maint summary').click();
 await page.locator('#btn_ota_c6').click();
 if(!await page.locator('#ota_progress').innerText().then(t=>t.includes('选择固件')))throw Error('missing firmware feedback');
 await page.evaluate(()=>{state.tc=true;onCtl({t:'fusion',flags:7,reason:1,distance:550,cap:420,roll:0,pitch:0});});
 if(!await page.locator('#assist_state').innerText().then(t=>t.includes('已限速')))throw Error('fusion slowdown absent');
 await page.evaluate(()=>onCtl({t:'fusion',flags:166,reason:1,distance:1280,cap:150}));
 if(!await page.locator('#assist_state').innerText().then(t=>t.includes('覆盖不足')))throw Error('sparse coverage warning absent');
 if(!await page.locator('#assist_detail').innerText().then(t=>t.includes('0.54')))throw Error('sparse coverage speed absent');
 if(!await page.locator('#assist_attitude').innerText().then(t=>t.includes('待标定')))throw Error('uncalibrated attitude exposed');
 await page.evaluate(()=>onCtl({t:'fusion',flags:71,reason:2,distance:100,cap:0}));
 if(!await page.locator('#assist_state').innerText().then(t=>t.includes('松开前进')))throw Error('fusion rearm feedback absent');
 await page.setViewportSize({width:320,height:900});
 if(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth))throw Error('fusion message overflows 320px');
 await page.evaluate(()=>onCtl({t:'fusion',flags:39,reason:0,distance:75,cap:0}));
 if(!await page.locator('#assist_state').innerText().then(t=>t.includes('距离不足')))throw Error('idle close obstacle shown ready');
 await page.evaluate(()=>{fusionTs=Date.now()-2000;}); await page.waitForTimeout(550);
 if(!await page.locator('#assist_state').innerText().then(t=>t.includes('过期')))throw Error('stale fusion shown healthy');
 if(errors.length)throw Error(errors.join('\n'));
 console.log('PASS: 1440/390/375(SE2)/320px layouts, hover, drag, stop, release, cancellation, spectator/upload gates, firmware feedback; no browser errors');
 await browser.close();
})().catch((error)=>{ console.error(error); process.exit(1); });
