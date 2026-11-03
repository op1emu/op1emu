// Only used after complete stock-246 package authentication. The pinned decoder
// lacks consumed-byte and incremental allocation bounds; never use this adapter
// for arbitrary compressed inputs. A disposable worker enables cancellation.
import decoderUrl from 'lzma/src/lzma-d.js?url';
export function decompressLzma(bytes,{maxOutputBytes,signal}={}) {
  return new Promise((resolve,reject)=>{
    const worker=new Worker(decoderUrl);let timer,finished=false;
    const finish=(error,value)=>{if(finished)return;finished=true;clearTimeout(timer);signal?.removeEventListener('abort',abort);worker.terminate();error?reject(error):resolve(value);};
    const abort=()=>finish(new DOMException('Firmware preparation cancelled','AbortError'));
    if(signal?.aborted)return abort();signal?.addEventListener('abort',abort,{once:true});
    timer=setTimeout(()=>finish(new Error('Firmware decompression timed out')),120000);
    worker.onerror=()=>finish(new Error('LZMA decoder failed'));
    worker.onmessage=({data})=>{
      if(data.action!==2||data.cbn!==1)return;
      if(data.error)return finish(new Error('Invalid LZMA firmware payload'));
      const out=typeof data.result==='string'?new TextEncoder().encode(data.result):Uint8Array.from(data.result??[]);
      if(out.length>maxOutputBytes)return finish(new Error('Decompressed firmware exceeds limit'));
      finish(null,{bytes:out});
    };
    worker.postMessage({action:2,cbn:1,data:bytes});
  });
}
