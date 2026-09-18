import { abortError, ServiceError } from "./errors.js";
import { boundedText, resamplePcm16Mono, safeTruncateUtf8, wavFromPcm16 } from "./protocol.js";

function responseText(payload) {
  const parts = payload?.candidates?.[0]?.content?.parts;
  if (!Array.isArray(parts)) throw new ServiceError("GEMINI_SCHEMA", "Gemini không trả về candidate hợp lệ.");
  return parts.map((part) => typeof part.text === "string" ? part.text : "").join("").trim();
}

async function readLimited(response, limit, signal) {
  const declared = Number(response.headers.get("content-length"));
  if (Number.isFinite(declared) && declared > limit) {
    await response.body?.cancel();
    throw new ServiceError("GEMINI_RESPONSE_TOO_LARGE", "Phản hồi Gemini vượt giới hạn.");
  }
  if (!response.body) return "";
  const reader = response.body.getReader();
  const chunks = [];
  let size = 0;
  try {
    for (;;) {
      if (signal?.aborted) throw abortError(signal);
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > limit) throw new ServiceError("GEMINI_RESPONSE_TOO_LARGE", "Phản hồi Gemini vượt giới hạn.");
      chunks.push(Buffer.from(value));
    }
  } finally {
    if (size > limit || signal?.aborted) await reader.cancel().catch(() => {});
  }
  return Buffer.concat(chunks, size).toString("utf8");
}

function geminiHttpError(status, capability) {
  if (status === 401 || status === 403) return new ServiceError(`GEMINI_${capability}_AUTH`, `Gemini ${capability} từ chối API key.`, 502, status);
  if (status === 429) return new ServiceError(`GEMINI_${capability}_RATE_LIMIT`, `Gemini ${capability} đang hết quota hoặc giới hạn tần suất.`, 503, status);
  if (status >= 500) return new ServiceError(`GEMINI_${capability}_UNAVAILABLE`, `Gemini ${capability} đang tạm thời không khả dụng.`, 503, status);
  return new ServiceError(`GEMINI_${capability}_REQUEST`, `Gemini ${capability} từ chối yêu cầu (${status}).`, 502, status);
}

async function geminiFetch(url, body, config, options, responseLimit, capability) {
  const fetchImpl = options.fetchImpl || fetch;
  const signal = options.signal;
  if (signal?.aborted) throw abortError(signal);
  let response;
  try {
    response = await fetchImpl(url, {
      method: "POST",
      headers: { "content-type": "application/json", "x-goog-api-key": config.geminiApiKey },
      body: JSON.stringify(body),
      signal,
    });
  } catch {
    if (signal?.aborted) throw abortError(signal);
    throw new ServiceError(`GEMINI_${capability}_NETWORK`, `Không thể kết nối Gemini ${capability}.`);
  }
  if (!response.ok) {
    await response.body?.cancel().catch(() => {});
    throw geminiHttpError(response.status, capability);
  }
  const raw = await readLimited(response, responseLimit, signal);
  try { return JSON.parse(raw); } catch {
    throw new ServiceError(`GEMINI_${capability}_JSON`, `Gemini ${capability} trả về JSON không hợp lệ.`);
  }
}

export async function transcribeGemini(wav, config, options = {}) {
  const body = {
    contents: [{ role: "user", parts: [
      { inlineData: { mimeType: "audio/wav", data: wav.toString("base64") } },
      { text: "Chỉ chép nguyên văn lời nói tiếng Việt trong audio. Trả đúng JSON {\"transcript\":\"...\"}; không trả lời câu hỏi, không tạo action, không thêm diễn giải." },
   ] }],
    generationConfig: {
      temperature: 0,
      maxOutputTokens: 128,
      responseMimeType: "application/json",
    },
  };
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(config.geminiSttModel)}:generateContent`;
  const payload = await geminiFetch(url, body, config, options, 256 * 1024, "STT");
  let value;
  try { value = JSON.parse(responseText(payload)); } catch (error) {
    if (error instanceof ServiceError) throw error;
    throw new ServiceError("GEMINI_STT_JSON", "Gemini STT trả về transcript không hợp lệ.");
  }
  if (!value || typeof value !== "object" || Array.isArray(value) ||
      Object.keys(value).some((key) => key !== "transcript") || typeof value.transcript !== "string") {
    throw new ServiceError("GEMINI_STT_SCHEMA", "Gemini STT trả về dữ liệu không đúng schema.");
  }
  const transcript = safeTruncateUtf8(value.transcript, config.maxTextBytes);
  if (!transcript) throw new ServiceError("GEMINI_STT_EMPTY", "Không nhận dạng được lời nói trong bản thu.", 422);
  try { return boundedText(transcript, config.maxTextBytes); } catch {
    throw new ServiceError("GEMINI_STT_SCHEMA", "Transcript không hợp lệ.");
  }
}

function findAudio(value) {
  if (!value || typeof value !== "object") return null;
  if (typeof value.data === "string" &&
      (value.type === "audio" || String(value.mime_type || value.mimeType || "").startsWith("audio/"))) return value.data;
  for (const child of Object.values(value)) {
    if (Array.isArray(child)) {
      for (const item of child) { const found = findAudio(item); if (found) return found; }
    } else if (child && typeof child === "object") {
      const found = findAudio(child); if (found) return found;
    }
  }
  return null;
}

export async function ttsGemini(text, config, options = {}) {
  const input = boundedText(text, config.maxTextBytes);
  const body = {
    model: config.geminiTtsModel,
    input: `Đọc tự nhiên bằng tiếng Việt: ${input}`,
    response_format: { type: "audio" },
    generation_config: { speech_config: [{ voice: config.geminiTtsVoice }] },
  };
  const provider = await geminiFetch(
    "https://generativelanguage.googleapis.com/v1beta/interactions",
    body, config, options, 3 * 1024 * 1024, "TTS");
  const encoded = findAudio(provider);
  if (!encoded) throw new ServiceError("GEMINI_TTS_EMPTY", "Gemini TTS không trả về âm thanh.");
  const pcm24k = Buffer.from(encoded, "base64");
  if (!pcm24k.length || (pcm24k.length & 1) || pcm24k.length > 3 * 1024 * 1024) {
    throw new ServiceError("GEMINI_TTS_AUDIO", "Gemini TTS trả về PCM không hợp lệ.");
  }
  return wavFromPcm16(resamplePcm16Mono(pcm24k, 24000, 16000), 16000);
}
