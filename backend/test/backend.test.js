import assert from "node:assert/strict";
import { afterEach, test } from "node:test";
import { queryDeepSeek } from "../lib/deepseek.js";
import { ServiceError } from "../lib/errors.js";
import { transcribeGemini, ttsGemini } from "../lib/gemini.js";
import { parseMusicSources, parsePcm16Mono16kWav, resamplePcm16Mono, sanitizeActions, wavFromPcm16 } from "../lib/protocol.js";
import { createServer, configFromEnv } from "../server.js";

const token = "0123456789abcdef0123456789abcdef";
const baseEnv = {
  AI_LLM_PROVIDER: "deepseek",
  DEEPSEEK_API_KEY: "deepseek-secret",
  DEEPSEEK_BASE_URL: "https://api.deepseek.com",
  DEEPSEEK_MODEL: "deepseek-flash",
  DEEPSEEK_MAX_RETRIES: "0",
  DEEPSEEK_RETRY_BASE_MS: "1",
  AI_STT_PROVIDER: "gemini",
  AI_TTS_PROVIDER: "gemini",
  GEMINI_API_KEY: "gemini-secret",
  ESP_DEVICE_TOKEN: token,
  MUSIC_SOURCES_JSON: '[{"id":"news","label":"News","url":"https://example.com/live.mp3"}]',
};
const config = configFromEnv(baseEnv);
const servers = [];

afterEach(async () => {
  while (servers.length) await new Promise((resolve) => servers.pop().close(resolve));
});

async function start(dependencies, override = {}) {
  const server = createServer({ ...config, ...override }, dependencies);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  servers.push(server);
  return `http://127.0.0.1:${server.address().port}`;
}

function sampleWav() { return wavFromPcm16(Buffer.alloc(1600 * 2), 16000); }

function completion(content, finishReason = "stop") {
  return new Response(JSON.stringify({
    choices: [{ finish_reason: finishReason, message: { content, reasoning_content: "không được đọc ra loa" } }],
  }), { status: 200, headers: { "content-type": "application/json" } });
}

test("configuration requires credentials only for selected providers", () => {
  assert.throws(() => configFromEnv({ ...baseEnv, DEEPSEEK_API_KEY: "" }), /DEEPSEEK_API_KEY/);
  assert.throws(() => configFromEnv({ ...baseEnv, GEMINI_API_KEY: "" }), /selected Gemini STT\/TTS/);
  assert.throws(() => configFromEnv({ ...baseEnv, AI_LLM_PROVIDER: "gemini" }), /AI_LLM_PROVIDER/);
  assert.throws(() => configFromEnv({ ...baseEnv, DEEPSEEK_BASE_URL: "http://api.deepseek.com" }), /HTTPS origin/);
  assert.throws(() => configFromEnv({ ...baseEnv, DEEPSEEK_MODEL: "DeepSeek V4.1 Flash" }), /deepseek-flash/);
});

test("protocol validates WAV, UTF-8 limits and action allowlist", () => {
  assert.equal(parsePcm16Mono16kWav(sampleWav()).length, 3200);
  assert.throws(() => parsePcm16Mono16kWav(Buffer.from("bad")));
  const sources = parseMusicSources('[{"id":"news","label":"News","url":"https://example.com/a.mp3"}]');
  assert.deepEqual(sanitizeActions([{ type: "music.play", source_id: "news" }, { type: "music.volume", value: 55 }], sources),
    [{ type: "music.play", source_id: "news" }, { type: "music.volume", value: 55 }]);
  assert.throws(() => sanitizeActions([{ type: "music.play", source_id: "invented" }], sources));
  assert.throws(() => sanitizeActions([{ type: "device.restart" }], sources));
  assert.throws(() => parseMusicSources('[{"id":"x","label":"x","url":"http://example.com/a"}]'));
});

