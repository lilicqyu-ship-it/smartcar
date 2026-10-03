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

 await page.goto('http://c6.test/calib.html');
 for(const width of [1440,390,375,320]) {
   await page.setViewportSize({width,height:width===375?667:900});
   if(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth))throw Error('overflow '+width);
   const bounds=await page.locator('#safety').boundingBox();
   await page.locator('#parameters').scrollIntoViewIfNeeded();
   const stop=await page.locator('#btn_stop').boundingBox();
   if(stop.y<0||stop.y+stop.height> (width===375?667:900))throw Error('stop not visible '+width);
   await page.evaluate(()=>window.scrollTo(0,0));
   if(process.env.SCREENSHOT_DIR)await page.screenshot({path:pathModule.join(process.env.SCREENSHOT_DIR,`c6-calib-${width}.png`),fullPage:true});
 }
 if(!await page.locator('#btn_calib').isDisabled())throw Error('calibration before airborne confirmation');
 if(!await page.locator('#jp0').isDisabled())throw Error('jog before airborne confirmation');
 await page.locator('#ck_airborne').check();
 if(await page.locator('#btn_calib').isDisabled())throw Error('calibration not ready');
 page.on('dialog',dialog=>dialog.accept());
 await page.locator('#btn_calib').click();
 if(!await page.locator('#jp0').isDisabled())throw Error('jog during calibration');
 await page.evaluate(()=>onCtl({t:'cal',status:0,saved:1,invert:[1,-1,1,1],delta:[20,-20,20,20]}));
 if(!await page.locator('#s1').innerText().then(t=>t.includes('翻转')))throw Error('calibration result missing');
 await page.evaluate(()=>onCtl({t:'rec',src:1,crcOk:true,pos:[0,2,3,1],invert:[1,-1,1,1],fullScale:3000,wheelDia:100}));
 await page.locator('#jp0').scrollIntoViewIfNeeded();
 const jog=await page.locator('#jp0').boundingBox();
 await page.mouse.move(jog.x+jog.width/2,jog.y+jog.height/2);await page.mouse.down();
 if(await page.evaluate(()=>state.jog.motor!==0))throw Error('jog not started');
 await page.mouse.up();
 if(await page.evaluate(()=>state.jog.motor!==-1))throw Error('jog not stopped');
 // Observe the actual encoded command payload for every channel and sign.
 await page.evaluate(()=>{
   window.jogFrames=[];
   const send=state.ws.send.bind(state.ws);
   state.ws.send=(data)=>{ if(data instanceof Uint8Array && data[3]===0x71) window.jogFrames.push(Array.from(data)); send(data); };
 });
 for(let motor=0;motor<4;motor++) {
   for(const [prefix,duty] of [['jp',500],['jn',-500]]) {
     await page.locator('#'+prefix+motor).scrollIntoViewIfNeeded();
     const button=await page.locator('#'+prefix+motor).boundingBox();
     await page.evaluate(()=>{ window.jogFrames=[]; });
     await page.mouse.move(button.x+button.width/2,button.y+button.height/2);await page.mouse.down();
     const frame=await page.evaluate(()=>window.jogFrames[0]);
     const encodedDuty=(frame[7]|(frame[8]<<8))<<16>>16;
     if(frame[6]!==motor||encodedDuty!==duty)throw Error('wrong jog channel/sign '+motor+' '+prefix);
     await page.mouse.up();
     const stop=await page.evaluate(()=>window.jogFrames[window.jogFrames.length-1]);
     if(stop[6]!==motor||stop[7]!==0||stop[8]!==0)throw Error('wrong jog stop '+motor);
   }
 }
 // Remapped positions must move controls while preserving channel identity.
 await page.evaluate(()=>onCtl({t:'rec',src:1,crcOk:true,pos:[2,0,3,1],invert:[1,-1,1,1],fullScale:3000,wheelDia:100}));
 if(!await page.locator('#jl0').innerText().then(t=>t.includes('后左')))throw Error('remapped label missing');
 if(await page.locator('[data-motor="0"]').evaluate(el=>el.style.order)!=='2')throw Error('remapped grid order');
 if(await page.locator('#lb_rl').textContent()!=='A')throw Error('remapped diagram missing');
 if(await page.locator('#wh_rl').getAttribute('transform'))throw Error('tyre rotates as steering');
 await page.locator('#map0').selectOption('0');
 await page.locator('#btn_rec_set').click();
 if(!await page.locator('#rec_msg').innerText().then(t=>t.includes('各不相同')))throw Error('duplicate positions accepted');
 await page.locator('#map0').selectOption('2');
 await page.locator('#in_fs').fill('99');await page.locator('#btn_rec_set').click();
 if(!await page.locator('#rec_msg').innerText().then(t=>t.includes('100')))throw Error('invalid parameter accepted');
 await page.evaluate(()=>onCtl({t:'err',e:'auth'}));
 if(!await page.locator('#jp0').isDisabled())throw Error('jog after auth loss');
 if(errors.length)throw Error(errors.join('\n'));
 console.log('PASS: calibration layouts, sticky stop, prerequisite gates, run interlock, results, jog release, parameter validation, all channel/sign payloads, spatial remapping, auth loss');
 await browser.close();
})().catch(error=>{console.error(error);process.exit(1);});
