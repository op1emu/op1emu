import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import {webcrypto} from 'node:crypto';
const main=fs.readFileSync(new URL('../main.js',import.meta.url),'utf8');
const source=fs.readFileSync(new URL('../worker.js',import.meta.url),'utf8');
const ref=name=>({path:[name],name:'nand.bin'});
const manual={kind:'manual',nand:ref('manual'),otp:{path:['manual'],name:'otp.bin'}};
const prepared={kind:'firmware',nand:ref('prepared'),ldr:{path:['prepared'],name:'plaintext.ldr'},pages:17};
function ui(running=false){
  const elements=new Map(),messages=[],statuses=[];
  let audioClosed=0;
  const $=id=>{if(!elements.has(id))elements.set(id,{disabled:false,style:{},classList:{remove(){},toggle(){}}});return elements.get(id)};
  const context={worker:{postMessage:message=>messages.push(message)},$,setStatus:(...args)=>statuses.push(args),params:new URLSearchParams(),cacheModule(){},console,
    KEYMAP:{KeyA:'synth'},ui:{buttons:{usb:{gpio:[1,2]},synth:{gpio:[3,4]}}},
    audioContext:{close:()=>{audioClosed++;return Promise.resolve()}},
  };
  vm.createContext(context);
  vm.runInContext(`let running=${running},started=${running};\n`+
    main.slice(main.indexOf('const keySources ='),main.indexOf('function turnEncoder('))+
    main.slice(main.indexOf('const held ='),main.indexOf('function bindCom('))+
    main.slice(main.indexOf('let files = {}'),main.indexOf('function bindFiles()'))+
    main.slice(main.indexOf('function stop()'),main.indexOf('async function start()'))+
    main.slice(main.indexOf('worker.onmessage ='),main.indexOf('\n(async () => {')),context);
  return {context,elements,messages,statuses,audioClosed:()=>audioClosed,send:data=>context.worker.onmessage({data}),running:()=>vm.runInContext('running',context),run:code=>vm.runInContext(code,context)};
}
function worker(config=manual){
  const messages=[],opened=[],removed=[];
  let committed={sequence:1,config};
  const fileFor=ref=>({size:ref.name==='otp.bin'?8192:ref.name==='plaintext.ldr'?2171624:553648128,lastModified:123,identity:ref.path.join('/'),arrayBuffer:async()=>new Uint8Array([42]).buffer});
  const directory=path=>({
    getFileHandle:async name=>({getFile:async()=>fileFor({path,name}),createSyncAccessHandle:async()=>{
      opened.push([...path,name].join('/'));return {truncate(){},write:b=>b.length,flush(){},close(){},getSize:()=>1,read:b=>{b[0]=42;return 1}};
    }}),
    getDirectoryHandle:async name=>directory([...path,name]),removeEntry:async name=>removed.push([...path,name].join('/')),
  });
  const root=directory([]);
  const context={self:{postMessage:m=>messages.push(m)},navigator:{storage:{getDirectory:async()=>root,estimate:async()=>({})}},MessageChannel:class{port1={};port2={postMessage(){}}},AbortController,DOMException,Uint8Array,Blob,console,crypto:webcrypto,
    loadConfiguration:async()=>committed,commitConfiguration:async(_root,previous,next)=>committed={sequence:previous.sequence+1,config:next},
    inputFile:async(_root,ref)=>fileFor(ref),inputDirectory:async(_root,ref)=>directory(ref.path),
    extractFirmware246:async()=>({loader:new Uint8Array([1]),entries:[{name:'content/sample'}]}),firmwareHash:async()=> 'plaintext',ENCRYPTED_246_SHA256:'encrypted',decryptLoader:loader=>({bytes:loader}),validateFirmware246:async()=>{},provisionNand(){},
    stageProfile:async()=>({name:'prepared',info:{pages:17}}),profileConfiguration:()=>prepared,PROFILES:'profiles',CONFIGURATIONS:'configs',MANUAL_INPUTS:'manual-inputs',
  };
  vm.createContext(context);
  vm.runInContext(source.replace(/^import .*?;\n/gms,'').replaceAll('import.meta.url',JSON.stringify('https://test.invalid/worker.js')),context);
  return {context,messages,root,opened,removed,run:code=>vm.runInContext(code,context),stored:()=>committed};
}
const prepare='prepareFirmware({requestId:7,file:{size:1,arrayBuffer:async()=>new ArrayBuffer(1)}})';
test('prepared-only Export, COM and Start are enabled from the active UI description',async()=>{
  const w=worker(prepared),u=ui();const files=await w.run('files()');u.send({type:'files',files,syncAccess:true});
  assert.equal(u.elements.get('start').disabled,false);assert.equal(u.elements.get('te-boot').disabled,false);assert.equal(u.elements.get('export-nand').disabled,false);assert.equal(u.elements.get('export-otp').disabled,true);
  assert.match(u.elements.get('firmware-state').textContent,/Active: firmware 246/);
  assert.equal((await w.run("exportFile('nand')")).identity,'prepared');
});
test('manual to prepared replaces the labels and Export only after successful preparation',async()=>{
  const w=worker();assert.equal((await w.run("exportFile('nand')")).identity,'manual');
  await w.run(prepare);assert.equal((await w.run('files()')).kind,'firmware');assert.equal((await w.run("exportFile('nand')")).identity,'prepared');
  assert.equal(w.stored().config,prepared);await assert.rejects(w.run("exportFile('otp')"),/No active otp/);
});
test('prepared to manual import selects only manual inputs; a second import completes it',async()=>{
  const w=worker(prepared);
  await w.run("importFile('otp',{size:8192,name:'otp.bin',slice:()=>({arrayBuffer:async()=>new Uint8Array(8192).buffer})})");
  assert.equal((await w.run('files()')).kind,'manual');assert.equal((await w.run('files()')).ready,false);
  await assert.rejects(w.run("exportFile('nand')"),/No active nand/);
  await w.run("importFile('nand',{size:553648128,name:'nand.bin',slice:(a,b)=>({arrayBuffer:async()=>new Uint8Array(Math.min(b,553648128)-a).buffer})})");
  assert.equal((await w.run('files()')).ready,true);assert.equal(w.stored().config.kind,'manual');assert.ok(w.stored().config.otp);
});
test('Start opens precisely the active selection and Export keeps it after handles open',async()=>{
  for(const config of [manual,prepared]){
    const w=worker(config);w.run("compileAhead=()=>Promise.resolve({});ensureBootRom=async()=>{};instantiate=async()=>{throw Error('native stub boundary')}");
    await assert.rejects(w.run('start({build:{}})'),/native stub boundary/);
    assert.equal(w.opened[0],`${config.nand.path[0]}/nand.bin`);
    assert.equal(w.opened.some(n=>n.endsWith('/otp.bin')),config.kind==='manual');
    w.run('dataFs={flush(){}}');assert.deepEqual([...new Uint8Array(await(await w.run("exportFile('nand')")).arrayBuffer())],[42]);
    assert.equal((await w.run('files()')).kind,config.kind);
  }
});
test('failed preparation preserves active inputs and never announces success',async()=>{
  for(const point of ['extractFirmware246','stageProfile','commitConfiguration']){
    const w=worker();w.context[point]=async()=>{throw Error('injected failure')};
    await assert.rejects(w.run(prepare),/injected failure/);assert.equal(w.stored().config,manual);
    assert.equal((await w.run("exportFile('nand')")).identity,'manual');assert.equal(w.messages.some(m=>m.type==='prepared'),false);
    assert.equal(w.run('preparing'),null);
  }
});
test('queued and in-flight cancellation preserve previous configuration',async()=>{
  for(const queued of [true,false]){
    const w=worker();
    if(queued)w.context.self.onmessage({data:{type:'cancel-firmware',requestId:7}});
    else w.context.stageProfile=async()=>{w.context.self.onmessage({data:{type:'cancel-firmware',requestId:7}});return {name:'cancelled',info:{pages:17}}};
    await assert.rejects(w.run(prepare),{name:'AbortError'});assert.equal(w.stored().config,manual);assert.equal(w.run('cancelledPreparations.size'),0);
    assert.equal(w.messages.some(m=>m.type==='prepared'),false);
  }
});
test('repeated preparation replaces only on success and survives worker reload',async()=>{
  const w=worker();await w.run(prepare);const first=w.stored();await w.run(prepare);assert.equal(w.stored().sequence,first.sequence+1);
  w.context.stageProfile=async()=>{throw Error('disk full')};await assert.rejects(w.run(prepare),/disk full/);
  w.run('activeConfiguration=undefined');assert.equal((await w.run('files()')).kind,'firmware');assert.equal((await w.run("exportFile('nand')")).identity,'prepared');
});
test('encrypted input without local OTP fails without retrieval',async()=>{
  const w=worker();w.context.firmwareHash=async()=> 'encrypted';await assert.rejects(w.run(prepare),/local OTP text file/);assert.equal(w.stored().config,manual);
});
test('generic storage errors preserve running state and audio',()=>{const u=ui(true);u.send({type:'error',command:'export',text:'failed export'});assert.equal(u.running(),true);assert.equal(u.run('started'),true);assert.equal(u.audioClosed(),0)});
test('stopped and failed starts release input and audio but remain terminal',()=>{
  for(const message of [{type:'stopped'},{type:'error',command:'start',text:'failure'}]){
    const u=ui(true);
    u.send({type:'files',syncAccess:true,files:{kind:'firmware',ready:true,nand:{size:553648128},firmware:{pages:17}}});
    u.run("keysDown.add('KeyA');key('synth',true,'keyboard:KeyA');comPointers.add(1);updateCom()");
    assert.equal(u.messages.filter(m=>m.pressed).length,2);
    u.send(message);
    assert.equal(u.running(),false);assert.equal(u.run('started'),true);
    assert.equal(u.elements.get('start').disabled,true);assert.equal(u.elements.get('te-boot').disabled,true);
    assert.equal(u.audioClosed(),1);assert.equal(u.run('keysDown.size+comPointers.size+comKeys.size+held.size+keySources.size'),0);
    assert.equal(u.messages.filter(m=>!m.pressed).length,2);
    const messageCount=u.messages.length;u.run("importFile({name:'otp.bin',size:8192})");assert.equal(u.messages.length,messageCount);
    const terminal=u.statuses.at(-1);assert.match(terminal[0],/Reload the page/);
    u.send({type:'progress',text:'late progress'});u.send({type:'stats',frames:1,guestS:42});
    assert.equal(u.statuses.at(-1),terminal);
  }
});
test('second native start and input replacement are refused',async()=>{const w=worker();w.run('op1 = {}');await assert.rejects(w.run('start({})'),/native session already exists/);await assert.rejects(w.run("importFile('otp',{size:8192})"),/reload/);await assert.rejects(w.run(prepare),/Reload/)});
test('failed manual import leaves prepared selection active',async()=>{
  const w=worker(prepared);
  await assert.rejects(w.run("importFile('otp',{size:8192,name:'otp.bin',slice:()=>({arrayBuffer:async()=>{throw Error('read failed')}})})"),/read failed/);
  assert.equal(w.stored().config,prepared);assert.equal((await w.run("exportFile('nand')")).identity,'prepared');
  assert.equal(w.removed.length,1);
});
test('legacy selection is pinned before a new profile can be staged',async()=>{
  const w=worker(manual);w.context.loadConfiguration=async()=>({sequence:0,config:manual});
  let commits=0;
  const commit=w.context.commitConfiguration;
  w.context.commitConfiguration=async(...args)=>{commits++;return commit(...args)};
  w.context.stageProfile=async()=>{assert.equal(commits,1);assert.equal(w.stored().config,manual);throw Error('interrupted staging')};
  await assert.rejects(w.run(prepare),/interrupted staging/);
  assert.equal(w.stored().config,manual);assert.equal(w.stored().sequence,1);
});
test('Export survives native instantiation failure before dataFs exists',async()=>{
  const w=worker(prepared);w.run("compileAhead=()=>Promise.resolve({});ensureBootRom=async()=>{};instantiate=async()=>{throw Error('instantiation failed')}");
  await assert.rejects(w.run('start({build:{}})'),/instantiation failed/);
  assert.equal(w.run('dataFs'),null);
  const exported=await w.run("exportFile('nand')");
  assert.deepEqual([...new Uint8Array(await exported.arrayBuffer())],[42]);
  assert.equal((await w.run('files()')).kind,'firmware');
});
test('Delete removes the obsolete OTP reference without reading or reusing it',async()=>{
  const w=worker();
  w.root.getFileHandle=async name=>{if(name==='firmware-otp-reference.txt')throw Error('must not read obsolete reference');throw Error('unexpected read')};
  await w.run('deleteFiles()');
  assert.ok(w.removed.includes('firmware-otp-reference.txt'));
  assert.ok(w.removed.includes('configs'));assert.ok(w.removed.includes('manual-inputs'));assert.ok(w.removed.includes('profiles'));
});

