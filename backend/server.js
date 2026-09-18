import http from "node:http";
import crypto from "node:crypto";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { parseMusicSources, parsePcm16Mono16kWav } from "./lib/protocol.js";
import { queryGemini, ttsGemini } from "./lib/gemini.js";

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

export function configFromEnv(env = process.env) {
  const timeoutMs = Number(env.REQUEST_TIMEOUT_MS || 45000);
  const maxQueryBytes = Number(env.MAX_QUERY_BYTES || 400000);
  const maxTextBytes = Number(env.MAX_TEXT_BYTES || 511);
  if (!env.GEMINI_API_KEY || !env.ESP_DEVICE_TOKEN || env.ESP_DEVICE_TOKEN.length < 32) {
    throw new Error("GEMINI_API_KEY and an ESP_DEVICE_TOKEN of at least 32 characters are required");
  }
  if (![timeoutMs, maxQueryBytes, maxTextBytes].every(Number.isSafeInteger)) throw new Error("numeric environment setting is invalid");
  return Object.freeze({
    apiKey: env.GEMINI_API_KEY,
    deviceToken: env.ESP_DEVICE_TOKEN,
    queryModel: env.GEMINI_QUERY_MODEL || "gemini-2.5-flash",
    ttsModel: env.GEMINI_TTS_MODEL || "gemini-3.1-flash-tts-preview",
    ttsVoice: env.GEMINI_TTS_VOICE || "Kore",
    timeoutMs: Math.min(Math.max(timeoutMs, 1000), 60000),
    maxQueryBytes: Math.min(Math.max(maxQueryBytes, 32044), 1024 * 1024),
    maxTextBytes: Math.min(Math.max(maxTextBytes, 32), 511),
    enableSearch: env.ENABLE_SEARCH === "true",
    musicSources: parseMusicSources(env.MUSIC_SOURCES_JSON),
  });
}

function authorized(req, expected) {
  const supplied = req.headers.authorization?.match(/^Bearer ([^\s]+)$/)?.[1] || "";
  const a = Buffer.from(supplied); const b = Buffer.from(expected);
  return a.length === b.length && a.length > 0 && crypto.timingSafeEqual(a, b);
}

function readBody(req, limit, timeoutMs) {
  return new Promise((resolve, reject) => {
    const chunks = []; let size = 0; let settled = false;
    const finish = (error, body) => { if (settled) return; settled = true; clearTimeout(timer); error ? reject(error) : resolve(body); };
    const timer = setTimeout(() => { req.destroy(); finish(Object.assign(new Error("request timeout"), { status: 408 })); }, timeoutMs);
    req.on("data", (chunk) => {
      size += chunk.length;
      if (size > limit) { req.destroy(); finish(Object.assign(new Error("request body too large"), { status: 413 })); return; }
      chunks.push(chunk);
    });
    req.on("end", () => finish(null, Buffer.concat(chunks)));
    req.on("error", (error) => finish(error));
  });
}

function sendJson(res, status, value) {
  const body = Buffer.from(JSON.stringify(value));
  res.writeHead(status, { "content-type": "application/json; charset=utf-8", "content-length": body.length, "cache-control": "no-store" });
  res.end(body);
}

export function createServer(config, dependencies = {}) {
  const query = dependencies.query || queryGemini;
  const tts = dependencies.tts || ttsGemini;
  return http.createServer(async (req, res) => {
    try {
      if (req.method === "GET" && req.url === "/healthz") return sendJson(res, 200, { ok: true });
      if (req.method !== "POST" || (req.url !== "/v1/query" && req.url !== "/v1/tts")) return sendJson(res, 404, { error: "not found" });
      if (!authorized(req, config.deviceToken)) return sendJson(res, 401, { error: "invalid device token" });
      if (req.url === "/v1/query") {
        if (!String(req.headers["content-type"] || "").toLowerCase().startsWith("audio/wav")) return sendJson(res, 415, { error: "content-type must be audio/wav" });
        const wav = await readBody(req, config.maxQueryBytes, config.timeoutMs);
        parsePcm16Mono16kWav(wav);
        return sendJson(res, 200, await query(wav, config));
      }
      if (!String(req.headers["content-type"] || "").toLowerCase().startsWith("application/json")) return sendJson(res, 415, { error: "content-type must be application/json" });
      const raw = await readBody(req, 4096, config.timeoutMs);
      let input;
      try { input = JSON.parse(raw.toString("utf8")); } catch { return sendJson(res, 400, { error: "invalid JSON" }); }
      if (!input || Object.keys(input).length !== 1 || typeof input.text !== "string") return sendJson(res, 400, { error: "expected {text}" });
      const wav = await tts(input.text, config);
      res.writeHead(200, { "content-type": "audio/wav", "content-length": wav.length, "cache-control": "no-store" });
      res.end(wav);
    } catch (error) {
      const status = error.status || (/WAV|text field|audio/i.test(error.message) ? 400 : 502);
      sendJson(res, status, { error: error.message === "request body too large" ? error.message : "AI request failed" });
    }
  });
}

const isMain = process.argv[1] && fileURLToPath(import.meta.url) === fs.realpathSync(process.argv[1]);
if (isMain) {
  loadEnvFile(new URL(".env", import.meta.url));
  const config = configFromEnv();
  const host = process.env.HOST || "127.0.0.1";
  const port = Number(process.env.PORT || 8787);
  createServer(config).listen(port, host, () => console.log(`AI backend listening on http://${host}:${port}`));
}
