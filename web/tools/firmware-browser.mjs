// Source-only browser preparation test. Deliberately stubs the emulator build
// manifest. Default: real UI/worker invalid-input rejection and no OTP fetch.
// Optional private inputs: decoder and OPFS preparation. Neither tests native boot.
import path from 'node:path';
import fs from 'node:fs';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {build} from 'vite';
import {chromium} from 'playwright-core';
import {serveDist} from './serve-dist.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const out=fs.mkdtempSync(path.join(os.tmpdir(),'op1-web-ui-'));
await build({configFile:false,root,base:'./',logLevel:'warn',worker:{format:'es'},build:{outDir:out,emptyOutDir:true},plugins:[{
  name:'test-only-emulator-manifest',resolveId:id=>id==='virtual:op1emu'?'\0test-manifest':null,
  load:id=>id==='\0test-manifest'?`export default ${JSON.stringify({js:'not-used.mjs',wasm:{file:'not-used.wasm',size:0},bootRom:{url:'https://invalid.test/not-used',sha256:''}})}`:null,
}]});
const server=await serveDist(out);
let browser, page;
try {
browser=await chromium.launch({headless:true,...(process.env.CHROME?{executablePath:process.env.CHROME}:{channel:'chrome'})});
const context=await browser.newContext();page=await context.newPage();
const errors=[];page.on('pageerror',e=>errors.push(e.message));
let otpRequests=0;
await context.route('https://raw.githubusercontent.com/Tolsi/op1dumps/**',async route=>{
  otpRequests++;
  if(process.env.OP1_OTP_TEXT)await route.fulfill({body:fs.readFileSync(process.env.OP1_OTP_TEXT),contentType:'text/plain'});
  else await route.abort();
});
const status=()=>page.locator('#status').textContent();
  await page.goto(server.url);
  await page.waitForFunction(()=>document.getElementById('nand-state').textContent!=='…');
  if(!await page.locator('#start').isDisabled())throw Error('Empty input unexpectedly startable');
  await page.locator('#firmware-file').setInputFiles({name:'bad.op1',mimeType:'application/octet-stream',buffer:Buffer.from([1,2,3])});
  await page.locator('#prepare-firmware').click();
  await page.waitForFunction(()=>document.getElementById('status').textContent.includes('Unsupported firmware'));
  if(otpRequests)throw Error('Invalid input triggered OTP network request');
  if(process.env.OP1_FIRMWARE&&process.env.OP1_OTP_TEXT){
    await page.locator('#firmware-file').setInputFiles(process.env.OP1_FIRMWARE);
    await page.locator('#firmware-otp').setInputFiles(process.env.OP1_OTP_TEXT);
    await page.locator('#prepare-firmware').click();
    await page.waitForFunction(()=>document.getElementById('firmware-state').textContent.includes('11810'),null,{timeout:180000});
    if(otpRequests)throw Error('Local OTP unexpectedly fetched');
    if(await page.locator('#start').isDisabled())throw Error('Prepared profile not startable');
    await page.reload();
    await page.waitForFunction(()=>document.getElementById('firmware-state').textContent.includes('11810'));
    await page.locator('#boot-source').selectOption('firmware');
    if(await page.locator('#start').isDisabled())throw Error('Profile not reusable after reload');
    // Wrong local key must not destroy the previously committed profile.
    await page.locator('#firmware-file').setInputFiles(process.env.OP1_FIRMWARE);
    await page.locator('#firmware-otp').setInputFiles({name:'wrong.txt',mimeType:'text/plain',buffer:Buffer.from('!0xd0 0000000000000000 0000000000000000')});
    await page.locator('#prepare-firmware').click();
    await page.waitForFunction(()=>document.getElementById('status').textContent.includes('SHA-256 mismatch'),null,{timeout:180000});
    if(!await page.locator('#firmware-state').textContent().then(t=>t.includes('11810')))throw Error('Failure lost prior profile');
    // Immediate cancellation preserves previous generation.
    await page.locator('#prepare-firmware').click();await page.locator('#cancel-firmware').click();
    await page.waitForFunction(()=>!document.getElementById('prepare-firmware').disabled,null,{timeout:30000});
    if(!await page.locator('#firmware-state').textContent().then(t=>t.includes('11810')))throw Error('Cancel lost prior profile');
    console.log('private canonical preparation, local-key isolation, persistence, wrong-key preservation and cancellation: passed');
  }
  if(errors.length)throw Error(errors.join('\n'));
  console.log('source UI/worker invalid-input rejection and no-fetch checks passed; native boot not executed');
}catch(e){if(page)console.error('UI status:',await page.locator('#status').textContent().catch(()=>''));throw e;}
finally{await browser?.close();server.close();fs.rmSync(out,{recursive:true,force:true});}
