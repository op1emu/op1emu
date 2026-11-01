// Opt-in key source only; this reference is NOT a complete emulator OTP image.
import {parseOtpText} from './firmware-package.js';
import {writeBytes} from './firmware-storage.js';
export const OTP_URL='https://raw.githubusercontent.com/Tolsi/op1dumps/e42843c4a31ae88dbe5d7ee2413b02bd5caeff92/TE-OP-1-BF524-OTP-dump.txt';
const BLOB='82e4de41f1c8e773dff9dbdd81b0668940d2081c';
export const OTP_CACHE='firmware-otp-reference.txt';
const LIMIT=65536;
export async function validateOtpReference(raw) {
  if(!(raw instanceof Uint8Array)||raw.length>LIMIT)throw new Error('OTP reference exceeds limit');
  const prefix=new TextEncoder().encode(`blob ${raw.length}\0`);
  const object=new Uint8Array(prefix.length+raw.length);object.set(prefix);object.set(raw,prefix.length);
  const digest=new Uint8Array(await crypto.subtle.digest('SHA-1',object));
  if(Array.from(digest,n=>n.toString(16).padStart(2,'0')).join('')!==BLOB)throw new Error('OTP reference does not match the pinned source');
  const text=new TextDecoder('utf-8',{fatal:true}).decode(raw);parseOtpText(text);return text;
}
export async function getOtpReference(root,{allowFetch=false,signal,fetcher=fetch}={}) {
  try{const f=await(await root.getFileHandle(OTP_CACHE)).getFile();if(f.size<=LIMIT)return await validateOtpReference(new Uint8Array(await f.arrayBuffer()));}catch{/* optional revalidated cache */}
  if(!allowFetch)throw new Error('Encrypted firmware needs an OTP text file or permission to download the public reference');
  const timeout=AbortSignal.timeout(15000), fetchSignal=signal?AbortSignal.any([signal,timeout]):timeout;
  const response=await fetcher(OTP_URL,{signal:fetchSignal,credentials:'omit',referrerPolicy:'no-referrer'});
  if(!response.ok||!response.body)throw new Error('Could not download OTP reference; choose a local text file instead');
  const reader=response.body.getReader(),parts=[];let size=0;
  try{for(;;){const {done,value}=await reader.read();if(done)break;size+=value.length;if(size>LIMIT)throw new Error('OTP reference exceeds size limit');parts.push(value);}}
  finally{await reader.cancel().catch(()=>{});}
  const bytes=new Uint8Array(size);let at=0;for(const p of parts){bytes.set(p,at);at+=p.length;}
  const text=await validateOtpReference(bytes);signal?.throwIfAborted();await writeBytes(root,OTP_CACHE,bytes);return text;
}
