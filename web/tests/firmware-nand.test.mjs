import test from 'node:test';
import assert from 'node:assert/strict';
import {provisionNand, NAND_SIZE, DATA_SIZE, START_PAGE, PAGE_SIZE, OOB_SIZE} from '../firmware-nand.js';
const file = (name = 'content/a', bytes = Uint8Array.of(1,2,3)) => ({name, bytes, type:'file', mode:0o644, mtime:123});
class Sink {
  records = new Map(); writes=0; erased=0; flushed=false;
  truncate(n) { this.size=n; }
  write(bytes,{at}) {
    this.writes++; assert.ok(bytes.length <= 1024*1024);
    if (bytes.length > PAGE_SIZE) {
      assert.equal(at,this.erased); assert.ok(bytes.every(b=>b===255)); this.erased+=bytes.length;
    } else this.records.set(at,bytes.slice());
    return bytes.length;
  }
  flush() { this.flushed=true; }
  data(i) { return this.records.get((START_PAGE+i)*PAGE_SIZE); }
  oob(i) { return this.records.get(DATA_SIZE+(START_PAGE+i)*OOB_SIZE); }
}
const hex=bytes=>Buffer.from(bytes).toString('hex');
test('split NAND geometry, erased bytes, source-derived headers and tag/ECC vectors',async()=>{
  const sink=new Sink(), progress=[];
  assert.deepEqual(await provisionNand([file()],sink,{onProgress:p=>progress.push(p)}),{pages:3,objects:2,bytes:553648128});
  assert.equal(sink.size,NAND_SIZE); assert.equal(sink.erased,NAND_SIZE); assert.ok(sink.flushed);
  const dir=new DataView(sink.data(0).buffer), hdr=new DataView(sink.data(1).buffer);
  assert.equal(dir.getUint32(0,true),3); assert.equal(dir.getUint32(4,true),1);
  assert.equal(hdr.getUint32(0,true),1); assert.equal(hdr.getUint32(4,true),257);
  assert.equal(hdr.getUint32(268,true),0o100644);
  for(const offset of [280,284,288]) assert.equal(hdr.getUint32(offset,true),123);
  assert.equal(hdr.getUint32(292,true),3); assert.equal(hdr.getUint32(460,true),0);
  assert.equal(hdr.getUint32(496,true),0xffffffff);
  assert.equal(sink.data(1)[10],97); assert.equal(sink.data(1)[11],0);
  assert.ok(sink.data(1).subarray(512).every(b=>b===255));
  assert.deepEqual(sink.data(2).subarray(0,3),Uint8Array.of(1,2,3));
  assert.ok(sink.data(2).subarray(3).every(b=>b===255));
  // Independently derived from upstream's parity table, not this algorithm.
  const golden=[
    '001000000101000000000000ffff000025ffffff00000000ffffffff',
    '001000000201000000000000ffff000026ffffff00000000ffffffff',
    '0010000002010000010000000300000030ffffff0800000008000000',
  ];
  for(let i=0;i<3;i++) {
    assert.equal(hex(sink.oob(i).subarray(4,32)),golden[i]);
    assert.ok(sink.oob(i).subarray(0,4).every(b=>b===255));
    assert.ok(sink.oob(i).subarray(32).every(b=>b===255));
  }
  assert.deepEqual(progress.at(-1),{phase:'content',completed:3,total:3});
});
test('tree ordering is deterministic, inferred directories and explicit metadata work',async()=>{
  const entries=[file('content/z/a',new Uint8Array(2049)),{name:'content/z/',type:'directory',mode:0o700,mtime:5},file('content/a',new Uint8Array())];
  const a=new Sink(),b=new Sink();
  await provisionNand(entries,a); await provisionNand([...entries].reverse(),b);
  assert.deepEqual(a.records,b.records); assert.equal(a.records.size,12);
  const z=new DataView(a.data(2).buffer);
  assert.equal(z.getUint32(268,true),0o040700); assert.equal(z.getUint32(284,true),5);
  assert.equal(new DataView(a.oob(5).buffer).getUint32(16,true),1);
});
test('reject unsafe, duplicate, link, metadata and conflicting paths before mutation',async()=>{
  const invalid=[[],[file('../content/a')],[file('/content/a')],[file('content/../a')],[file('content//a')],[file('content/./a')],[file('content/a\\b')],[file('content/\0')],[file('content/'+'é'.repeat(128))],[file(),file()],[{...file(),type:'symlink'}],[file('content'),file()],[file(),file('content/a/b')],[{...file(),mtime:-1}],[{...file(),mode:0o040644}],[{...file(),mode:0o120644}],[{...file(),mode:0o200000}],[{name:'content',type:'directory',bytes:Uint8Array.of(1)},file()]];
  for(const entries of invalid) {
    const sink=new Sink(); await assert.rejects(provisionNand(entries,sink));
    assert.equal(sink.writes,0); assert.equal(sink.size,undefined);
  }
  class Huge extends Uint8Array { get length() { return NAND_SIZE; } }
  const sink=new Sink(); await assert.rejects(provisionNand([file('content/a',new Huge())],sink),/capacity/); assert.equal(sink.size,undefined);
});
test('short writes are retried, zero writes rejected and cancellation is observable',async()=>{
  const controller=new AbortController(); let written=0;
  const short={truncate(){},write(bytes,{at}){assert.equal(at,written); const n=Math.min(100000,bytes.length); written+=n; return n;},flush(){assert.fail('cancelled output must not flush');}};
  await assert.rejects(provisionNand([file()],short,{signal:controller.signal,onProgress(){controller.abort();}}),{name:'AbortError'});
  assert.equal(written,1024*1024);
  await assert.rejects(provisionNand([file()],{truncate(){},write(){return 0;},flush(){}}),/Incomplete/);
  const sink=new Sink(); await assert.rejects(provisionNand([file()],sink,{signal:controller.signal}),{name:'AbortError'}); assert.equal(sink.size,undefined);
  await assert.rejects(provisionNand([file()],{truncate(){throw Error('quota');},write(){},flush(){}}),/quota/);
});
test('accept canonical TAR modes carrying matching POSIX type bits',async()=>{
  const sink=new Sink(); await provisionNand([{name:'content',type:'directory',mode:0o40666,mtime:1},{...file(),mode:0o100666}],sink);
  assert.equal(new DataView(sink.data(0).buffer).getUint32(268,true),0o40666);
  assert.equal(new DataView(sink.data(1).buffer).getUint32(268,true),0o100666);
  const untouched=new Sink(); await assert.rejects(provisionNand([{name:'content',type:'directory',mode:0o100666},file()],untouched),/Conflicting mode type/); assert.equal(untouched.size,undefined);
});

