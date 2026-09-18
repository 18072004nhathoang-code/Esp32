import { boundedText, resamplePcm16Mono, sanitizeActions, wavFromPcm16 } from "./protocol.js";

function extractJson(text) {
  if (typeof text !== "string") throw new Error("Gemini returned no text");
  const cleaned = text.trim().replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/, "");
  const start = cleaned.indexOf("{");
  const end = cleaned.lastIndexOf("}");
  if (start < 0 || end < start) throw new Error("Gemini response is not JSON");
  return JSON.parse(cleaned.slice(start, end + 1));
}

function responseText(payload) {
  const parts = payload?.candidates?.[0]?.content?.parts;
  if (!Array.isArray(parts)) throw new Error("Gemini response has no candidate");
  return parts.map((part) => typeof part.text === "string" ? part.text : "").join("").trim();
}

function citations(payload) {
  const chunks = payload?.candidates?.[0]?.groundingMetadata?.groundingChunks;
  if (!Array.isArray(chunks)) return [];
  const seen = new Set();
  const result = [];
  for (const chunk of chunks) {
    const title = chunk?.web?.title;
    const url = chunk?.web?.uri;
    if (typeof title !== "string" || typeof url !== "string" || seen.has(url)) continue;
    let parsed;
    try { parsed = new URL(url); } catch { continue; }
    if (parsed.protocol !== "https:" && parsed.protocol !== "http:") continue;
    seen.add(url);
    result.push({ title: title.slice(0, 80), url: url.slice(0, 240) });
    if (result.length === 3) break;
  }
  return result;
}

function safeTruncateUtf8(str, maxBytes) {
  if (typeof str !== "string") return "";
  let buf = Buffer.from(str, "utf8");
  if (buf.length <= maxBytes) return str;
  buf = buf.subarray(0, maxBytes);
  return buf.toString("utf8").replace(/\uFFFD*$/, "").trim();
}

async function geminiFetch(url, body, config, fetchImpl) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), config.timeoutMs);
  try {
    const response = await fetchImpl(url, {
      method: "POST",
      headers: { "content-type": "application/json", "x-goog-api-key": config.apiKey },
      body: JSON.stringify(body),
      signal: controller.signal,
    });
    const raw = await response.text();
    if (!response.ok) throw new Error(`Gemini HTTP ${response.status}`);
    if (Buffer.byteLength(raw) > 2 * 1024 * 1024) throw new Error("Gemini response too large");
    return JSON.parse(raw);
  } finally {
    clearTimeout(timer);
  }
}

export async function queryGemini(wav, config, fetchImpl = fetch) {
  const sourceSummary = config.musicSources.map(({ id, label }) => ({ id, label }));
  const prompt = [
    "Bạn là trợ lý tiếng Việt trên ESP32. Hãy nghe âm thanh, chép lại chính xác và trả lời cực kỳ ngắn gọn (tối đa 1-2 câu ngắn) để phản hồi nhanh nhất.",
    "Chỉ phát lệnh nhạc khi người dùng yêu cầu rõ ràng. Action cho phép: music.play, music.pause, music.resume, music.stop, music.volume.",
    "music.play không source_id nghĩa là bài SD đang chọn/đầu tiên. Chỉ dùng source_id có trong danh sách cấu hình; không tạo URL.",
    `Nguồn stream cấu hình: ${JSON.stringify(sourceSummary)}.`,
    "Không nói lệnh đã thành công vì thiết bị sẽ tự xác nhận. Trả đúng một JSON, không markdown:",
    '{"transcript":"...","reply":"...","needs_current_info":false,"actions":[]}',
  ].join("\n");
  const body = {
    contents: [{ role: "user", parts: [
      { inlineData: { mimeType: "audio/wav", data: wav.toString("base64") } },
      { text: prompt },
    ] }],
    generationConfig: { temperature: 0.1, maxOutputTokens: 256 },
  };
  if (config.enableSearch) {
    body.tools = [{ google_search: {} }];
  }
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(config.queryModel)}:generateContent`;
  let provider;
  let searchUsed = !!config.enableSearch;
  try {
    provider = await geminiFetch(url, body, config, fetchImpl);
  } catch (error) {
    if (body.tools) {
      delete body.tools;
      searchUsed = false;
      provider = await geminiFetch(url, body, config, fetchImpl);
    } else {
      throw error;
    }
  }
  const parsed = extractJson(responseText(provider));
  const sources = citations(provider);
  const rawTranscript = typeof parsed.transcript === "string" ? parsed.transcript : "";
  const rawReply = typeof parsed.reply === "string" ? parsed.reply : "";
  const transcript = boundedText(safeTruncateUtf8(rawTranscript, config.maxTextBytes) || "...", config.maxTextBytes);
  const reply = boundedText(safeTruncateUtf8(rawReply, config.maxTextBytes) || "Tôi đã nghe bạn.", config.maxTextBytes);
  const actions = sanitizeActions(parsed.actions, config.musicSources);
  if (searchUsed && parsed.needs_current_info === true && sources.length === 0) {
    throw new Error("current-information answer was not grounded by Google Search");
  }
  return { transcript, reply, actions, sources };
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

export async function ttsGemini(text, config, fetchImpl = fetch) {
  const input = boundedText(text, config.maxTextBytes);
  const body = {
    model: config.ttsModel,
    input: `Đọc tự nhiên bằng tiếng Việt: ${input}`,
    response_format: { type: "audio" },
    generation_config: { speech_config: [{ voice: config.ttsVoice }] },
  };
  const provider = await geminiFetch("https://generativelanguage.googleapis.com/v1beta/interactions", body, config, fetchImpl);
  const encoded = findAudio(provider);
  if (!encoded) throw new Error("Gemini TTS returned no audio");
  const pcm24k = Buffer.from(encoded, "base64");
  if (!pcm24k.length || (pcm24k.length & 1) || pcm24k.length > 3 * 1024 * 1024) throw new Error("Gemini TTS returned invalid PCM");
  return wavFromPcm16(resamplePcm16Mono(pcm24k, 24000, 16000), 16000);
}
