import assert from "node:assert/strict";
import { afterEach, test } from "node:test";
import http from "node:http";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import { createServer, configFromEnv } from "../server.js";
import { resolveYtDlpPath, isAllowedYouTubeUrl, probeYtDlp, streamYouTubeAudio } from "../lib/youtube.js";
const servers=[];
afterEach(async()=>{while(servers.length)await new Promise(resolve=>servers.pop().close(resolve));});
async function start(config={},handler){const server=createServer(config,handler);await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));servers.push(server);return `http://127.0.0.1:${server.address().port}`;}
function spawnFixture({stdout="https://media.example/audio.m4a\n",stderr="",code=0,hold=false}={}){
  let child;
  const spawnImpl=()=>{
    child=new EventEmitter();child.stdout=new PassThrough();child.stderr=new PassThrough();child.killed=false;
    child.kill=()=>{child.killed=true;child.emit("close",null);return true;};
    if(!hold)queueMicrotask(()=>{child.stdout.end(stdout);child.stderr.end(stderr);child.emit("close",code);});
    return child;
  };
  return {spawnImpl,get child(){return child;}};
}
test("configFromEnv returns frozen config and loopback-safe default",()=>{const d=configFromEnv({});assert.equal(d.port,8787);assert.equal(d.host,"127.0.0.1");const c=configFromEnv({PORT:"9000",HOST:"192.168.1.28",YT_DLP_PATH:"/opt/bin/yt-dlp",PROXY_USER:"esp32",PROXY_PASSWORD:"secret"});assert.equal(c.port,9000);assert.equal(c.host,"192.168.1.28");assert.equal(c.ytDlpPath,"/opt/bin/yt-dlp");assert.equal(Object.isFrozen(c),true);});
test("LAN binding requires both proxy credentials",()=>{assert.throws(()=>configFromEnv({HOST:"0.0.0.0"}),/requires/);assert.throws(()=>configFromEnv({PROXY_USER:"x"}),/together/);});
test("configFromEnv rejects invalid PORT",()=>{assert.throws(()=>configFromEnv({PORT:"99999"}),/PORT/);assert.throws(()=>configFromEnv({PORT:"abc"}),/PORT/);});
test("resolveYtDlpPath supports explicit config, PlatformIO and PATH fallback",()=>{assert.equal(resolveYtDlpPath({ytDlpPath:"/custom/yt-dlp"}),"/custom/yt-dlp");const old=process.env.YT_DLP_PATH;delete process.env.YT_DLP_PATH;try{const expected="C:\\Users\\NNH\\.platformio\\penv\\Scripts\\yt-dlp.exe";assert.equal(resolveYtDlpPath({}, {platform:"win32",homeDir:"C:\\Users\\NNH",existsSync:p=>p===expected}),expected);assert.equal(resolveYtDlpPath({}, {platform:"linux",homeDir:"/none",existsSync:()=>false}),"yt-dlp");}finally{if(old===undefined)delete process.env.YT_DLP_PATH;else process.env.YT_DLP_PATH=old;}});
test("yt-dlp readiness is bounded and versioned",()=>{assert.deepEqual(probeYtDlp({ytDlpPath:"yt-dlp"},()=>({status:0,stdout:"2026.01.01\n"})),{ready:true,version:"2026.01.01"});assert.equal(probeYtDlp({},()=>{throw new Error("missing");}).ready,false);});
test("GET /healthz returns ok",async()=>{const base=await start();const res=await fetch(`${base}/healthz`);assert.equal(res.status,200);assert.equal((await res.json()).ok,true);});
test("GET /healthz reports missing yt-dlp as not ready",async()=>{const base=await start({ytDlpReady:false});const res=await fetch(`${base}/healthz`);assert.equal(res.status,503);assert.equal((await res.json()).ytDlp.ready,false);});
test("unknown route returns 404",async()=>{const base=await start();const res=await fetch(`${base}/v1/query`,{method:"POST"});assert.equal(res.status,404);assert.equal((await res.json()).error.code,"NOT_FOUND");});
test("GET /youtube/stream without query returns 400",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream`);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"INVALID_QUERY");});
test("GET /youtube/stream with blank query returns 400",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream?q=   `);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"INVALID_QUERY");});
test("GET /youtube/stream rejects oversized query before spawning yt-dlp",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream?q=${"x".repeat(129)}`);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"QUERY_TOO_LONG");});

test("stream endpoint enforces Basic auth",async()=>{const base=await start({proxyUser:"esp32",proxyPassword:"secret"});const res=await fetch(`${base}/youtube/stream?q=test`);assert.equal(res.status,401);assert.equal((await res.json()).error.code,"UNAUTHORIZED");});

test("stream endpoint enforces concurrency capacity",async()=>{let held;const handler=(_q,_req,res)=>{held=res;};const server=createServer({maxStreams:1},handler);await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));servers.push(server);const base=`http://127.0.0.1:${server.address().port}`;const first=fetch(`${base}/youtube/stream?q=one`);await new Promise(resolve=>setTimeout(resolve,20));const second=await fetch(`${base}/youtube/stream?q=two`);assert.equal(second.status,503);assert.equal((await second.json()).error.code,"CAPACITY_EXHAUSTED");held.end("done");await first;});

test("mocked yt-dlp and fetch preserve Range/206",async()=>{const child=spawnFixture();let seenRange="";const base=await start({spawnImpl:child.spawnImpl,fetchImpl:async(_url,options)=>{seenRange=options.headers.Range;return new Response(Buffer.from("audio"),{status:206,headers:{"content-type":"audio/mp4","content-range":"bytes 0-4/5","accept-ranges":"bytes","content-length":"5"}});},streamIdleTimeoutMs:100},streamYouTubeAudio);const res=await fetch(`${base}/youtube/stream?q=test`,{headers:{Range:"bytes=0-4"}});assert.equal(res.status,206);assert.equal(seenRange,"bytes=0-4");assert.equal(await res.text(),"audio");});

test("mocked HEAD resolves metadata without a response body",async()=>{const child=spawnFixture();let seenMethod="";const base=await start({spawnImpl:child.spawnImpl,fetchImpl:async(_url,options)=>{seenMethod=options.method;return new Response(null,{status:200,headers:{"content-type":"audio/mp4","content-length":"42"}});}},streamYouTubeAudio);const res=await fetch(`${base}/youtube/stream?q=test`,{method:"HEAD"});assert.equal(res.status,200);assert.equal(seenMethod,"HEAD");assert.equal(res.headers.get("content-length"),"42");assert.equal(await res.text(),"");});

test("mocked yt-dlp failure returns deterministic JSON",async()=>{const child=spawnFixture({stdout:"",stderr:"not found",code:1});const base=await start({spawnImpl:child.spawnImpl},streamYouTubeAudio);const res=await fetch(`${base}/youtube/stream?q=missing`);assert.equal(res.status,502);assert.equal((await res.json()).error.code,"YTDLP_FAILED");});

test("yt-dlp failure logs never expose signed media URLs",async()=>{
  const secretUrl="https://media.example/audio.m4a?token=super-secret";
  const child=spawnFixture({stdout:"",stderr:`ERROR ${secretUrl}`,code:1});
  const lines=[];const original=console.error;console.error=(...args)=>lines.push(args.join(" "));
  try{
    const base=await start({spawnImpl:child.spawnImpl},streamYouTubeAudio);
    const res=await fetch(`${base}/youtube/stream?q=missing`);
    assert.equal(res.status,502);await res.json();
  }finally{console.error=original;}
  assert.equal(lines.some(line=>line.includes(secretUrl)||line.includes("super-secret")),false);
});

test("invalid direct URL is rejected before spawn",async()=>{let spawned=false;const base=await start({spawnImpl:()=>{spawned=true;}},streamYouTubeAudio);const res=await fetch(`${base}/youtube/stream?q=${encodeURIComponent("http://127.0.0.1/private")}`);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"UNSUPPORTED_URL");assert.equal(spawned,false);});

