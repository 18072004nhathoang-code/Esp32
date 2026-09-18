import assert from "node:assert/strict";
import { afterEach, test } from "node:test";
import { createServer, configFromEnv } from "../server.js";
import { parseMusicSources, parsePcm16Mono16kWav, resamplePcm16Mono, sanitizeActions, wavFromPcm16 } from "../lib/protocol.js";

const token = "0123456789abcdef0123456789abcdef";
const config = configFromEnv({
  GEMINI_API_KEY: "provider-secret",
  ESP_DEVICE_TOKEN: token,
  MUSIC_SOURCES_JSON: '[{"id":"news","label":"News","url":"https://example.com/live.mp3"}]',
});
const servers = [];
afterEach(async () => { while (servers.length) await new Promise((resolve) => servers.pop().close(resolve)); });

async function start(deps) {
  const server = createServer(config, deps);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  servers.push(server);
  return `http://127.0.0.1:${server.address().port}`;
}

function sampleWav() { return wavFromPcm16(Buffer.alloc(1600 * 2), 16000); }

test("protocol validates WAV and configured action allowlist", () => {
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
  const pcm = Buffer.alloc(24000 * 2); for (let i = 0; i < 24000; i++) pcm.writeInt16LE(i % 1000, i * 2);
  const converted = resamplePcm16Mono(pcm, 24000, 16000);
  assert.equal(converted.length, 32000);
  assert.equal(parsePcm16Mono16kWav(wavFromPcm16(converted)).length, 32000);
});

test("query requires token and returns the synchronized response", async () => {
  const expected = { transcript: "mở radio", reply: "Đang gửi lệnh.", actions: [{ type: "music.play", source_id: "news" }], sources: [] };
  const base = await start({ query: async () => expected });
  const denied = await fetch(`${base}/v1/query`, { method: "POST", headers: { "content-type": "audio/wav" }, body: sampleWav() });
  assert.equal(denied.status, 401);
  const response = await fetch(`${base}/v1/query`, { method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav() });
  assert.equal(response.status, 200); assert.deepEqual(await response.json(), expected);
});

test("network/provider failure is explicit and does not leak secrets", async () => {
  const base = await start({ query: async () => { throw new Error("provider-secret detail"); } });
  const response = await fetch(`${base}/v1/query`, { method: "POST", headers: { "content-type": "audio/wav", authorization: `Bearer ${token}` }, body: sampleWav() });
  assert.equal(response.status, 502); assert.deepEqual(await response.json(), { error: "AI request failed" });
});

test("TTS response is bounded protocol WAV", async () => {
  const output = sampleWav(); const base = await start({ tts: async () => output });
  const response = await fetch(`${base}/v1/tts`, { method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` }, body: JSON.stringify({ text: "xin chào" }) });
  assert.equal(response.status, 200); assert.equal(response.headers.get("content-type"), "audio/wav");
  assert.equal(Buffer.from(await response.arrayBuffer()).length, output.length);
});
