import assert from "node:assert/strict";
import { afterEach, test } from "node:test";
import { createServer, configFromEnv } from "../server.js";
import { resolveYtDlpPath } from "../lib/youtube.js";
const servers=[];
afterEach(async()=>{while(servers.length)await new Promise(resolve=>servers.pop().close(resolve));});
async function start(config={}){const server=createServer(config);await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));servers.push(server);return `http://127.0.0.1:${server.address().port}`;}
test("configFromEnv returns frozen config with defaults",()=>{const c=configFromEnv({PORT:"9000",HOST:"0.0.0.0",YT_DLP_PATH:"/opt/bin/yt-dlp"});assert.equal(c.port,9000);assert.equal(c.host,"0.0.0.0");assert.equal(c.ytDlpPath,"/opt/bin/yt-dlp");assert.equal(Object.isFrozen(c),true);});
test("configFromEnv rejects invalid PORT",()=>{assert.throws(()=>configFromEnv({PORT:"99999"}),/PORT/);assert.throws(()=>configFromEnv({PORT:"abc"}),/PORT/);});
test("resolveYtDlpPath supports explicit config and portable PATH fallback",()=>{assert.equal(resolveYtDlpPath({ytDlpPath:"/custom/yt-dlp"}),"/custom/yt-dlp");const old=process.env.YT_DLP_PATH;delete process.env.YT_DLP_PATH;try{assert.equal(resolveYtDlpPath({}),"yt-dlp");}finally{if(old===undefined)delete process.env.YT_DLP_PATH;else process.env.YT_DLP_PATH=old;}});
test("GET /healthz returns ok",async()=>{const base=await start();const res=await fetch(`${base}/healthz`);assert.equal(res.status,200);assert.equal((await res.json()).ok,true);});
test("unknown route returns 404",async()=>{const base=await start();const res=await fetch(`${base}/v1/query`,{method:"POST"});assert.equal(res.status,404);assert.equal((await res.json()).error.code,"NOT_FOUND");});
test("GET /youtube/stream without query returns 400",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream`);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"INVALID_QUERY");});
test("GET /youtube/stream with blank query returns 400",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream?q=   `);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"INVALID_QUERY");});
test("GET /youtube/stream rejects oversized query before spawning yt-dlp",async()=>{const base=await start();const res=await fetch(`${base}/youtube/stream?q=${"x".repeat(129)}`);assert.equal(res.status,400);assert.equal((await res.json()).error.code,"QUERY_TOO_LONG");});
