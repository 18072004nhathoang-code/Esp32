import { spawn, spawnSync } from "node:child_process";
import { Readable } from "node:stream";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const EXTRACT_TIMEOUT_MS=15000;
const MAX_STDOUT_BYTES=64*1024;
const MAX_STDERR_BYTES=16*1024;

export function isAllowedYouTubeUrl(value) {
  try {
    const url=new URL(String(value));
    if(url.protocol!=="http:"&&url.protocol!=="https:") return false;
    const host=url.hostname.toLowerCase();
    return host==="youtu.be"||host==="youtube.com"||host.endsWith(".youtube.com");
  } catch { return false; }
}

export function resolveYtDlpPath(config={}, runtime={}) {
  const configured=String(config.ytDlpPath||process.env.YT_DLP_PATH||"").trim();
  if(configured) return configured;
  const platform=runtime.platform||process.platform;
  const homeDir=runtime.homeDir||os.homedir();
  const existsSync=runtime.existsSync||fs.existsSync;
  const join=platform==="win32"?path.win32.join:path.posix.join;
  const candidates=platform==="win32"
    ? [join(homeDir,".platformio","penv","Scripts","yt-dlp.exe")]
    : [join(homeDir,".platformio","penv","bin","yt-dlp"),join(homeDir,".local","bin","yt-dlp")];
  for(const candidate of candidates){
    try{if(existsSync(candidate)) return candidate;}catch{}
  }
  return "yt-dlp";
}
export function probeYtDlp(config={}, spawnSyncImpl=spawnSync) {
  const executable=resolveYtDlpPath(config);
  try{
    const result=spawnSyncImpl(executable,["--version"],{encoding:"utf8",timeout:5000,windowsHide:true});
    const version=String(result?.stdout||"").trim().split(/\r?\n/)[0].slice(0,64);
    return Object.freeze({ready:result?.status===0&&Boolean(version),version});
  }catch{return Object.freeze({ready:false,version:""});}
}
function sendError(res,status,code,message){
  if(res.destroyed||res.writableEnded||res.headersSent) return;
  const body=Buffer.from(JSON.stringify({error:{code,message}}));
  res.writeHead(status,{"content-type":"application/json; charset=utf-8","content-length":body.length,"cache-control":"no-store"});
  res.end(body);
}
export function streamYouTubeAudio(query,req,res,config={}){
  const ytdlpPath=resolveYtDlpPath(config);
  const directUrl=/^https?:\/\//i.test(query);
  if(directUrl&&!isAllowedYouTubeUrl(query))
    return sendError(res,400,"UNSUPPORTED_URL","Chỉ chấp nhận URL YouTube trực tiếp.");
  const searchPattern=directUrl?query:`ytsearch1:${query}`;
  const requestId=config.requestId||"-";
  console.log(`[YOUTUBE ${requestId}] Resolving request`);
  const args=["--no-warnings","--no-playlist","--no-progress","--socket-timeout","10","-g","-f","ba[ext=m4a]/ba",searchPattern];
  const child=(config.spawnImpl||spawn)(ytdlpPath,args,{windowsHide:true,stdio:["ignore","pipe","pipe"]});
  let stdoutData="",stderrData="",terminal=false;
  const killChild=()=>{try{if(!child.killed) child.kill("SIGTERM");}catch{}};
  let extractTimer;
  const cleanup=()=>{res.removeListener("close",onClientGone);req.removeListener("aborted",onClientGone);if(extractTimer)clearTimeout(extractTimer);};
  const onClientGone=()=>{if(terminal)return;terminal=true;cleanup();killChild();};
  const fail=(status,code,message)=>{if(terminal)return;terminal=true;cleanup();killChild();sendError(res,status,code,message);};
  res.once("close",onClientGone); req.once("aborted",onClientGone);
  extractTimer=setTimeout(()=>fail(504,"YTDLP_TIMEOUT","yt-dlp phản hồi quá chậm"),EXTRACT_TIMEOUT_MS); extractTimer.unref?.();
  child.stdout.setEncoding("utf8"); child.stderr.setEncoding("utf8");
  child.stdout.on("data",chunk=>{
    if(terminal)return;
    if(Buffer.byteLength(stdoutData,"utf8")+Buffer.byteLength(chunk,"utf8")>MAX_STDOUT_BYTES)
      return fail(502,"YTDLP_OUTPUT_TOO_LARGE","yt-dlp trả về dữ liệu bất thường");
    stdoutData+=chunk;
  });
  child.stderr.on("data",chunk=>{
    if(terminal)return;
    const room=MAX_STDERR_BYTES-Buffer.byteLength(stderrData,"utf8");
    if(room>0) stderrData+=chunk.slice(0,room);
  });
  child.once("error",err=>{console.error("[YOUTUBE] Spawn error:",err.message);fail(500,"YTDLP_START_FAILED","Không khởi động được yt-dlp");});
  child.once("close",async code=>{
    if(terminal)return; terminal=true; cleanup();
    if(code!==0){
      console.error(`[YOUTUBE ${requestId}] yt-dlp exited with code ${code}: ${stderrData.trim().slice(0,512)}`);
      return sendError(res,502,"YTDLP_FAILED","Không tìm thấy hoặc không trích xuất được bài hát YouTube");
    }
    const streamUrl=stdoutData.split(/\r?\n/).map(x=>x.trim()).find(x=>/^https?:\/\//i.test(x));
    if(!streamUrl) return sendError(res,404,"STREAM_URL_MISSING","Không lấy được link stream YouTube");
    const controller=new AbortController();
    const abortUpstream=()=>controller.abort();
    res.once("close",abortUpstream); req.once("aborted",abortUpstream);
    const fetchTimer=setTimeout(()=>controller.abort(),EXTRACT_TIMEOUT_MS); fetchTimer.unref?.();
    try{
      const headers={"User-Agent":"Mozilla/5.0 (ESP32 Mini OS YouTube Proxy)","Accept":"*/*"};
      if(req.headers.range) headers.Range=req.headers.range;
      const upstream=await (config.fetchImpl||fetch)(streamUrl,{method:req.method==="HEAD"?"HEAD":"GET",signal:controller.signal,headers,redirect:"follow"});
      clearTimeout(fetchTimer);
      if(!upstream.ok){
        res.removeListener("close",abortUpstream); req.removeListener("aborted",abortUpstream);
        console.error(`[YOUTUBE] Upstream returned status ${upstream.status}`);
        return sendError(res,502,"UPSTREAM_REJECTED","Máy chủ âm thanh YouTube từ chối yêu cầu");
      }
      const out={"Content-Type":upstream.headers.get("content-type")||"audio/mp4","Cache-Control":"no-cache, no-store","Access-Control-Allow-Origin":"*"};
      for(const h of ["content-length","content-range","accept-ranges"]){const v=upstream.headers.get(h);if(v)out[h]=v;}
      res.writeHead(upstream.status,out);
      if(req.method==="HEAD"||!upstream.body){
        res.removeListener("close",abortUpstream); req.removeListener("aborted",abortUpstream);
        if(upstream.body) await upstream.body.cancel().catch(()=>{});
        return res.end();
      }
      const nodeStream=Readable.fromWeb(upstream.body);
      let idleTimer;
      const clearIdle=()=>{if(idleTimer)clearTimeout(idleTimer);idleTimer=undefined;};
      const resetIdle=()=>{clearIdle();idleTimer=setTimeout(()=>{controller.abort();nodeStream.destroy(new Error("upstream idle timeout"));},config.streamIdleTimeoutMs||30000);idleTimer.unref?.();};
      nodeStream.on("data",resetIdle);
      nodeStream.once("end",clearIdle);
      res.once("close",clearIdle);
      resetIdle();
      nodeStream.once("error",err=>{console.error("[YOUTUBE] Stream pipe error:",err.message);controller.abort();if(!res.destroyed)res.destroy(err);});
      nodeStream.pipe(res);
    }catch(err){
      clearTimeout(fetchTimer); res.removeListener("close",abortUpstream); req.removeListener("aborted",abortUpstream);
      if(err?.name!=="AbortError")console.error("[YOUTUBE] Fetch exception:",err);
      sendError(res,502,"UPSTREAM_UNAVAILABLE","Không kết nối được tới máy chủ âm thanh YouTube");
    }
  });
}