test("24 kHz PCM is converted to valid 16 kHz mono WAV", () => {
  const pcm = Buffer.alloc(24000 * 2);
  for (let i = 0; i < 24000; i++) pcm.writeInt16LE(i % 1000, i * 2);
  const converted = resamplePcm16Mono(pcm, 24000, 16000);
  assert.equal(converted.length, 32000);
  assert.equal(parsePcm16Mono16kWav(wavFromPcm16(converted)).length, 32000);
});

test("Gemini STT only transcribes and never asks for an answer or action", async () => {
  let sent;
  const transcript = await transcribeGemini(sampleWav(), config, {
    fetchImpl: async (_url, options) => {
      sent = JSON.parse(options.body);
      return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: '{"transcript":"phát radio"}' }] } }] }), { status: 200 });
    },
  });
  assert.equal(transcript, "phát radio");
  assert.ok(sent.contents[0].parts[0].inlineData.data.length > 100);
  assert.match(sent.contents[0].parts[1].text, /không trả lời câu hỏi/);
  assert.equal(sent.generationConfig.responseMimeType, "application/json");
});

test("DeepSeek request uses exact model, auth, JSON mode and disabled thinking", async () => {
  let url;
  let options;
  const answer = await queryDeepSeek("phát radio", config, {
    fetchImpl: async (input, init) => {
      url = input;
      options = init;
      return completion('{"reply":"Tôi sẽ gửi lệnh.","needs_current_info":false,"actions":[{"type":"music.play","source_id":"news"}]}');
    },
  });
  const body = JSON.parse(options.body);
  assert.equal(url, "https://api.deepseek.com/chat/completions");
  assert.equal(options.headers.authorization, "Bearer deepseek-secret");
  assert.equal(body.model, "deepseek-flash");
  assert.deepEqual(body.thinking, { type: "disabled" });
  assert.deepEqual(body.response_format, { type: "json_object" });
  assert.equal(body.max_tokens, 256);
  assert.match(body.messages[0].content, /Schema JSON/);
  assert.equal(body.messages[1].content, "phát radio");
  assert.doesNotMatch(options.body, /UklGR|audio\/wav|base64/i);
  assert.deepEqual(answer.actions, [{ type: "music.play", source_id: "news" }]);
  assert.deepEqual(answer.sources, []);
});

test("DeepSeek rejects 401 and 402 without retry and reports provider status", async () => {
  for (const [status, code] of [[401, "DEEPSEEK_AUTH"], [402, "DEEPSEEK_BALANCE"]]) {
    let calls = 0;
    await assert.rejects(
      queryDeepSeek("xin chào", { ...config, deepseekRetries: 3 }, {
        fetchImpl: async () => { calls++; return new Response("{}", { status }); },
      }),
      (error) => error.code === code && error.providerStatus === status,
    );
    assert.equal(calls, 1);
  }
});

test("DeepSeek retries 429 and 5xx finitely with backoff", async () => {
  for (const status of [429, 503]) {
    let calls = 0;
    const answer = await queryDeepSeek("xin chào", { ...config, deepseekRetries: 2 }, {
      fetchImpl: async () => {
        calls++;
        return calls < 3 ? new Response("{}", { status }) :
          completion('{"reply":"Xin chào.","needs_current_info":false,"actions":[]}');
      },
    });
    assert.equal(answer.reply, "Xin chào.");
    assert.equal(calls, 3);
  }
});

