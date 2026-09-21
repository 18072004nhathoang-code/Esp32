const ACTIONS = new Set([
  "music.play",
  "music.pause",
  "music.resume",
  "music.stop",
  "music.volume",
]);

export function utf8Bytes(value) {
  return Buffer.byteLength(value, "utf8");
}

export function boundedText(value, maxBytes, required = true) {
  if (typeof value !== "string") throw new Error("invalid text field");
  const text = value.trim();
  if ((required && !text) || utf8Bytes(text) > maxBytes) {
    throw new Error("text field exceeds protocol limit");
  }
  return text;
}

export function safeTruncateUtf8(value, maxBytes) {
  if (typeof value !== "string") return "";
  const source = Buffer.from(value, "utf8");
  if (source.length <= maxBytes) return value.trim();
  return source.subarray(0, maxBytes).toString("utf8").replace(/\uFFFD+$/u, "").trim();
}

export function parseMusicSources(raw) {
  let value;
  try { value = JSON.parse(raw || "[]"); } catch { throw new Error("MUSIC_SOURCES_JSON is invalid JSON"); }
  if (!Array.isArray(value) || value.length > 16) throw new Error("MUSIC_SOURCES_JSON must be an array of at most 16 items");
  const ids = new Set();
  return value.map((entry) => {
    if (!entry || typeof entry !== "object") throw new Error("invalid music source");
    const id = boundedText(entry.id, 31);
    const label = boundedText(entry.label, 63);
    const url = boundedText(entry.url, 255);
    if (!/^[a-z0-9][a-z0-9_-]*$/i.test(id) || ids.has(id)) throw new Error("music source id is invalid or duplicated");
    const parsed = new URL(url);
    if (parsed.protocol !== "https:") throw new Error("music source URL must use HTTPS");
    ids.add(id);
    return Object.freeze({ id, label, url });
  });
}

export function sanitizeActions(value, configuredSources) {
  if (value == null) return [];
  if (!Array.isArray(value) || value.length > 2) throw new Error("invalid actions array");
  const sourceIds = new Set(configuredSources.map((source) => source.id));
  return value.map((entry) => {
    if (!entry || typeof entry !== "object" || !ACTIONS.has(entry.type)) throw new Error("action is not allowed");
    const keys = Object.keys(entry);
    if (entry.type === "music.volume") {
      if (keys.some((key) => key !== "type" && key !== "value") ||
          !Number.isInteger(entry.value) || entry.value < 0 || entry.value > 100) {
        throw new Error("invalid volume action");
      }
      return { type: entry.type, value: entry.value };
    }
    if (entry.type === "music.play") {
      if (keys.some((key) => key !== "type" && key !== "source_id" && key !== "query")) throw new Error("invalid play action");
      if (entry.source_id != null && (!sourceIds.has(entry.source_id) || utf8Bytes(entry.source_id) > 31)) {
        throw new Error("unknown music source");
      }
      if (entry.query != null && (typeof entry.query !== "string" || utf8Bytes(entry.query) > 63)) {
        throw new Error("invalid music query");
      }
      const res = { type: entry.type };
      if (entry.source_id) res.source_id = entry.source_id;
      if (entry.query) res.query = entry.query;
      return res;
    }
    if (keys.some((key) => key !== "type")) throw new Error("invalid music action fields");
    return { type: entry.type };
  });
}

export function parsePcm16Mono16kWav(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < 44 || buffer.toString("ascii", 0, 4) !== "RIFF" ||
      buffer.toString("ascii", 8, 12) !== "WAVE") throw new Error("audio must be a RIFF/WAV file");
  let offset = 12;
  let format;
  let pcm;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString("ascii", offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const start = offset + 8;
    const end = start + size;
    if (end > buffer.length) throw new Error("truncated WAV chunk");
    if (id === "fmt ") {
      if (size < 16) throw new Error("invalid WAV format chunk");
      format = {
        encoding: buffer.readUInt16LE(start),
        channels: buffer.readUInt16LE(start + 2),
        rate: buffer.readUInt32LE(start + 4),
        bits: buffer.readUInt16LE(start + 14),
      };
    } else if (id === "data") {
      pcm = buffer.subarray(start, end);
    }
    offset = end + (size & 1);
  }
  if (!format || !pcm || format.encoding !== 1 || format.channels !== 1 ||
      format.rate !== 16000 || format.bits !== 16 || pcm.length === 0 || (pcm.length & 1)) {
    throw new Error("WAV must be PCM16 mono 16 kHz");
  }
  return pcm;
}

export function wavFromPcm16(pcm, sampleRate = 16000) {
  if (!Buffer.isBuffer(pcm) || (pcm.length & 1)) throw new Error("invalid PCM16 data");
  const wav = Buffer.allocUnsafe(44 + pcm.length);
  wav.write("RIFF", 0); wav.writeUInt32LE(36 + pcm.length, 4); wav.write("WAVEfmt ", 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(sampleRate, 24); wav.writeUInt32LE(sampleRate * 2, 28);
  wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34); wav.write("data", 36);
  wav.writeUInt32LE(pcm.length, 40); pcm.copy(wav, 44);
  return wav;
}

export function resamplePcm16Mono(pcm, fromRate, toRate) {
  if (!Buffer.isBuffer(pcm) || (pcm.length & 1) || fromRate <= 0 || toRate <= 0) throw new Error("invalid PCM input");
  if (fromRate === toRate) return Buffer.from(pcm);
  const inputCount = pcm.length / 2;
  const outputCount = Math.floor(inputCount * toRate / fromRate);
  const output = Buffer.allocUnsafe(outputCount * 2);
  for (let i = 0; i < outputCount; i++) {
    const position = i * fromRate / toRate;
    const left = Math.floor(position);
    const right = Math.min(left + 1, inputCount - 1);
    const fraction = position - left;
    const sample = Math.round(pcm.readInt16LE(left * 2) * (1 - fraction) + pcm.readInt16LE(right * 2) * fraction);
    output.writeInt16LE(Math.max(-32768, Math.min(32767, sample)), i * 2);
  }
  return output;
}
