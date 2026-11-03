// Immutable selection records: an incomplete write never replaces the last
// usable configuration. All consumers resolve exactly this selection.
import {writeBytes, findProfile, PROFILES} from './firmware-storage.js';
import {sha256, PLAINTEXT_246_SHA256} from './firmware-package.js';
export const CONFIGURATIONS = 'boot-configurations';
export const MANUAL_INPUTS = 'manual-inputs';
const sizes = {nand:553648128, otp:8192};
export async function inputDirectory(root, ref) {
  let dir = root;
  for (const name of ref.path) dir = await dir.getDirectoryHandle(name);
  return dir;
}
export async function inputFile(root, ref) {
  return (await (await inputDirectory(root, ref)).getFileHandle(ref.name)).getFile();
}
async function valid(root, config) {
  if (config === null) return true;
  if (!config || !['manual','firmware'].includes(config.kind)) return false;
  for (const key of ['nand','otp','ldr']) {
    const ref = config[key];
    if (!ref) continue;
    if (!Array.isArray(ref.path) || !ref.path.every(n=>typeof n==='string' && !n.includes('/')) || typeof ref.name!=='string') return false;
    if ((await inputFile(root,ref)).size !== (key==='ldr'?2171624:sizes[key])) return false;
  }
  if(config.kind==='firmware' && config.ldr && await sha256(new Uint8Array(await(await inputFile(root,config.ldr)).arrayBuffer()))!==PLAINTEXT_246_SHA256)return false;
  return config.kind==='manual' ? Boolean(config.nand||config.otp) : Boolean(config.nand&&config.ldr&&!config.otp);
}
export async function loadConfiguration(root) {
  let dir;
  try { dir = await root.getDirectoryHandle(CONFIGURATIONS); } catch { /* pre-selection storage */ }
  if (dir) {
    const records=[];
    for await (const [name,handle] of dir.entries()) {
      if (handle.kind!=='file') continue;
      try {
        const file=await handle.getFile();
        if (file.size>4096) continue;
        const record=JSON.parse(await file.text());
        if (record.schema===1 && Number.isSafeInteger(record.sequence)) records.push(record);
      } catch { /* uncommitted record */ }
    }
    records.sort((a,b)=>b.sequence-a.sequence);
    for (const record of records) {
      try { if (await valid(root,record.config)) return record; } catch { /* incomplete input */ }
    }
    if(records.length) return {sequence:0,config:null};
  }
  // One-time compatibility for existing installations: legacy manual images
  // were what Export described. Prefer them consistently, then a profile.
  const config={kind:'manual'};
  for (const key of ['nand','otp']) {
    const ref={path:[],name:`${key}.bin`};
    try { if ((await inputFile(root,ref)).size===sizes[key]) config[key]=ref; } catch {}
  }
  if (config.nand||config.otp) return {sequence:0,config};
  const profile=await findProfile(root);
  return {sequence:0,config:profile?profileConfiguration(profile):null};
}
export function profileConfiguration(profile) {
  const path=[PROFILES,profile.name];
  return {kind:'firmware',nand:{path,name:'nand.bin'},ldr:{path,name:'plaintext.ldr'},pages:profile.info.pages};
}
export async function commitConfiguration(root, previous, config, {signal} = {}) {
  const sequence=Math.max(Date.now(),(previous?.sequence??0)+1);
  const record={schema:1,sequence,config};
  const dir=await root.getDirectoryHandle(CONFIGURATIONS,{create:true});
  const name=`selection-${sequence}-${crypto.randomUUID()}.json`;
  try { await writeBytes(dir,name,new TextEncoder().encode(JSON.stringify(record)),{signal}); }
  catch (e) { await dir.removeEntry(name).catch(()=>{}); throw e; }
  return record;
}
