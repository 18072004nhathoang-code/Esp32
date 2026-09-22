import http from "node:http";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { streamYouTubeAudio } from "./lib/youtube.js";

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
  return Object.freeze({
    port: integerSetting(env.PORT, 8787, 1, 65535, "PORT"),
    host: env.HOST || "127.0.0.1",
    ytDlpPath: env.YT_DLP_PATH || "",
  });
}
function sendJson(res, status, value) {
  if (res.destroyed || res.writableEnded) return;
  const body = Buffer.from(JSON.stringify(value));
  res.writeHead(status, {"content-type":"application/json; charset=utf-8","content-length":body.length,"cache-control":"no-store"});
  res.end(body);
}
export function createServer(config = {}) {
  return http.createServer((req,res)=>{
    if(req.method==="GET" && req.url==="/healthz") return sendJson(res,200,{ok:true});
    let parsedUrl;
    try { parsedUrl=new URL(req.url||"/","http://localhost"); }
    catch { return sendJson(res,400,{error:{code:"INVALID_URL",message:"URL yêu cầu không hợp lệ."}}); }
    if((req.method==="GET"||req.method==="HEAD") && parsedUrl.pathname==="/youtube/stream"){
      const q=(parsedUrl.searchParams.get("q")||"").trim();
      if(!q) return sendJson(res,400,{error:{code:"INVALID_QUERY",message:"Thiếu từ khóa tìm kiếm bài hát (?q=...)" }});
      if(Buffer.byteLength(q,"utf8")>MAX_YOUTUBE_QUERY_BYTES)
        return sendJson(res,400,{error:{code:"QUERY_TOO_LONG",message:"Từ khóa tìm kiếm quá dài."}});
      return streamYouTubeAudio(q,req,res,config);
    }
    return sendJson(res,404,{error:{code:"NOT_FOUND",message:"Endpoint không tồn tại."}});
  });
}
const isMain=process.argv[1] && fileURLToPath(import.meta.url)===fs.realpathSync(process.argv[1]);
if(isMain){
  loadEnvFile(new URL(".env",import.meta.url));
  const config=configFromEnv();
  createServer(config).listen(config.port,config.host,()=>console.log(`YouTube audio proxy listening on http://${config.host}:${config.port}`));
}
