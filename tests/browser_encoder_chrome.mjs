/** Real Chrome + production TS/WASM + production C API (controlled HTTP boundary).
 * Run after test_encoder_native_runtime.py --build-only PATH; no production access.
 * CHROME_PATH and ENCODER_BRIDGE point to installed Chrome and the WSL fixture.
 */
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createWriteStream } from "node:fs";
import { mkdir, writeFile, readFile } from "node:fs/promises";
import { createInterface } from "node:readline";
import { pipeline } from "node:stream/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const directory = path.join(root, ".git", "encoder-chrome-e2e");
await mkdir(directory, { recursive: true });
const unix = (value) =>
  value
    .replace(/^([A-Za-z]):/, (_, drive) => `/mnt/${drive.toLowerCase()}`)
    .replaceAll("\\", "/");
const bridgePath = process.env.ENCODER_BRIDGE;
assert(bridgePath, "ENCODER_BRIDGE must identify the native test executable");
const library = path.join(directory, `library-${Date.now()}`);
await mkdir(library);
const bridge = spawn(
  "wsl.exe",
  ["-d", "Ubuntu", "--", bridgePath, "--bridge", unix(library)],
  { windowsHide: true, stdio: ["pipe", "pipe", "pipe"] },
);
bridge.stderr.on("data", (data) => process.stderr.write(data));
let pending;
createInterface({ input: bridge.stdout }).on("line", (line) => {
  if (!line.startsWith("{")) return;
  try {
    const reply = JSON.parse(line);
    pending?.resolve(reply);
    pending = undefined;
  } catch (error) {
    pending?.reject(error);
    pending = undefined;
  }
});
function command(value) {
  assert(!pending);
  assert.equal(bridge.exitCode, null, "native fixture must still be running");
  return new Promise((resolve, reject) => {
    pending = { resolve, reject };
    bridge.stdin.write(JSON.stringify(value) + "\n");
  });
}
bridge.on("exit", (code) => {
  pending?.reject(new Error(`bridge exited: ${code}`));
  pending = undefined;
});

