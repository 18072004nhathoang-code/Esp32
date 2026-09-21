import http from "node:http";
import crypto from "node:crypto";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { queryDeepSeek } from "./lib/deepseek.js";
import { ServiceError, publicError } from "./lib/errors.js";
import { transcribeGemini, ttsGemini } from "./lib/gemini.js";
import { boundedText, parseMusicSources, parsePcm16Mono16kWav } from "./lib/protocol.js";
import { streamYouTubeAudio } from "./lib/youtube.js";

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
  if (!Number.isSafeInteger(parsed) || parsed < minimum || parsed > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return parsed;
}

function booleanSetting(value, fallback, name) {
  const normalized = String(value ?? fallback).toLowerCase();
  if (normalized !== "true" && normalized !== "false") throw new Error(`${name} must be true or false`);
  return normalized === "true";
}

function deepseekBaseUrl(value) {
  let parsed;
  try { parsed = new URL(value || "https://api.deepseek.com"); } catch { throw new Error("DEEPSEEK_BASE_URL is invalid"); }
  if (parsed.protocol !== "https:" || parsed.username || parsed.password || parsed.search || parsed.hash ||
      (parsed.pathname !== "/" && parsed.pathname !== "")) {
    throw new Error("DEEPSEEK_BASE_URL must be an HTTPS origin without credentials or a path");
  }
  return parsed.origin;
}

export function configFromEnv(env = process.env) {
  const llmProvider = (env.AI_LLM_PROVIDER || "deepseek").toLowerCase();
  const sttProvider = (env.AI_STT_PROVIDER || "gemini").toLowerCase();
  const ttsProvider = (env.AI_TTS_PROVIDER || "gemini").toLowerCase();
  if (llmProvider !== "deepseek") throw new Error(`AI_LLM_PROVIDER '${llmProvider}' is not supported`);
  if (sttProvider !== "gemini") throw new Error(`AI_STT_PROVIDER '${sttProvider}' is not supported`);
  if (ttsProvider !== "gemini") throw new Error(`AI_TTS_PROVIDER '${ttsProvider}' is not supported`);
  const deepseekModel = env.DEEPSEEK_MODEL || "deepseek-flash";
  if (deepseekModel !== "deepseek-flash") {
    throw new Error("DEEPSEEK_MODEL must use the API model ID deepseek-flash");
  }
  if (!env.DEEPSEEK_API_KEY) {
    console.warn("[WARN] DEEPSEEK_API_KEY chưa cấu hình. Endpoint /v1/query sẽ không khả dụng, nhưng /youtube/stream vẫn hoạt động bình thường.");
  }
  if ((sttProvider === "gemini" || ttsProvider === "gemini") && !env.GEMINI_API_KEY) {
    throw new Error("GEMINI_API_KEY is required for the selected Gemini STT/TTS provider");
  }
  if (!env.ESP_DEVICE_TOKEN || env.ESP_DEVICE_TOKEN.length < 32) {
    throw new Error("ESP_DEVICE_TOKEN of at least 32 characters is required");
  }
  return Object.freeze({
    llmProvider,
    sttProvider,
    ttsProvider,
    deepseekApiKey: env.DEEPSEEK_API_KEY,
    deepseekBaseUrl: deepseekBaseUrl(env.DEEPSEEK_BASE_URL),
    deepseekModel,
    deepseekThinking: booleanSetting(env.DEEPSEEK_THINKING, false, "DEEPSEEK_THINKING"),
    deepseekMaxOutputTokens: integerSetting(env.DEEPSEEK_MAX_OUTPUT_TOKENS, 256, 64, 1024, "DEEPSEEK_MAX_OUTPUT_TOKENS"),
    deepseekRetries: integerSetting(env.DEEPSEEK_MAX_RETRIES, 2, 0, 3, "DEEPSEEK_MAX_RETRIES"),
    deepseekRetryBaseMs: integerSetting(env.DEEPSEEK_RETRY_BASE_MS, 250, 1, 5000, "DEEPSEEK_RETRY_BASE_MS"),
    geminiApiKey: env.GEMINI_API_KEY,
    geminiSttModel: env.GEMINI_STT_MODEL || "gemini-2.5-flash",
    geminiTtsModel: env.GEMINI_TTS_MODEL || "gemini-3.1-flash-tts-preview",
    geminiTtsVoice: env.GEMINI_TTS_VOICE || "Kore",
    deviceToken: env.ESP_DEVICE_TOKEN,
    timeoutMs: integerSetting(env.REQUEST_TIMEOUT_MS, 45000, 1000, 60000, "REQUEST_TIMEOUT_MS"),
    maxQueryBytes: integerSetting(env.MAX_QUERY_BYTES, 400000, 32044, 1024 * 1024, "MAX_QUERY_BYTES"),
    maxTextBytes: integerSetting(env.MAX_TEXT_BYTES, 511, 32, 511, "MAX_TEXT_BYTES"),
    maxConcurrentRequests: integerSetting(env.MAX_CONCURRENT_REQUESTS, 4, 1, 32, "MAX_CONCURRENT_REQUESTS"),
    ttsPrewarm: booleanSetting(env.TTS_PREWARM_ENABLED, false, "TTS_PREWARM_ENABLED"),
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
    const chunks = [];
    let size = 0;
    let settled = false;
    const finish = (error, body) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      error ? reject(error) : resolve(body);
    };
    const timer = setTimeout(() => {
      req.destroy();
      finish(new ServiceError("REQUEST_TIMEOUT", "Hết thời gian nhận request.", 408));
    }, timeoutMs);
    req.on("data", (chunk) => {
      if (settled) return;
      size += chunk.length;
      if (size > limit) {
        finish(new ServiceError("REQUEST_TOO_LARGE", "Request body vượt giới hạn.", 413));
        req.resume();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => finish(null, Buffer.concat(chunks, size)));
    req.on("aborted", () => finish(new ServiceError("REQUEST_CANCELLED", "Yêu cầu đã bị hủy.", 499)));
    req.on("error", () => finish(new ServiceError("REQUEST_IO", "Lỗi khi nhận request.", 400)));
  });
}

function sendJson(res, status, value) {
  if (res.destroyed || res.writableEnded) return;
  const body = Buffer.from(JSON.stringify(value));
  res.writeHead(status, { "content-type": "application/json; charset=utf-8", "content-length": body.length, "cache-control": "no-store" });
  res.end(body);
}

function requestContext(req, res, timeoutMs) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(
    new ServiceError("AI_TIMEOUT", "Pipeline STT→LLM đã hết thời gian.", 504)), timeoutMs);
  const cancel = () => {
    if (!res.writableEnded && !controller.signal.aborted) {
      controller.abort(new ServiceError("REQUEST_CANCELLED", "Yêu cầu đã bị hủy.", 499));
    }
  };
  req.once("aborted", cancel);
  res.once("close", cancel);
  return {
    signal: controller.signal,
    cleanup() {
      clearTimeout(timer);
      req.removeListener("aborted", cancel);
      res.removeListener("close", cancel);
    },
  };
}

