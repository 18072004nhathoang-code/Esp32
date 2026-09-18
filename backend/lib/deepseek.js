import { abortError, ServiceError } from "./errors.js";
import { boundedText, safeTruncateUtf8, sanitizeActions } from "./protocol.js";

const RESPONSE_LIMIT = 64 * 1024;

function providerError(status) {
  if (status === 401) return new ServiceError("DEEPSEEK_AUTH", "DeepSeek từ chối API key.", 502, 401);
  if (status === 402) return new ServiceError("DEEPSEEK_BALANCE", "Tài khoản DeepSeek không đủ số dư.", 502, 402);
  if (status === 429) return new ServiceError("DEEPSEEK_RATE_LIMIT", "DeepSeek đang giới hạn tần suất yêu cầu.", 503, 429);
  if (status >= 500) return new ServiceError("DEEPSEEK_UNAVAILABLE", "DeepSeek đang tạm thời không khả dụng.", 503, status);
  return new ServiceError("DEEPSEEK_REQUEST", `DeepSeek từ chối yêu cầu (${status}).`, 502, status);
}

async function readLimited(response, limit, signal) {
  const declared = Number(response.headers.get("content-length"));
  if (Number.isFinite(declared) && declared > limit) {
    await response.body?.cancel();
    throw new ServiceError("DEEPSEEK_RESPONSE_TOO_LARGE", "Phản hồi DeepSeek vượt giới hạn.");
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
      if (size > limit) throw new ServiceError("DEEPSEEK_RESPONSE_TOO_LARGE", "Phản hồi DeepSeek vượt giới hạn.");
      chunks.push(Buffer.from(value));
    }
  } finally {
    if (size > limit || signal?.aborted) await reader.cancel().catch(() => {});
  }
  return Buffer.concat(chunks, size).toString("utf8");
}

function abortableDelay(milliseconds, signal) {
  if (milliseconds <= 0) return Promise.resolve();
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(abortError(signal));
    const timer = setTimeout(done, milliseconds);
    function done() {
      signal?.removeEventListener("abort", cancelled);
      resolve();
    }
    function cancelled() {
      clearTimeout(timer);
      reject(abortError(signal));
    }
    signal?.addEventListener("abort", cancelled, { once: true });
  });
}

function parseCompletion(payload, config) {
  const choice = payload?.choices?.[0];
  if (!choice || typeof choice !== "object") {
    throw new ServiceError("DEEPSEEK_SCHEMA", "DeepSeek trả về cấu trúc không hợp lệ.");
  }
  if (choice.finish_reason === "length") {
    throw new ServiceError("DEEPSEEK_TRUNCATED", "Phản hồi DeepSeek bị cắt do vượt giới hạn token.");
  }
  if (choice.finish_reason !== "stop") {
    throw new ServiceError("DEEPSEEK_INCOMPLETE", `DeepSeek dừng bất thường (${choice.finish_reason || "unknown"}).`);
  }
  const content = choice.message?.content;
  if (typeof content !== "string" || !content.trim()) {
    throw new ServiceError("DEEPSEEK_EMPTY", "DeepSeek trả về nội dung rỗng.");
  }
  let value;
  try { value = JSON.parse(content); } catch {
    throw new ServiceError("DEEPSEEK_JSON", "DeepSeek trả về JSON không hợp lệ.");
  }
  const allowedKeys = new Set(["reply", "needs_current_info", "actions"]);
  if (!value || typeof value !== "object" || Array.isArray(value) ||
      Object.keys(value).some((key) => !allowedKeys.has(key)) ||
      typeof value.reply !== "string" || typeof value.needs_current_info !== "boolean" ||
      !Array.isArray(value.actions)) {
    throw new ServiceError("DEEPSEEK_SCHEMA", "DeepSeek trả về dữ liệu không đúng schema.");
  }
  let reply;
  try {
    reply = boundedText(safeTruncateUtf8(value.reply, config.maxTextBytes), config.maxTextBytes);
  } catch {
    throw new ServiceError("DEEPSEEK_SCHEMA", "Câu trả lời DeepSeek không hợp lệ.");
  }
  let actions;
  try { actions = sanitizeActions(value.actions, config.musicSources); } catch {
    throw new ServiceError("DEEPSEEK_ACTION_REJECTED", "DeepSeek tạo action ngoài danh sách cho phép.");
  }
  if (value.needs_current_info) {
    reply = "Tôi chưa thể tra cứu thông tin mới vì gateway chưa cấu hình dịch vụ tìm kiếm.";
    actions = [];
  }
  return { reply, actions, sources: [] };
}

export async function queryDeepSeek(transcript, config, options = {}) {
  const fetchImpl = options.fetchImpl || fetch;
  const signal = options.signal;
  const sourceSummary = config.musicSources.map(({ id, label }) => ({ id, label }));
  const system = [
    "Bạn là trợ lý tiếng Việt trên ESP32. Chỉ trả về một JSON object hợp lệ, không markdown.",
    "Trả lời ngắn gọn, tự nhiên, tối đa một câu để đọc thành tiếng.",
    "Không tự nhận là đã tra cứu Internet. Nếu câu hỏi cần dữ liệu mới/thời gian thực, đặt needs_current_info=true và không bịa nguồn.",
    "Chỉ tạo action khi người dùng yêu cầu rõ ràng. Action cho phép: music.play, music.pause, music.resume, music.stop, music.volume.",
    "music.volume dùng value nguyên 0..100. music.play chỉ được dùng source_id trong danh sách cấu hình, tuyệt đối không tạo URL.",
    `Nguồn nhạc cấu hình: ${JSON.stringify(sourceSummary)}.`,
    "Schema JSON bắt buộc và không thêm trường: {\"reply\":\"...\",\"needs_current_info\":false,\"actions\":[]}",
  ].join("\n");
  const body = {
    model: config.deepseekModel,
    messages: [
      { role: "system", content: system },
      { role: "user", content: boundedText(transcript, config.maxTextBytes) },
    ],
    thinking: { type: config.deepseekThinking ? "enabled" : "disabled" },
    response_format: { type: "json_object" },
    max_tokens: config.deepseekMaxOutputTokens,
  };
  const url = `${config.deepseekBaseUrl}/chat/completions`;
  let lastError;
  for (let attempt = 0; attempt <= config.deepseekRetries; attempt++) {
    if (signal?.aborted) throw abortError(signal);
    let response;
    try {
      response = await fetchImpl(url, {
        method: "POST",
        headers: {
          "content-type": "application/json",
          authorization: `Bearer ${config.deepseekApiKey}`,
        },
        body: JSON.stringify(body),
        signal,
      });
    } catch (error) {
      if (signal?.aborted) throw abortError(signal);
      throw new ServiceError("DEEPSEEK_NETWORK", "Không thể kết nối DeepSeek.", 502);
    }
    if (response.ok) {
      const raw = await readLimited(response, RESPONSE_LIMIT, signal);
      let payload;
      try { payload = JSON.parse(raw); } catch {
        throw new ServiceError("DEEPSEEK_JSON", "DeepSeek trả về JSON HTTP không hợp lệ.");
      }
      return parseCompletion(payload, config);
    }
    await response.body?.cancel().catch(() => {});
    lastError = providerError(response.status);
    const retryable = response.status === 429 || response.status >= 500;
    if (!retryable || attempt === config.deepseekRetries) throw lastError;
    await abortableDelay(config.deepseekRetryBaseMs * (2 ** attempt), signal);
  }
  throw lastError;
}
