import http from "node:http";
import fs from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { streamYouTubeAudio, probeYtDlp } from "./lib/youtube.js";

const MAX_YOUTUBE_QUERY_BYTES = 128;

function loadEnvFile(path) {
  if (!fs.existsSync(path)) return;
  for (const line of fs.readFileSync(path, "utf8").split(/\r?\n/)) {
    const match = line.match(/^\s*([A-Z][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (!match || Object.hasOwn(process.env, match[1])) continue;
    let value = match[2];
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) value = value.slice(1, -1);
    process.env[match[1]] = value;
  }
}
function integerSetting(value, fallback, minimum, maximum, name) {
  const parsed = Number(value ?? fallback);
  if (!Number.isSafeInteger(parsed) || parsed < minimum || parsed > maximum)
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  return parsed;
}
export function configFromEnv(env = process.env) {
  const host=env.HOST||"127.0.0.1";
  const proxyUser=env.PROXY_USER||"";
  const proxyPassword=env.PROXY_PASSWORD||"";
  if(Boolean(proxyUser)!==Boolean(proxyPassword))throw new Error("PROXY_USER and PROXY_PASSWORD must be set together");
  if(host!=="127.0.0.1"&&host!=="::1"&&host!=="localhost"&&(!proxyUser||!proxyPassword))
    throw new Error("LAN binding requires PROXY_USER and PROXY_PASSWORD");
  return Object.freeze({
    port: integerSetting(env.PORT, 8787, 1, 65535, "PORT"),
    host,
    ytDlpPath: env.YT_DLP_PATH || "",
    proxyUser,
    proxyPassword,
    maxStreams: integerSetting(env.MAX_STREAMS,2,1,8,"MAX_STREAMS"),
    streamIdleTimeoutMs: integerSetting(env.STREAM_IDLE_TIMEOUT_MS,30000,5000,120000,"STREAM_IDLE_TIMEOUT_MS"),
  });
}
function sendJson(res, status, value) {
  if (res.destroyed || res.writableEnded) return;
  const body = Buffer.from(JSON.stringify(value));
  res.writeHead(status, {"content-type":"application/json; charset=utf-8","content-length":body.length,"cache-control":"no-store"});
  res.end(body);
}
function equalSecret(actual,expected){
  const a=Buffer.from(actual);const b=Buffer.from(expected);
  return a.length===b.length&&crypto.timingSafeEqual(a,b);
}
function authorized(req,config){
  if(!config.proxyUser&&!config.proxyPassword)return true;
  const header=req.headers.authorization||"";
  if(!header.startsWith("Basic "))return false;
  let decoded="";try{decoded=Buffer.from(header.slice(6),"base64").toString("utf8");}catch{return false;}
  const separator=decoded.indexOf(":");
  return separator>=0&&equalSecret(decoded.slice(0,separator),config.proxyUser)&&
    equalSecret(decoded.slice(separator+1),config.proxyPassword);
}
export function createServer(config = {}, streamHandler = streamYouTubeAudio) {
  let activeStreams=0;
  let nextRequestId=0;
  return http.createServer((req,res)=>{
    if(req.method==="GET" && req.url==="/healthz"){
      const ready=config.ytDlpReady!==false;
      return sendJson(res,ready?200:503,{ok:ready,ytDlp:{ready,version:config.ytDlpVersion||""},activeStreams,capacity:config.maxStreams||2});
    }
    let parsedUrl;
    try { parsedUrl=new URL(req.url||"/","http://localhost"); }
    catch { return sendJson(res,400,{error:{code:"INVALID_URL",message:"URL yêu cầu không hợp lệ."}}); }
    if((req.method==="GET"||req.method==="HEAD") && parsedUrl.pathname==="/youtube/stream"){
      if(!authorized(req,config)){
        res.setHeader("www-authenticate",'Basic realm="esp32-youtube"');
        return sendJson(res,401,{error:{code:"UNAUTHORIZED",message:"Xác thực proxy không hợp lệ."}});
      }
      const q=(parsedUrl.searchParams.get("q")||"").trim();
      if(!q) return sendJson(res,400,{error:{code:"INVALID_QUERY",message:"Thiếu từ khóa tìm kiếm bài hát (?q=...)" }});
      if(Buffer.byteLength(q,"utf8")>MAX_YOUTUBE_QUERY_BYTES)
        return sendJson(res,400,{error:{code:"QUERY_TOO_LONG",message:"Từ khóa tìm kiếm quá dài."}});
      if(activeStreams>=(config.maxStreams||2))
        return sendJson(res,503,{error:{code:"CAPACITY_EXHAUSTED",message:"Proxy đang phục vụ tối đa luồng."}});
      activeStreams+=1;
      const requestId=++nextRequestId;
      let released=false;
      const release=()=>{if(!released){released=true;activeStreams-=1;}};
      res.once("finish",release);res.once("close",release);
      try{return streamHandler(q,req,res,{...config,requestId});}
      catch(error){release();console.error(`[YOUTUBE ${requestId}] Handler error`,error);return sendJson(res,500,{error:{code:"INTERNAL_ERROR",message:"Lỗi proxy nội bộ."}});}
    }
    return sendJson(res,404,{error:{code:"NOT_FOUND",message:"Endpoint không tồn tại."}});
  });
}
const isMain=process.argv[1] && fileURLToPath(import.meta.url)===fs.realpathSync(process.argv[1]);
if(isMain){
  loadEnvFile(new URL(".env",import.meta.url));
  const baseConfig=configFromEnv();
  const readiness=probeYtDlp(baseConfig);
  const config=Object.freeze({...baseConfig,ytDlpReady:readiness.ready,ytDlpVersion:readiness.version});
  if(!readiness.ready)console.error("yt-dlp readiness check failed; /healthz will report 503 until the process is restarted with yt-dlp installed.");
  createServer(config).listen(config.port,config.host,()=>console.log(`YouTube audio proxy listening on http://${config.host}:${config.port}`));
}
