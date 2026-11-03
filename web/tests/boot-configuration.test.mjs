import test from 'node:test';
import assert from 'node:assert/strict';
import {loadConfiguration,commitConfiguration,inputFile,CONFIGURATIONS} from '../boot-configuration.js';
class Dir {
  kind='directory'; children=new Map(); failWrite=false;
  async getDirectoryHandle(name,{create=false}={}) {
    if(!this.children.has(name)){if(!create)throw Error('missing');this.children.set(name,new Dir());}
    return this.children.get(name);
  }
  async getFileHandle(name,{create=false}={}) {
    if(!this.children.has(name)){if(!create)throw Error('missing');this.add(name,0);}
    return this.children.get(name);
  }
  add(name,size) {
    const item={kind:'file',size,data:new Uint8Array(),getFile:async()=>({size:item.size,text:async()=>new TextDecoder().decode(item.data),arrayBuffer:async()=>item.data.buffer}),createSyncAccessHandle:async()=>({
      truncate:n=>{item.size=n;item.data=new Uint8Array(n)},
      write:(bytes,{at})=>{if(this.failWrite)throw Error('disk full');const next=new Uint8Array(Math.max(item.data.length,at+bytes.length));next.set(item.data);next.set(bytes,at);item.data=next;item.size=next.length;return bytes.length},flush(){},close(){},
    })};
    this.children.set(name,item);return item;
  }
  async removeEntry(name){this.children.delete(name)}
  async *entries(){yield* this.children.entries()}
}
async function fixture(){
  const root=new Dir();root.add('nand.bin',553648128);root.add('otp.bin',8192);
  const dir=await root.getDirectoryHandle('selected',{create:true});dir.add('nand.bin',553648128);dir.add('plaintext.ldr',2171624);
  const config={kind:'manual',nand:{path:['selected'],name:'nand.bin'},otp:{path:[],name:'otp.bin'}};
  return {root,config};
}
test('selection commit replaces legacy inputs and survives discovery',async()=>{
  const {root,config}=await fixture();const before=await loadConfiguration(root);assert.equal(before.config.kind,'manual');
  const next=await commitConfiguration(root,before,config);assert.deepEqual(await loadConfiguration(root),next);
  assert.equal((await inputFile(root,next.config.nand)).size,553648128);
});
test('partial manual selection persists without reviving the previous NAND',async()=>{
  const {root,config}=await fixture();let next=await commitConfiguration(root,await loadConfiguration(root),config);
  next=await commitConfiguration(root,next,{kind:'manual',otp:{path:[],name:'otp.bin'}});
  assert.deepEqual(await loadConfiguration(root),next);assert.equal((await loadConfiguration(root)).config.nand,undefined);
});
test('failed selection writes and malformed trailing records preserve previous selection',async()=>{
  const {root,config}=await fixture();const selected=await commitConfiguration(root,await loadConfiguration(root),config);
  const dir=await root.getDirectoryHandle(CONFIGURATIONS);dir.failWrite=true;
  await assert.rejects(commitConfiguration(root,selected,{kind:'manual',nand:{path:[],name:'nand.bin'}}),/disk full/);
  assert.deepEqual(await loadConfiguration(root),selected);assert.equal(dir.children.size,1);
  const broken=dir.add('truncated.json',2);broken.data=new TextEncoder().encode('{"');
  assert.deepEqual(await loadConfiguration(root),selected);
});
test('repeated commits use increasing sequence even when clock does not advance',async()=>{
  const {root,config}=await fixture();const future={sequence:Date.now()+100000,config};
  const one=await commitConfiguration(root,future,config),two=await commitConfiguration(root,one,{kind:'manual',nand:{path:[],name:'nand.bin'}});
  assert.equal(two.sequence,one.sequence+1);assert.deepEqual(await loadConfiguration(root),two);
});
test('incomplete selected input cannot displace an earlier valid record',async()=>{
  const {root,config}=await fixture();const previous=await commitConfiguration(root,await loadConfiguration(root),config);
  await commitConfiguration(root,previous,{kind:'manual',nand:{path:[],name:'missing.bin'}});
  assert.deepEqual(await loadConfiguration(root),previous);
});
test('persisted empty selection prevents discovery of unrelated legacy inputs',async()=>{
  const {root}=await fixture();const empty=await commitConfiguration(root,{sequence:0,config:null},null);
  assert.deepEqual(await loadConfiguration(root),empty);
});
test('failed first selection write preserves legacy inputs after reload',async()=>{
  const {root,config}=await fixture();const previous=await loadConfiguration(root);
  const dir=await root.getDirectoryHandle(CONFIGURATIONS,{create:true});dir.failWrite=true;
  await assert.rejects(commitConfiguration(root,previous,config),/disk full/);
  assert.deepEqual(await loadConfiguration(root),previous);
});

test('synthetic invalid firmware loader cannot replace valid selected inputs',async()=>{
  const {root,config}=await fixture();const previous=await commitConfiguration(root,await loadConfiguration(root),config);
  await commitConfiguration(root,previous,{kind:'firmware',nand:{path:['selected'],name:'nand.bin'},ldr:{path:['selected'],name:'plaintext.ldr'},pages:8});
  assert.deepEqual(await loadConfiguration(root),previous);
});
test('cancellation while acquiring final selection handle preserves the previous commit',async()=>{
  const {root,config}=await fixture();const previous=await commitConfiguration(root,await loadConfiguration(root),config);
  const dir=await root.getDirectoryHandle(CONFIGURATIONS),original=dir.getFileHandle.bind(dir);
  let acquired,release;
  const waiting=new Promise(resolve=>{acquired=resolve}),gate=new Promise(resolve=>{release=resolve});
  let closed=false,wrote=false;
  dir.getFileHandle=async(...args)=>{
    const file=await original(...args),open=file.createSyncAccessHandle.bind(file);
    return {...file,createSyncAccessHandle:async()=>{
      const handle=await open();acquired();await gate;
      return {...handle,write:(...a)=>{wrote=true;return handle.write(...a)},close:()=>{closed=true;handle.close()}};
    }};
  };
  const controller=new AbortController();
  const pending=commitConfiguration(root,previous,{kind:'manual',otp:{path:[],name:'otp.bin'}},{signal:controller.signal});
  await waiting;controller.abort();release();
  await assert.rejects(pending,{name:'AbortError'});
  assert.equal(wrote,false);assert.equal(closed,true);assert.equal(dir.children.size,1);
  assert.deepEqual(await loadConfiguration(root),previous);
});