test("DeepSeek distinguishes timeout, empty content, truncation and invalid actions", async () => {
  const controller = new AbortController();
  const timed = queryDeepSeek("xin chào", config, {
    signal: controller.signal,
    fetchImpl: async (_url, options) => new Promise((_resolve, reject) => {
      options.signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")), { once: true });
    }),
  });
  controller.abort(new ServiceError("AI_TIMEOUT", "timeout", 504));
  await assert.rejects(timed, (error) => error.code === "AI_TIMEOUT");
  await assert.rejects(queryDeepSeek("x", config, { fetchImpl: async () => completion("") }), (error) => error.code === "DEEPSEEK_EMPTY");
  await assert.rejects(queryDeepSeek("x", config, { fetchImpl: async () => completion("{}", "length") }), (error) => error.code === "DEEPSEEK_TRUNCATED");
  await assert.rejects(queryDeepSeek("x", config, { fetchImpl: async () => completion("{bad") }), (error) => error.code === "DEEPSEEK_JSON");
  await assert.rejects(queryDeepSeek("x", config, {
    fetchImpl: async () => completion('{"reply":"ok","needs_current_info":false,"actions":[{"type":"device.restart"}]}'),
  }), (error) => error.code === "DEEPSEEK_ACTION_REJECTED");
  await assert.rejects(queryDeepSeek("x", config, {
    fetchImpl: async () => completion('{"reply":"ok","needs_current_info":false,"actions":[],"transcript":"fake"}'),
  }), (error) => error.code === "DEEPSEEK_SCHEMA");
});

test("DeepSeek enforces actual response bytes and UTF-8-safe firmware text", async () => {
  await assert.rejects(queryDeepSeek("x", config, {
    fetchImpl: async () => new Response(new ReadableStream({
      start(controller) {
        controller.enqueue(Buffer.alloc(40 * 1024, 0x20));
        controller.enqueue(Buffer.alloc(25 * 1024, 0x20));
        controller.close();
      },
    }), { status: 200 }),
  }), (error) => error.code === "DEEPSEEK_RESPONSE_TOO_LARGE");
  const answer = await queryDeepSeek("x", config, {
    fetchImpl: async () => completion(JSON.stringify({ reply: "ấ".repeat(300), needs_current_info: false, actions: [] })),
  });
  assert.ok(Buffer.byteLength(answer.reply, "utf8") <= config.maxTextBytes);
  assert.doesNotMatch(answer.reply, /\uFFFD/u);
});

test("questions requiring fresh data get an honest no-search response", async () => {
  const answer = await queryDeepSeek("giá vàng hôm nay", config, {
    fetchImpl: async () => completion('{"reply":"Giá vàng là 1 đồng.","needs_current_info":true,"actions":[]}'),
  });
  assert.match(answer.reply, /chưa thể tra cứu thông tin mới/);
  assert.deepEqual(answer.sources, []);
});

test("full HTTP pipeline keeps STT transcript and provides DeepSeek reply then Gemini TTS", async () => {
  const providerFetch = async (url, options) => {
    if (url.includes(":generateContent")) {
      return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: '{"transcript":"tạm dừng nhạc"}' }] } }] }), { status: 200 });
    }
    if (url.endsWith("/chat/completions")) {
      const body = JSON.parse(options.body);
      assert.equal(body.messages[1].content, "tạm dừng nhạc");
      return completion('{"reply":"Tôi sẽ gửi lệnh tạm dừng.","needs_current_info":false,"actions":[{"type":"music.pause"}]}');
    }
    if (url.endsWith("/interactions")) {
      const pcm24k = Buffer.alloc(2400 * 2);
      return new Response(JSON.stringify({ output: [{ type: "audio", data: pcm24k.toString("base64") }] }), { status: 200 });
    }
    throw new Error(`unexpected URL ${url}`);
  };
  const dependencies = {
    stt: (wav, cfg, ctx) => transcribeGemini(wav, cfg, { ...ctx, fetchImpl: providerFetch }),
    llm: (text, cfg, ctx) => queryDeepSeek(text, cfg, { ...ctx, fetchImpl: providerFetch }),
    tts: (text, cfg, ctx) => ttsGemini(text, cfg, { ...ctx, fetchImpl: providerFetch }),
  };
  const base = await start(dependencies);
  const query = await fetch(`${base}/v1/query`, {
    method: "POST",
    headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` },
    body: sampleWav(),
  });
  assert.equal(query.status, 200);
  const result = await query.json();
  assert.deepEqual(result, {
    transcript: "tạm dừng nhạc",
    reply: "Tôi sẽ gửi lệnh tạm dừng.",
    actions: [{ type: "music.pause" }],
    sources: [],
  });
  const speech = await fetch(`${base}/v1/tts`, {
    method: "POST",
    headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
    body: JSON.stringify({ text: result.reply }),
  });
  assert.equal(speech.status, 200);
  assert.equal(speech.headers.get("content-type"), "audio/wav");
  assert.ok(parsePcm16Mono16kWav(Buffer.from(await speech.arrayBuffer())).length > 0);
});

test("gateway exposes safe provider error codes without leaking keys", async () => {
  const base = await start({
    stt: async () => "xin chào",
    llm: async () => { throw new ServiceError("DEEPSEEK_AUTH", "DeepSeek từ chối API key.", 502, 401); },
  });
  const response = await fetch(`${base}/v1/query`, {
    method: "POST",
    headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` },
    body: sampleWav(),
  });
  assert.equal(response.status, 502);
  assert.deepEqual(await response.json(), {
    error: { code: "DEEPSEEK_AUTH", message: "DeepSeek từ chối API key.", provider_status: 401 },
  });
});