const { createServer } = await import(
  pathToFileURL(
    path.join(root, "teddycloud_web/node_modules/vite/dist/node/index.js"),
  )
);
let requestId = 0;
const failures = [];
const seconds = Number(process.env.ENCODER_TEST_SECONDS || 40);
const failureOnly = process.env.ENCODER_TEST_MODE === "failures";
const fixture = Buffer.alloc(44 + Math.round(seconds * 48000) * 4);
fixture.write("RIFF");
fixture.writeUInt32LE(fixture.length - 8, 4);
fixture.write("WAVEfmt ", 8);
fixture.writeUInt32LE(16, 16);
fixture.writeUInt16LE(1, 20);
fixture.writeUInt16LE(2, 22);
fixture.writeUInt32LE(48000, 24);
fixture.writeUInt32LE(192000, 28);
fixture.writeUInt16LE(4, 32);
fixture.writeUInt16LE(16, 34);
fixture.write("data", 36);
fixture.writeUInt32LE(fixture.length - 44, 40);
let seed = 123456789;
for (let i = 44; i < fixture.length; i += 2) {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  const frame = Math.floor((i - 44) / 4);
  const sample =
    frame > seconds * 48000 - 9600
      ? Math.sin((frame * 2 * Math.PI * 1000) / 48000) * 24000
      : (seed / 4294967296 - 0.5) * 45000;
  fixture.writeInt16LE(Math.round(sample), i);
}
const testPage = `<!doctype html><title>Encoder Chrome regression</title><body><pre id="status">Starting</pre><script type="module">
import {encodeBrowserAudio} from '/web/src/utils/audio/wasmEncoder.ts';
import {supportsBrowserEncoding} from '/web/src/utils/audio/encoderPreflight.ts';
window.test = {state:'running', results:[], progress:'starting', errors:[]};
const status = document.querySelector('#status');
async function json(url, options={}) {
 const response=await fetch(url,options); const text=await response.text();
 if(!response.ok) throw new Error(response.status+': '+text);
 try {return text?JSON.parse(text):{};} catch {return {text};}
}
try {
 if(!supportsBrowserEncoding()) throw new Error('OPFS unavailable in real Chrome');
 const data=await (await fetch('/fixture.wav')).blob();
 const files=Array.from({length:114},(_,i)=>new File([data],String(i+1).padStart(3,'0')+' chapter.wav',{type:'audio/wav'}));
 const options={audioId:1700000000,bitrate:192,maxBytes:2147418111,signal:new AbortController().signal,
 onProgress:(i,total,name)=>{window.test.progress=i+'/'+total+' '+name;status.textContent=window.test.progress;}};
 const metadata=await json('/api/encoder/capabilities');
 if(!metadata.formats.native.enabled) throw new Error('native unavailable');
 for(const format of ${JSON.stringify(failureOnly ? [] : ["taf", "native"])}) {
   let bytes=0; const started=performance.now();
   const memoryStart=performance.memory?.usedJSHeapSize;
   let maxHeap=memoryStart||0;
   const sample=setInterval(()=>{maxHeap=Math.max(maxHeap,performance.memory?.usedJSHeapSize||0)},100);
   const upload=format==='native'?await json('/api/library/native/import',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({title:'Chrome 114 chapters',chapters:files.map(file=>({title:file.name}))})}):null;
   await encodeBrowserAudio(files,{...options,format,onFile:async(file,index,duration)=>{
     bytes+=file.size;
     if(format==='taf') {
       const form=new FormData();form.append('file',file,'browser-upload.taf');
       await json('/api/tafUpload',{method:'POST',body:form});
     } else {
       await json('/api/library/native/import/'+upload.id+'/chapter/'+index,{method:'POST',body:file});
       if(index===113) {
         const decoder=new AudioContext({sampleRate:48000});
         const audio=await decoder.decodeAudioData(await file.arrayBuffer());
         if(Math.abs(audio.duration-${seconds})>.002) throw new Error('native duration/end mismatch '+audio.duration);
         const pcm=audio.getChannelData(0);let energy=0;
         for(let i=pcm.length-4800;i<pcm.length;i++) energy+=pcm[i]*pcm[i];
         if(energy/4800<.05) throw new Error('native final tone missing');
         await decoder.close();
       }
     }
   }});
   const finish=upload?await json('/api/library/native/import/'+upload.id+'/finish',{method:'POST'}):{};
   clearInterval(sample);
   if(bytes<=80*1024*1024 && ${seconds}>=40) throw new Error('output did not exceed 80 MiB: '+bytes);
   window.test.results.push({format,bytes,seconds:performance.now()/1000-started/1000,maxHeap,memoryStart,...finish});
 }
 // A real worker/OPFS size failure must not reach the upload callback.
 let callbacks=0, rejected=false;
 try {await encodeBrowserAudio(files.slice(0,1),{...options,format:'taf',maxBytes:4097,onFile:async()=>{callbacks++}});} catch(error){rejected=true;}
 if(!rejected||callbacks) throw new Error('write failure was reported as success');
 window.test.results.push({controlledWriteFailure:true,callbacks});
 const wrapper=new Blob([
   "FileSystemSyncAccessHandle.prototype.write=function(){throw new DOMException('Controlled quota failure','QuotaExceededError')};"+
   "await import("+JSON.stringify(location.origin+'/web/src/utils/audio/encoder.worker.ts')+");postMessage({ready:true});"
 ],{type:'text/javascript'});
 const wrapperUrl=URL.createObjectURL(wrapper);const quotaWorker=new Worker(wrapperUrl,{type:'module'});
 await new Promise((resolve,reject)=>{quotaWorker.onmessage=resolve;quotaWorker.onerror=reject;});
 let requestId=0;
 const call=command=>new Promise(resolve=>{quotaWorker.onmessage=event=>resolve(event.data);quotaWorker.postMessage({...command,requestId:++requestId});});
 try {
   const init=await call({type:'init',moduleUrl:location.origin+'/web/wasm/audio_encoder.js?encoder=stream-v1',jobId:'job-'+crypto.randomUUID(),format:'taf',maxBytes:2147418111});
   if(!init.ok)throw new Error(init.error);
   const begin=await call({type:'begin',filename:'audio.taf',audioId:1700000000,bitrate:192});
   if(begin.ok||!/quota/i.test(begin.error)) throw new Error('quota exception was not propagated '+JSON.stringify(begin));
   window.test.results.push({controlledQuotaFailure:true,error:begin.error});
 } finally {await call({type:'cleanup'});quotaWorker.terminate();URL.revokeObjectURL(wrapperUrl);}
 window.test.state='passed';status.textContent=JSON.stringify(window.test,null,2);
} catch(error) {window.test.state='failed';window.test.errors.push(error.stack||String(error));status.textContent=JSON.stringify(window.test,null,2);}
</script>`;
const server = await createServer({
  configFile: false,
  root: path.join(root, "teddycloud_web"),
  base: "/web/",
  server: { host: "127.0.0.1", port: 0, open: false, strictPort: false },
  plugins: [
    {
      name: "encoder-native-test-boundary",
      configureServer(server) {
        server.middlewares.use(async (req, res, next) => {
          const uri = req.url.split("?")[0];
          if (uri === "/fixture.wav") {
            res.setHeader("Content-Type", "audio/wav");
            res.end(fixture);
            return;
          }
          if (uri === "/test") {
            res.setHeader("Content-Type", "text/html");
            res.end(testPage);
            return;
          }
          if (!uri.startsWith("/api/")) return next();
          try {
            const bodyFile = path.join(
              directory,
              `request-${++requestId}.body`,
            );
            await pipeline(req, createWriteStream(bodyFile));
            const boundary = /boundary=(?:"([^"]+)"|([^;]+))/.exec(
              req.headers["content-type"] || "",
            );
            const reply = await command({
              method: req.method,
              uri,
              bodyFile: unix(bodyFile),
              boundary: boundary?.[1] || boundary?.[2] || "",
            });
            res.statusCode = reply.status;
            res.setHeader("Content-Type", "application/json");
            res.end(
              typeof reply.body === "string"
                ? reply.body
                : JSON.stringify(reply.body),
            );
            if (reply.status >= 400) failures.push({ uri, ...reply });
          } catch (error) {
            failures.push(String(error));
            res.statusCode = 500;
            res.end(JSON.stringify({ error: String(error) }));
          }
        });
      },
    },
  ],
});
let chrome;
let socket;
try {
  await server.listen();
  const url = `http://127.0.0.1:${server.httpServer.address().port}/test`;
  const port = Number(process.env.CHROME_DEBUG_PORT || 19327);
  chrome = spawn(
    process.env.CHROME_PATH ||
      "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    [
      "--headless=new",
      "--disable-gpu",
      "--no-first-run",
      "--no-default-browser-check",
      "--autoplay-policy=no-user-gesture-required",
      "--enable-precise-memory-info",
      `--remote-debugging-port=${port}`,
      `--user-data-dir=${path.join(directory, "profile-" + Date.now())}`,
      "about:blank",
    ],
    { windowsHide: true, stdio: "ignore" },
  );
  let tabs;
  for (let attempt = 0; attempt < 50; attempt++) {
    try {
      tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      if (tabs.length) break;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  assert(tabs?.length, "Chrome debugging target unavailable");
  socket = new WebSocket(
    tabs.find((tab) => tab.type === "page").webSocketDebuggerUrl,
  );
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });
  let id = 0;
  const waiting = new Map();
  const workers = new Set();
  const workerMemory = [];
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (
      message.method === "Target.attachedToTarget" &&
      message.params.targetInfo.type === "worker"
    )
      workers.add(message.params.sessionId);
    if (message.method === "Target.detachedFromTarget")
      workers.delete(message.params.sessionId);
    if (message.id) {
      const task = waiting.get(message.id);
      waiting.delete(message.id);
      message.error ? task.reject(message.error) : task.resolve(message.result);
    }
  };
  const cdp = (method, params = {}, sessionId) =>
    new Promise((resolve, reject) => {
      waiting.set(++id, { resolve, reject });
      socket.send(JSON.stringify({ id, method, params, sessionId }));
    });
  await cdp("Target.setAutoAttach", {
    autoAttach: true,
    waitForDebuggerOnStart: false,
    flatten: true,
  });
  await cdp("Page.navigate", { url });
  const started = Date.now();
  let result;
  while (Date.now() - started < 45 * 60 * 1000) {
    await new Promise((resolve) => setTimeout(resolve, 3000));
    result = (
      await cdp("Runtime.evaluate", {
        expression: "JSON.stringify(window.test)",
        returnByValue: true,
      })
    ).result.value;
    if (!result) continue;
    result = JSON.parse(result);
    for (const worker of workers) {
      try {
        workerMemory.push({
          time: Date.now(),
          progress: result.progress,
          ...(await cdp("Runtime.getHeapUsage", {}, worker)),
        });
      } catch (error) {
        workerMemory.push({ error });
      }
    }
    console.log(
      JSON.stringify({
        state: result.state,
        progress: result.progress,
        completed: result.results?.length,
      }),
    );
    if (result.state !== "running") break;
  }
  const chromeVersion = await (
    await fetch(`http://127.0.0.1:${port}/json/version`)
  ).json();
  await writeFile(
    path.join(directory, failureOnly ? "result-failures.json" : "result.json"),
    JSON.stringify(
      {
        chrome: chromeVersion.Browser,
        library,
        result,
        failures,
        workerMemory,
      },
      null,
      2,
    ),
  );
  console.log(JSON.stringify({ library, result, failures }, null, 2));
  assert.equal(result.state, "passed");
  assert.equal(failures.length, 0);
} finally {
  socket?.close();
  chrome?.kill();
  bridge.stdin.end();
  bridge.kill();
  await server.close();
}
