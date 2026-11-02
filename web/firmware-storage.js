// New OPFS generations commit last; never overwrite the user's legacy inputs.
import {sha256, PLAINTEXT_246_SHA256} from './firmware-package.js';
export const PROFILES = 'firmware-profiles';
export const NAND_BYTES = 553648128;
export async function writeBytes(dir, name, bytes, {signal} = {}) {
  const handle = await (await dir.getFileHandle(name, {create:true})).createSyncAccessHandle();
  try {
    signal?.throwIfAborted();
    handle.truncate(0);
    for (let at=0; at<bytes.length;) {
      const count=handle.write(bytes.subarray(at),{at});
      if (!Number.isInteger(count) || count<=0 || count>bytes.length-at) throw new Error('Storage write made no progress');
      at+=count;
    }
    handle.flush();
  } finally {handle.close();}
}
export async function findProfile(root) {
  let profiles;
  try {profiles=await root.getDirectoryHandle(PROFILES);} catch{return null;}
  const valid=[];
  for await(const [name,dir] of profiles.entries()) {
    if(dir.kind!=='directory'||!/^profile-[0-9]+-[a-f0-9-]+$/.test(name))continue;
    try {
      const commit=await(await dir.getFileHandle('complete.json')).getFile();
      if(commit.size>4096)continue;
      const info=JSON.parse(await commit.text());
      if(info.schema!==1||info.loaderHash!==PLAINTEXT_246_SHA256||!Number.isSafeInteger(info.created))continue;
      const nand=await(await dir.getFileHandle('nand.bin')).getFile();
      const ldr=await(await dir.getFileHandle('plaintext.ldr')).getFile();
      if(nand.size!==NAND_BYTES||ldr.size!==2171624)continue;
      if(await sha256(new Uint8Array(await ldr.arrayBuffer()))!==info.loaderHash)continue;
      valid.push({name,dir,info});
    }catch{/* incomplete, corrupt, or locked generation */}
  }
  return valid.sort((a,b)=>b.info.created-a.info.created||b.name.localeCompare(a.name))[0]??null;
}
export async function stageProfile(root,loader,entries,provisionNand,{signal,onProgress}={}) {
  const profiles=await root.getDirectoryHandle(PROFILES,{create:true});
  const name=`profile-${Date.now()}-${crypto.randomUUID()}`;
  const dir=await profiles.getDirectoryHandle(name,{create:true});
  let committed=false;
  try {
    const nand=await(await dir.getFileHandle('nand.bin',{create:true})).createSyncAccessHandle();
    let result;try{result=await provisionNand(entries,nand,{signal,onProgress});}finally{nand.close();}
    signal?.throwIfAborted();
    await writeBytes(dir,'plaintext.ldr',loader);
    const loaderHash=await sha256(loader);
    if(loaderHash!==PLAINTEXT_246_SHA256)throw new Error('Loader identity changed before storage');
    signal?.throwIfAborted();
    const info={schema:1,created:Date.now(),loaderHash,pages:result.pages,source:'original-op1-246'};
    await writeBytes(dir,'complete.json',new TextEncoder().encode(JSON.stringify(info)));
    committed=true;return {name,dir,info};
  }finally{if(!committed)await profiles.removeEntry(name,{recursive:true}).catch(()=>{});}
}