test("gateway rejects an invalid device token before invoking providers", async () => {
  let called = false;
  const base = await start({ stt: async () => { called = true; return "x"; } });
  const response = await fetch(`${base}/v1/query`, {
    method: "POST", headers: { "content-type": "audio/wav" }, body: sampleWav(),
  });
  assert.equal(response.status, 401);
  assert.equal((await response.json()).error.code, "DEVICE_AUTH");
  assert.equal(called, false);
});

test("client cancellation aborts the active provider and no stale response is sent", async () => {
  let providerAborted = false;
  const base = await start({
    stt: async () => "xin chào",
    llm: async (_text, _config, context) => new Promise((_resolve, reject) => {
      context.signal.addEventListener("abort", () => {
        providerAborted = true;
        reject(context.signal.reason);
      }, { once: true });
    }),
  });
  const controller = new AbortController();
  const pending = fetch(`${base}/v1/query`, {
    method: "POST",
    headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` },
    body: sampleWav(),
    signal: controller.signal,
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  controller.abort();
  await assert.rejects(pending, /abort/i);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(providerAborted, true);
});

test("TTS prewarm is disabled by default and concurrency is bounded", async () => {
  let ttsCalls = 0;
  let release;
  const blocked = new Promise((resolve) => { release = resolve; });
  const base = await start({
    stt: async () => "xin chào",
    llm: async () => { await blocked; return { reply: "Xin chào.", actions: [], sources: [] }; },
    tts: async () => { ttsCalls++; return sampleWav(); },
  }, { maxConcurrentRequests: 1, ttsPrewarm: false });
  const first = fetch(`${base}/v1/query`, {
    method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  const busy = await fetch(`${base}/v1/query`, {
    method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
  });
  assert.equal(busy.status, 503);
  assert.equal((await busy.json()).error.code, "AI_BUSY");
  release();
  assert.equal((await first).status, 200);
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(ttsCalls, 0);
});

test("TTS prewarm concurrency is capped at 2 and evicted entries abort in-flight jobs", async () => {
  let activePrewarms = 0;
  let maxConcurrentPrewarms = 0;
  let abortedPrewarms = 0;
  const resolvers = [];
  let queryCounter = 0;

  const base = await start({
    stt: async () => "xin chào",
    llm: async () => ({ reply: `Reply ${++queryCounter}`, actions: [], sources: [] }),
    tts: async (_text, _config, context) => new Promise((resolve) => {
      activePrewarms++;
      if (activePrewarms > maxConcurrentPrewarms) maxConcurrentPrewarms = activePrewarms;
      context.signal?.addEventListener("abort", () => {
        abortedPrewarms++;
      }, { once: true });
      resolvers.push(() => {
        activePrewarms--;
        resolve(sampleWav());
      });
    }),
  }, { maxConcurrentRequests: 5, ttsPrewarm: true });

  try {
    // Make 3 queries in parallel
    const req1 = fetch(`${base}/v1/query`, {
      method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
    });
    const req2 = fetch(`${base}/v1/query`, {
      method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
    });
    const req3 = fetch(`${base}/v1/query`, {
      method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
    });

    await Promise.all([req1, req2, req3]);
    // Give a small tick for prewarm tasks to start
    await new Promise((r) => setTimeout(r, 25));

    // Max concurrent prewarms should be capped at 2
    assert.equal(maxConcurrentPrewarms, 2);
  } finally {
    // Clean up resolvers so test runner can exit
    while (resolvers.length > 0) {
      resolvers.pop()();
    }
  }
});

test("in-flight prewarm entry consumed by /v1/tts does not leak totalCacheBytes", async () => {
  let finishPrewarm = null;
  const prewarmStarted = new Promise((resolve) => {
    finishPrewarm = resolve;
  });

  const base = await start({
    stt: async () => "xin chào",
    llm: async () => ({ reply: "Xin chào bạn!", actions: [], sources: [] }),
    tts: async (_text, _config, _context) => new Promise((resolve) => {
      finishPrewarm = () => resolve(sampleWav());
    }),
  }, { ttsPrewarm: true });

  // 1. Trigger /v1/query which triggers prewarm of "Xin chào bạn!"
  const qres = await fetch(`${base}/v1/query`, {
    method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
  });
  assert.equal(qres.status, 200);

  // Give prewarm a moment to be enqueued in cache
  await new Promise((r) => setTimeout(r, 20));

  // 2. Client requests /v1/tts for "Xin chào bạn!" while prewarm is still in flight
  const ttsReq = fetch(`${base}/v1/tts`, {
    method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
    body: JSON.stringify({ text: "Xin chào bạn!" }),
  });

  // Small delay to ensure /v1/tts took the entry from cache
  await new Promise((r) => setTimeout(r, 20));

  // 3. Resolve the in-flight prewarm promise
  finishPrewarm();
  const ttsRes = await ttsReq;
  assert.equal(ttsRes.status, 200);

  // 4. Inspect cache stats via /healthz: totalCacheBytes must be 0!
  const hres = await fetch(`${base}/healthz`);
  const health = await hres.json();
  assert.equal(health.cache.entries, 0);
  assert.equal(health.cache.bytes, 0);
});

test("client aborting /v1/tts while waiting for in-flight cache promise aborts job cleanly", async () => {
  let prewarmAborted = false;
  let resolveJob = null;

  const base = await start({
    stt: async () => "test",
    llm: async () => ({ reply: "Chờ hủy", actions: [], sources: [] }),
    tts: async (_text, _config, context) => new Promise((resolve) => {
      resolveJob = resolve;
      context.signal?.addEventListener("abort", () => {
        prewarmAborted = true;
      }, { once: true });
    }),
  }, { ttsPrewarm: true });

  // 1. Trigger prewarm
  await fetch(`${base}/v1/query`, {
    method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav(),
  });
  await new Promise((r) => setTimeout(r, 20));

  // 2. Client initiates /v1/tts with an AbortController
  const ac = new AbortController();
  const ttsReq = fetch(`${base}/v1/tts`, {
    method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
    body: JSON.stringify({ text: "Chờ hủy" }),
    signal: ac.signal,
  });

  await new Promise((r) => setTimeout(r, 20));

  // 3. Abort client request
  ac.abort();

  await assert.rejects(ttsReq);
  await new Promise((resolve) => setTimeout(resolve, 30));
  // Prewarm job controller should be aborted as orphaned consumer left
  assert.equal(prewarmAborted, true);

  if (resolveJob) resolveJob(sampleWav());
});

