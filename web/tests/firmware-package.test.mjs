import test from 'node:test';
import assert from 'node:assert/strict';
import {webcrypto} from 'node:crypto';
import {crc32,parseTar,extractFirmware,extractFirmware246,parseOtpText,parseKeyHex,decryptXteaBlock,decryptLoader,validateFirmware246} from '../firmware-package.js';
if (!globalThis.crypto) globalThis.crypto = webcrypto;
const cat=(...chunks)=>Uint8Array.from(chunks.flatMap(a=>Array.from(a)));
const otp='!0xd0 0000000000000000 0000000000000000';
const cipher=Uint8Array.from([0xd8,0xd4,0xe9,0xde,0xd9,0x1e,0x13,0xf7]);
function hdr(flags,count=0) {
 const b=new Uint8Array(16), d=new DataView(b.buffer);
 d.setUint16(0,flags,true);b[3]=0xad;d.setUint32(4,0xff90021c,true);d.setUint32(8,count,true);
 b[2]=b.reduce((a,b)=>a^b,0);return b;
}
function tar(members) {
 const out=[];
 for(const [name,data,type=48] of members) {
  const h=new Uint8Array(512),text=(offset,s)=>h.set(new TextEncoder().encode(s),offset);
  text(0,name);text(100,'0000644\0');text(124,data.length.toString(8).padStart(11,'0')+'\0');text(136,'00000000001\0');
  h[156]=type;h.fill(32,148,156);text(148,h.reduce((a,b)=>a+b,0).toString(8).padStart(6,'0')+'\0 ');
  out.push(h,data,new Uint8Array((512-data.length%512)%512));
 }
 return cat(...out,new Uint8Array(1024));
}
function pkg(length) {
 const c=new Uint8Array(14),d=new DataView(c.buffer);c[0]=93;d.setUint32(1,4096,true);d.setBigUint64(5,BigInt(length),true);
 const prefix=new Uint8Array(4);new DataView(prefix.buffer).setUint32(0,crc32(c),true);return cat(prefix,c);
}
test('CRC32 standard vector',()=>assert.equal(crc32(new TextEncoder().encode('123456789')),0xcbf43926));
test('XTEA independent known-answer vectors, display word order',()=>{
 for(const [key,encrypted,plain] of [
 ['00000000000000000000000000000000',cipher,new Uint8Array(8)],
 ['00000001000000020000000300000004',[0x41,0xab,0x97,0xa5,0x72,0x4d,0x01,0x76],new Uint8Array(8)],
 ['00000003000000040000000500000006',[0xa9,0x5d,0xfd,0xb1,0xdc,0xc9,0x6d,0xcc],[1,0,0,0,2,0,0,0]]]) {
 assert.deepEqual(decryptXteaBlock(Uint8Array.from(encrypted),parseKeyHex(key)),Uint8Array.from(plain));
 }
 assert.deepEqual(parseOtpText('!0xcf ignored\n0xD0 0000000100000002 0000000300000004'),parseKeyHex('00000001000000020000000300000004'));
});
test('OTP missing/malformed/duplicate rejected without echo',()=>{
 for(const text of ['', '!0xd0 secret',otp+'\n'+otp,'!0xd0 0000000000000000 000000000000000g']) assert.throws(()=>parseOtpText(text));
});
test('all callback tails, preserved indirect flag and trailer, no mutation',()=>{
 for(let length=0;length<=80;length++) {
  const payload=new Uint8Array(length).fill(90),expected=payload.slice();let units=0;
  for(let p=0;p+8<=length;p+=24){payload.set(cipher,p);expected.fill(0,p,p+8);units++;}
  const input=cat(hdr(0x2406,length),payload,hdr(0x8006),[1,2,3]),saved=input.slice();
  const result=decryptLoader(input,otp);assert.deepEqual(input,saved);
  assert.deepEqual(result.bytes,cat(hdr(0x2006,length),expected,hdr(0x8006),[1,2,3]));
  assert.equal(result.report.decryptedUnits,units);assert.equal(result.report.trailerBytes,3);
  assert.equal(result.report.encryptedTailUnits,Number(length%24>=8));
  assert.deepEqual(decryptLoader(result.bytes).bytes,result.bytes);
 }
});
test('loader errors fail closed',()=>{
 for(const input of [new Uint8Array(),hdr(0x0006),cat(hdr(0x0406),hdr(0x8006)),cat(hdr(0x2506),hdr(0x8006)),hdr(0x8006,200)])assert.throws(()=>decryptLoader(input,otp));
 assert.throws(()=>decryptLoader(cat(hdr(0x2406,4),[1,2,3,4],hdr(0x8006))),/OTP/);
 const broken=hdr(0x8006);broken[2]^=1;assert.throws(()=>decryptLoader(broken),/checksum/);
 assert.equal(decryptLoader(cat(hdr(0x0106,0xffffffff),hdr(0x8006))).report.fillRecords,1);
});
test('TAR names, metadata and bounds',()=>{
 const archive=tar([['./OP1_vdk.ldr',hdr(0x8006)],['content/',new Uint8Array(),53],['content/a',[1,2]]]);
 const entries=parseTar(archive);assert.equal(entries[0].name,'OP1_vdk.ldr');assert.equal(entries[0].mode,0o644);assert.equal(entries[0].mtime,1);assert.equal(entries[1].type,'directory');
 for(const bad of [tar([['../x',[]]]),tar([['/x',[]]]),tar([['a',[]],['./a',[]]]),tar([['a',[],50]]),archive.subarray(0,archive.length-1)])assert.throws(()=>parseTar(bad));
 const corrupt=archive.slice();corrupt[0]^=1;assert.throws(()=>parseTar(corrupt),/checksum/);
 const trailing=archive.slice();trailing[trailing.length-1]=1;assert.throws(()=>parseTar(trailing),/end marker/);
});
test('package injection contract, CRC first, lengths and decoder limits',async()=>{
 const archive=tar([['OP1_vdk.ldr',hdr(0x8006)]]),input=pkg(archive.length);
 const decode=async(compressed,limits)=>{assert.equal(limits.maxOutputBytes,256*1024*1024);return {bytes:archive,bytesRead:compressed.length};};
 const result=await extractFirmware(input,{decompressLzma:decode});assert.equal(result.format,'op1');assert.deepEqual(result.loader,hdr(0x8006));
 const corrupt=input.slice();corrupt[0]^=1;await assert.rejects(extractFirmware(corrupt,{decompressLzma:()=>{throw new Error('must not run');}}),/CRC32/);
 await assert.rejects(extractFirmware(input,{decompressLzma:async()=>({bytes:archive,bytesRead:0})}),/after LZMA/);
 await assert.rejects(extractFirmware(pkg(1),{decompressLzma:decode}),/length mismatch/);
 assert.equal((await extractFirmware(hdr(0x8006))).format,'ldr');
});
test('canonical verification rejects synthetic loader',async()=>await assert.rejects(validateFirmware246(hdr(0x8006)),/SHA-256 mismatch/));
test('production wrapper rejects unknown package before decoder invocation',async()=>{
 await assert.rejects(extractFirmware246(pkg(0),{decompressLzma:()=>{throw new Error('decoder must not run');}}),/Unsupported firmware/);
});