function waitWithSignal(promise, signal) {
  if (signal?.aborted) {
    return Promise.reject(signal.reason || new Error("aborted"));
  }
  return new Promise((resolve, reject) => {
    let cleanup = null;
    if (signal) {
      const onAbort = () => reject(signal.reason || new Error("aborted"));
      signal.addEventListener("abort", onAbort, { once: true });
      cleanup = () => signal.removeEventListener("abort", onAbort);
    }
    promise.then(
      (res) => {
        if (cleanup) cleanup();
        resolve(res);
      },
      (err) => {
        if (cleanup) cleanup();
        reject(err);
      }
    );
  });
}

export function createServer(config, dependencies = {}) {
  const stt = dependencies.stt || transcribeGemini;
  const llm = dependencies.llm || queryDeepSeek;
  const tts = dependencies.tts || ttsGemini;
  const MAX_CACHE_ENTRIES = 20;
  const MAX_CACHE_BYTES = 4 * 1024 * 1024;
  const MAX_PREWARM_CONCURRENT = 2;
  const ttsCache = new Map();
  let totalCacheBytes = 0;
  let activePrewarmCount = 0;
  let activeRequests = 0;

  function evict(key) {
    const entry = ttsCache.get(key);
    if (!entry) return;
    ttsCache.delete(key);
    if (entry.bytes) {
      totalCacheBytes = Math.max(0, totalCacheBytes - entry.bytes);
    }
    if (entry.controller && !entry.controller.signal.aborted) {
      try {
        entry.controller.abort(new Error("evicted from cache"));
      } catch {}
    }
  }

  function trimCache() {
    while (ttsCache.size > MAX_CACHE_ENTRIES || totalCacheBytes > MAX_CACHE_BYTES) {
      const oldest = ttsCache.keys().next().value;
      if (oldest === undefined) break;
      evict(oldest);
    }
  }

  function prewarmTts(text) {
    if (!config.ttsPrewarm || !text || ttsCache.has(text)) return;
    if (activePrewarmCount >= MAX_PREWARM_CONCURRENT) return;

    const controller = new AbortController();
    const timer = setTimeout(() => {
      try {
        controller.abort(new ServiceError("AI_TIMEOUT", "Prewarm TTS timeout.", 504));
      } catch {}
    }, config.timeoutMs);

    activePrewarmCount++;
    const entry = {
      controller,
      bytes: 0,
      promise: null,
    };

    entry.promise = Promise.resolve()
      .then(() => tts(text, config, { signal: controller.signal }))
      .then((wav) => {
        parsePcm16Mono16kWav(wav);
        entry.bytes = wav.length;
        if (ttsCache.get(text) === entry) {
          totalCacheBytes += wav.length;
          trimCache();
        }
        return wav;
      })
      .catch(() => {
        if (ttsCache.get(text) === entry) {
          evict(text);
        }
        return null;
      })
      .finally(() => {
        clearTimeout(timer);
        activePrewarmCount = Math.max(0, activePrewarmCount - 1);
      });

    ttsCache.set(text, entry);
    trimCache();
  }

  return http.createServer(async (req, res) => {
    if (req.method === "GET" && req.url === "/healthz") {
      return sendJson(res, 200, {
        ok: true,
        providers: { stt: config.sttProvider, llm: config.llmProvider, tts: config.ttsProvider },
        cache: { entries: ttsCache.size, bytes: totalCacheBytes },
      });
    }

    const parsedUrl = new URL(req.url, "http://localhost");
    if ((req.method === "GET" || req.method === "HEAD") && parsedUrl.pathname === "/youtube/stream") {
      const q = parsedUrl.searchParams.get("q") || "";
      if (!q.trim()) {
        return sendJson(res, 400, { error: { code: "INVALID_QUERY", message: "Thiếu từ khóa tìm kiếm bài hát (?q=...)" } });
      }
      return streamYouTubeAudio(q.trim(), req, res);
    }

    if (req.method !== "POST" || (req.url !== "/v1/query" && req.url !== "/v1/tts")) {
      return sendJson(res, 404, { error: { code: "NOT_FOUND", message: "Endpoint không tồn tại." } });
    }
    if (!authorized(req, config.deviceToken)) {
      return sendJson(res, 401, { error: { code: "DEVICE_AUTH", message: "Device token không hợp lệ." } });
    }
    if (activeRequests >= config.maxConcurrentRequests) {
      return sendJson(res, 503, { error: { code: "AI_BUSY", message: "Gateway đang xử lý quá nhiều yêu cầu." } });
    }
    activeRequests++;
    const context = requestContext(req, res, config.timeoutMs);
    try {
      if (req.url === "/v1/query") {
        if (!String(req.headers["content-type"] || "").toLowerCase().startsWith("audio/wav")) {
          throw new ServiceError("INVALID_CONTENT_TYPE", "Content-Type phải là audio/wav.", 415);
        }
        const wav = await readBody(req, config.maxQueryBytes, config.timeoutMs);
        try { parsePcm16Mono16kWav(wav); } catch {
          throw new ServiceError("INVALID_WAV", "WAV phải là PCM16 mono 16 kHz hoàn chỉnh.", 400);
        }
        const transcript = boundedText(await stt(wav, config, context), config.maxTextBytes);
        const answer = await llm(transcript, config, context);
        const result = {
          transcript,
          reply: boundedText(answer.reply, config.maxTextBytes),
          actions: answer.actions || [],
          sources: answer.sources || [],
        };
        prewarmTts(result.reply);
        return sendJson(res, 200, result);
      }

      if (!String(req.headers["content-type"] || "").toLowerCase().startsWith("application/json")) {
        throw new ServiceError("INVALID_CONTENT_TYPE", "Content-Type phải là application/json.", 415);
      }
      const raw = await readBody(req, 4096, config.timeoutMs);
      let input;
      try { input = JSON.parse(raw.toString("utf8")); } catch {
        throw new ServiceError("INVALID_JSON", "JSON request không hợp lệ.", 400);
      }
      if (!input || Object.keys(input).length !== 1 || typeof input.text !== "string") {
        throw new ServiceError("INVALID_TTS_REQUEST", "TTS yêu cầu đúng một trường text.", 400);
      }
      const text = boundedText(input.text, config.maxTextBytes);
      let wav;
      if (ttsCache.has(text)) {
        const entry = ttsCache.get(text);
        ttsCache.delete(text);
        if (entry.bytes) {
          totalCacheBytes = Math.max(0, totalCacheBytes - entry.bytes);
        }
        try {
          wav = await waitWithSignal(entry.promise, context.signal);
        } catch (err) {
          if (context.signal.aborted) {
            if (entry.controller && !entry.controller.signal.aborted) {
              try { entry.controller.abort(context.signal.reason); } catch {}
            }
            throw err;
          }
          wav = null;
        }
      }
      if (!wav) wav = await tts(text, config, context);
      try { parsePcm16Mono16kWav(wav); } catch {
        throw new ServiceError("INVALID_TTS_AUDIO", "Provider TTS trả về WAV không hợp lệ.");
      }
      if (res.destroyed || res.writableEnded || context.signal.aborted) return;
      res.writeHead(200, { "content-type": "audio/wav", "content-length": wav.length, "cache-control": "no-store" });
      res.end(wav);
    } catch (error) {
      if (!res.destroyed && !res.writableEnded) {
        const output = publicError(error);
        sendJson(res, output.status, output.value);
      }
    } finally {
      context.cleanup();
      activeRequests--;
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