// Browser harnesses must implement the production files-message contract.
for(const name of ['input-browser.mjs','panel-browser.mjs'])test(`${name} advertises a startable active configuration`,()=>{
  const source=fs.readFileSync(new URL(`../tools/${name}`,import.meta.url),'utf8');
  const context={setTimeout:callback=>callback()};vm.createContext(context);
  vm.runInContext(source.slice(source.indexOf('function standIn()'),source.indexOf('const server ='))+'standIn();',context);
  const w=new context.Worker(),u=ui();w.onmessage=({data})=>u.send(data);
  w.postMessage({type:'files'});
  assert.equal(u.elements.get('start').disabled,false);
  assert.equal(u.elements.get('export-nand').disabled,false);
});

test('preparation disables COM and Start without losing prepared configuration',async()=>{
  const w=worker(prepared),u=ui();u.send({type:'files',files:await w.run('files()'),syncAccess:true});
  u.send({type:'preparing',value:true});assert.equal(u.elements.get('start').disabled,true);assert.equal(u.elements.get('te-boot').disabled,true);
  u.send({type:'preparing',value:false});assert.equal(u.elements.get('start').disabled,false);assert.equal(u.elements.get('te-boot').disabled,false);
  assert.match(u.elements.get('firmware-state').textContent,/Active: firmware 246/);
});
test('queued COM level reaches native before the first slice for both selected boot paths',async()=>{
  for(const config of [manual,prepared])for(const released of [false,true]){
    const w=worker(config),events=[];
    let loading,finish;
    const atLoading=new Promise(resolve=>{loading=resolve}),gate=new Promise(resolve=>{finish=resolve});
    w.context.events=events;w.context.setInterval=()=>0;
    w.context.native={FS:{mkdir(){},mount(){},chdir(){},writeFile(){}},stringToNewUTF8:s=>s,
      _op1_start:(nand,ldr)=>{events.push(['start',nand,ldr]);return 0},
      _op1_key:(bank,pin,pressed)=>events.push(['key',bank,pin,pressed]),
      _op1_step:()=>{events.push(['slice']);return -1},
    };
    w.context.opfsFs=()=>({flush(){}});
    w.context.instantiateStub=async()=>{loading();await gate;return w.context.native};
    w.run('compileAhead=()=>Promise.resolve({});ensureBootRom=async()=>{};instantiate=instantiateStub;tierWorker=()=>{}');
    const send=data=>w.context.self.onmessage({data});
    send({type:'com',bank:1,pin:2,pressed:true});send({type:'start',build:{}});
    await atLoading;
    if(released)send({type:'com',bank:1,pin:2,pressed:false});
    assert.deepEqual(events,[]);finish();await w.run('queue');
    assert.deepEqual(events,[['start','/data/nand.bin',config.kind==='firmware'?'/firmware.ldr':''],['key',1,2,released?0:1],['slice']]);
    assert.equal(w.opened[0],`${config.nand.path[0]}/nand.bin`);
    assert.equal(w.opened.some(n=>n.endsWith('/otp.bin')),config.kind==='manual');
    assert.equal(w.messages.some(m=>m.type==='error'),false);
  }
});
test('worker start failures retain command tagging for terminal UI cleanup',async()=>{
  const w=worker();w.run('op1={}');w.context.self.onmessage({data:{type:'start',build:{}}});await w.run('queue');
  const failure=w.messages.find(m=>m.type==='error');assert.equal(failure.command,'start');assert.match(failure.text,/native session already exists/);
});
