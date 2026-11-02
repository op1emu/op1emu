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
  const elements=new Map(),$=id=>{if(!elements.has(id))elements.set(id,{disabled:false,classList:{remove(){}}});return elements.get(id)};
  const context={worker:{},$,setStatus(){},params:new URLSearchParams(),cacheModule(){}};
  vm.createContext(context);
  vm.runInContext(`let running=${running};\n`+main.slice(main.indexOf('let files = {}'),main.indexOf('function importFile(file)'))+main.slice(main.indexOf('worker.onmessage ='),main.indexOf('\n(async () => {')),context);
  return {context,elements,send:data=>context.worker.onmessage({data}),running:()=>vm.runInContext('running',context)};
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
test('prepared-only Export and Start are enabled from the active UI description',async()=>{
  const w=worker(prepared),u=ui();const files=await w.run('files()');u.send({type:'files',files,syncAccess:true});
  assert.equal(u.elements.get('start').disabled,false);assert.equal(u.elements.get('export-nand').disabled,false);assert.equal(u.elements.get('export-otp').disabled,true);
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
test('generic storage errors preserve running state',()=>{const u=ui(true);u.send({type:'error',text:'failed export'});assert.equal(u.running(),true)});
test('stopped and failed sessions require reload',()=>{for(const type of ['stopped','start-failed']){const u=ui(true);u.send({type,text:'failure'});assert.equal(u.running(),true);assert.equal(u.elements.get('start').disabled,true)}});
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