test("client abort terminates a pending yt-dlp child",async()=>{const fixture=spawnFixture({hold:true});const base=await start({spawnImpl:fixture.spawnImpl},streamYouTubeAudio);await new Promise(resolve=>{const req=http.get(`${base}/youtube/stream?q=abort`);req.on("error",()=>resolve());setTimeout(()=>req.destroy(),20);});await new Promise(resolve=>setTimeout(resolve,20));assert.equal(fixture.child.killed,true);});

test("stalled upstream body hits the idle deadline and is aborted",async()=>{
  const fixture=spawnFixture();let aborted=false;
  const fetchImpl=async(_url,{signal})=>{signal.addEventListener("abort",()=>{aborted=true;},{once:true});return new Response(new ReadableStream({start(){}}),{status:200,headers:{"content-type":"audio/mp4"}});};
  const base=await start({spawnImpl:fixture.spawnImpl,fetchImpl,streamIdleTimeoutMs:20},streamYouTubeAudio);
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error("idle timeout did not close response")),1000);const done=()=>{clearTimeout(timer);resolve();};const req=http.get(`${base}/youtube/stream?q=stall`,res=>{res.on("close",done);res.on("error",done);});req.on("error",done);});
  assert.equal(aborted,true);
});

test("paused client applies backpressure and aborts upstream on disconnect",async()=>{
  const fixture=spawnFixture();let pulls=0;let aborted=false;let streamController;
  const fetchImpl=async(_url,{signal})=>{
    signal.addEventListener("abort",()=>{aborted=true;streamController?.error(new Error("aborted"));},{once:true});
    const body=new ReadableStream({start(controller){streamController=controller;},pull(controller){pulls+=1;controller.enqueue(new Uint8Array(1024*1024));if(pulls>=64)controller.close();}});
    return new Response(body,{status:200,headers:{"content-type":"audio/mp4"}});
  };
  const base=await start({spawnImpl:fixture.spawnImpl,fetchImpl,streamIdleTimeoutMs:1000},streamYouTubeAudio);
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error("backpressure test timed out")),1500);const req=http.get(`${base}/youtube/stream?q=slow-client`,res=>{res.pause();setTimeout(()=>{assert.ok(pulls<64,`producer drained ${pulls} chunks despite backpressure`);req.destroy();clearTimeout(timer);resolve();},50);});req.on("error",()=>{});});
  await new Promise(resolve=>setTimeout(resolve,20));
  assert.equal(aborted,true);
});

test("direct URL allowlist accepts YouTube only",()=>{
  assert.equal(isAllowedYouTubeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"),true);
  assert.equal(isAllowedYouTubeUrl("https://music.youtube.com/watch?v=dQw4w9WgXcQ"),true);
  assert.equal(isAllowedYouTubeUrl("https://youtu.be/dQw4w9WgXcQ"),true);
  assert.equal(isAllowedYouTubeUrl("http://127.0.0.1:8080/private"),false);
  assert.equal(isAllowedYouTubeUrl("https://youtube.com.evil.example/watch?v=x"),false);
  assert.equal(isAllowedYouTubeUrl("file:///etc/passwd"),false);
});