// Opt-in external oracle. Build only the independently approved pinned source;
// this test never downloads/installs/builds software itself. Example:
// MKYAFFS2=/absolute/path/to/approved/mkyaffs2 node --test tests/firmware-nand.test.mjs
// Single-child trees avoid mkyaffs2's platform-dependent readdir ordering.
test('independent mkyaffs2: complete page and translated OOB equivalence', {skip:!process.env.MKYAFFS2}, async()=>{
  const fs=await import('node:fs/promises'), os=await import('node:os'), path=await import('node:path');
  const {execFileSync}=await import('node:child_process');
  const base=await fs.mkdtemp(path.join(os.tmpdir(),'op1-yaffs-oracle-'));
  try {
    for(const size of [0,4097,64*PAGE_SIZE+7]) {
      const root=path.join(base,`root-${size}`), content=path.join(root,'content'), nested=path.join(content,'nested');
      await fs.mkdir(nested,{recursive:true});
      const bytes=Uint8Array.from({length:size},(_,i)=>(i*37+11)&255);
      const target=path.join(nested,'data.bin'); await fs.writeFile(target,bytes);
      await fs.chmod(target,0o644); await fs.chmod(nested,0o750); await fs.chmod(content,0o755);
      for(const p of [target,nested,content,root]) await fs.utimes(p,123456,123456);
      const image=path.join(base,`image-${size}.yaffs`);
      execFileSync(process.env.MKYAFFS2,['-p','2048','-s','64','--all-root',root,image],{stdio:'pipe'});
      const reference=await fs.readFile(image), sink=new Sink();
      const result=await provisionNand([
        {name:'content',type:'directory',mode:0o755,mtime:123456},
        {name:'content/nested',type:'directory',mode:0o750,mtime:123456},
        {...file('content/nested/data.bin',bytes),mtime:123456},
      ],sink);
      assert.equal(reference.length,result.pages*(PAGE_SIZE+OOB_SIZE));
      for(let i=0;i<result.pages;i++) {
        const data=reference.subarray(i*2112,i*2112+2048);
        const oob=reference.subarray(i*2112+2048,(i+1)*2112);
        // Apply PR76's timestamp normalization; host ctime is not settable.
        if(oob.readUInt32LE(10)===0) {
          data.writeUInt32LE(data.readUInt32LE(284),280);
          data.writeUInt32LE(data.readUInt32LE(284),288);
        }
        assert.equal(hex(sink.data(i)),hex(data),`size=${size} page=${i}: complete data bytes`);
        const translated=Buffer.alloc(64,255); oob.copy(translated,4,2,30);
        assert.equal(hex(sink.oob(i)),hex(translated),`size=${size} page=${i}: complete OOB/ECC bytes`);
      }
    }
  } finally { await fs.rm(base,{recursive:true,force:true}); }
});
