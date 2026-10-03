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
 const results=await page.evaluate(()=>{
   const realNow=Date.now;
   let now=100000;
   Date.now=()=>now;
   const history=[];
   function feed(mv,count=1,pct=70,fault=0) {
     for(let i=0;i<count;i++) {
       now+=20;
       const telemetry=new Uint8Array(38);
       telemetry[19]=mv&255;telemetry[20]=mv>>8;telemetry[21]=pct;
       telemetry[9]=fault&255;telemetry[10]=fault>>8;
       onTelemetry(telemetry);
       const text=document.getElementById('batt_v').textContent;
       if(text)history.push(Number.parseFloat(text));
     }
   }
   try {
     feed(0);
     if(document.getElementById('batt_v').textContent!=='')throw Error('zero measurement initialized voltage');
     feed(8400);
     if(document.getElementById('batt_v').textContent!=='8.40V')throw Error('first sample not initialized');
     for(let i=0;i<500;i++)feed(i%2?8410:8390);
     if(document.getElementById('batt_v').textContent!=='8.40V')throw Error('small jitter changes display');
     feed(7000);feed(8400,100);
     if(document.getElementById('batt_v').textContent!=='8.40V')throw Error('single low spike latched');
     feed(8000,150);
     const lowered=Number.parseFloat(document.getElementById('batt_v').textContent);
     if(lowered>8.03||lowered<8)throw Error('sustained decline not followed');
     feed(8500,150);
     if(Number.parseFloat(document.getElementById('batt_v').textContent)!==lowered)throw Error('rebound increases displayed voltage');
     feed(0,100);
     if(Number.parseFloat(document.getElementById('batt_v').textContent)!==lowered)throw Error('missing sample changes voltage');
     now+=10000;feed(9000,100);
     if(Number.parseFloat(document.getElementById('batt_v').textContent)!==lowered)throw Error('telemetry gap resets monotonic latch');
     feed(7500,150,5,0x80);
     if(!document.getElementById('batt_pct').textContent.includes('5%'))throw Error('sustained low battery percent not shown');
     if(document.getElementById('state').dataset.tone!=='bad')throw Error('fault presentation changed');
     if(Number.parseFloat(document.getElementById('batt_v').textContent)>7.53)throw Error('further decline not followed');
     if(history.some((v,i)=>i>0&&v>history[i-1]))throw Error('displayed voltage increased');
     return {frames:history.length,first:history[0],last:history[history.length-1]};
   } finally {Date.now=realNow;}
 });
 await page.reload();
 await page.evaluate(()=>renderBatteryVoltage(8600));
 if(await page.locator('#batt_v').innerText()!=='8.60V')throw Error('new page did not initialize from current voltage');
 await page.reload();
 const pctResults=await page.evaluate(()=>{
   const realNow=Date.now;
   let now=100000;
   Date.now=()=>now;
   const history=[];
   const shown=()=>document.getElementById('batt_pct').textContent;
   function feed(pct,count=1,mv=8000) {
     for(let i=0;i<count;i++) {
       now+=20;
       const telemetry=new Uint8Array(38);
       telemetry[19]=mv&255;telemetry[20]=mv>>8;telemetry[21]=pct;
       onTelemetry(telemetry);
       if(shown()!=='--')history.push(Number.parseInt(shown()));
     }
   }
   try {
     feed(0,1,0);
     if(shown()!=='--')throw Error('percent initialized from missing voltage');
     feed(50);
     if(shown()!=='50%')throw Error('first percent sample not initialized');
     for(let i=0;i<500;i++)feed(i%2?50:49);
     if(shown()!=='50%')throw Error('1% boundary noise changes percentage');
     feed(0);feed(50,100);
     if(shown()!=='50%')throw Error('isolated low percentage latched');
     feed(49,50);
     if(shown()!=='50%')throw Error('percentage dropped without confirmation');
     feed(49,50);
     if(shown()!=='49%')throw Error('sustained 1% decline ignored');
     feed(60,150);
     if(shown()!=='49%')throw Error('percentage rebound displayed');
     feed(48,25);now+=10000;feed(48);
     if(shown()!=='49%')throw Error('missing telemetry counted as confirmation');
     feed(49,100);feed(255,100);feed(0,100,0);
     if(shown()!=='49%')throw Error('invalid percentage or voltage changes display');
     feed(5);
     if(document.getElementById('batt_pill').style.color!=='var(--bad)')throw Error('raw low-battery warning delayed');
     if(shown()!=='49%')throw Error('single low percentage bypasses confirmation');
     feed(0,200);
     if(shown()!=='0%')throw Error('valid empty battery rejected');
     if(history.some((v,i)=>i>0&&v>history[i-1]))throw Error('displayed percentage increased');
     return {frames:history.length,first:history[0],last:history[history.length-1]};
   } finally { Date.now=realNow; }
 });
 await page.reload();
 await page.evaluate(()=>renderBatteryPercent(80,8500));
 if(await page.locator('#batt_pct').innerText()!=='80%')throw Error('new page did not initialize percentage');
 if(errors.length)throw Error(errors.join('\n'));
 console.log('PASS: percentage boundary/spike debounce, 1.5s confirmation, no rebound, invalid samples, gap handling, raw warning color, zero/reload initialization; '+JSON.stringify(pctResults));
 console.log('PASS: voltage jitter/spike rejection, sustained decline, no rebound or gap increase, invalid zero handling, reload initialization, percent/fault presentation; '+JSON.stringify(results));
 await browser.close();
})().catch(error=>{console.error(error);process.exit(1);});
